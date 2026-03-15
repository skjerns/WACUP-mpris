Your task is to create a plugin for WACUP (which is installed via wine-11.0 32bit on this system) that enables native MPRIS integration.

The approach: a single `gen_mpris.dll` that runs inside WACUP and directly registers as an MPRIS2 player on the host's D-Bus session bus ? no external process.

**The core problem:** The DLL is a Windows PE binary running in WINE. It cannot directly call `libdbus` (a Linux ELF shared library). There are several possible bridges:

**Option A: WINE unixlib interface**
- WINE 7.0+ has `__wine_unix_call` for PE ? Unix bridging
- Requires building against WINE's internal headers and creating both a PE stub and a `.so` companion
- Tightly coupled to WINE version, undocumented/unstable ABI
- This is how `winepulse.drv` works internally

**Realistic assessment:** Options A and C are the only truly "plugin-only" paths. Option A is the most maintainable. Option C is the most self-contained but amounts to reimplementing a D-Bus client from scratch over raw Unix sockets.

Here's the plan for **Option A (WINE unixlib)**:

---

## Plan: `gen_mpris` ? WINE unixlib plugin

### Components

```
gen_mpris/
??? gen_mpris.c          # PE-side: Winamp gen_ plugin, IPC with WACUP
??? gen_mpris_unixlib.c  # Unix-side: D-Bus/MPRIS registration (linked against libdbus)
??? unixlib.h            # Shared command/struct definitions between PE and Unix sides
??? sdk/
?   ??? gen.h            # Winamp SDK
?   ??? wa_ipc.h         # Winamp IPC definitions
??? Makefile             # Cross-compile PE + native .so
??? README.md
```

### Data flow

```
KDE media key
  ? D-Bus method call on org.mpris.MediaPlayer2.wacup
  ? gen_mpris_unixlib.so (handles D-Bus, registered as MPRIS player)
  ? __wine_unix_call dispatch
  ? gen_mpris.dll (PE side, translates to SendMessage WM_WA_IPC)
  ? WACUP responds

WACUP state change (detected by PE-side polling timer)
  ? gen_mpris.dll packages state into shared struct
  ? __wine_unix_call to Unix side
  ? gen_mpris_unixlib.so emits PropertiesChanged on D-Bus
  ? KDE taskbar widget updates
```

### Phase 1: Scaffolding & build system

1. Obtain Winamp SDK headers (`gen.h`, `wa_ipc.h`)
2. Obtain WINE source headers for unixlib (`wine/unixlib.h`, `winternl.h`)
3. Set up Makefile:
   - PE side: `i686-w64-mingw32-gcc` ? `gen_mpris.dll`
   - Unix side: native `gcc` ? `gen_mpris_unixlib.so`, linked with `-ldbus-1`
   - Both produce outputs for the same WINE DLL pair

### Phase 2: Shared interface (`unixlib.h`)

Define the call table between PE ? Unix:

```c
enum mpris_unix_funcs {
    unix_init,           // Initialize D-Bus connection, register MPRIS bus name
    unix_shutdown,       // Release bus name, disconnect
    unix_update_state,   // PE ? Unix: push new playback state + metadata
    unix_poll_commands,  // PE ? Unix: check for pending MPRIS commands (play/pause/etc.)
    unix_process_dbus,   // PE ? Unix: pump the D-Bus event loop
};

struct mpris_state {
    int playback_status;   // 0=stopped, 1=playing, 3=paused
    int position_ms;
    int length_s;
    int volume;            // 0-255
    wchar_t title[512];
    wchar_t artist[256];
};

struct mpris_command {
    enum {
        CMD_NONE, CMD_PLAY, CMD_PAUSE, CMD_PLAYPAUSE,
        CMD_STOP, CMD_NEXT, CMD_PREV, CMD_SEEK, CMD_SETVOL,
    } type;
    int param;  // seek position or volume
};
```

### Phase 3: PE side (`gen_mpris.c`)

- `init()`: call `__wine_unix_call(unix_init)` to set up D-Bus
- Set a Windows timer (`SetTimer`) at ~250ms interval:
  - Gather state via `SendMessage(hwndWA, WM_WA_IPC, ...)` ? status, position, length, title, volume
  - Pack into `struct mpris_state`
  - Call `__wine_unix_call(unix_update_state, &state)`
  - Call `__wine_unix_call(unix_poll_commands, &cmd)`
  - If command received, dispatch via `SendMessage(hwndWA, WM_COMMAND, ...)`
  - Call `__wine_unix_call(unix_process_dbus)` to pump D-Bus
- `quit()`: call `__wine_unix_call(unix_shutdown)`, kill timer

### Phase 4: Unix side (`gen_mpris_unixlib.c`)

- `unix_init`: connect to session D-Bus, request name `org.mpris.MediaPlayer2.wacup`, register object `/org/mpris/MediaPlayer2`, set up vtables for both MPRIS interfaces
- `unix_update_state`: compare with previous state, if changed emit `org.freedesktop.DBus.Properties.PropertiesChanged` signals with updated `PlaybackStatus`, `Metadata`, `Position`, `Volume`
- `unix_poll_commands`: check if any D-Bus method calls arrived (Play, Pause, Next, etc.), return the next pending command
- `unix_process_dbus`: call `dbus_connection_read_write_dispatch()` (non-blocking)
- `unix_shutdown`: release bus name, close connection

### Phase 5: MPRIS2 interface implementation (Unix side)

Properties to expose:

**`org.mpris.MediaPlayer2`**: Identity="WACUP", CanQuit=true, CanRaise=false

**`org.mpris.MediaPlayer2.Player`**:
- `PlaybackStatus` ? mapped from IPC_ISPLAYING
- `Metadata` ? dict: `xesam:title`, `xesam:artist` (parsed from title string, Winamp uses "Artist - Title" format), `mpris:trackid`, `mpris:length`
- `Position` ? microseconds (convert from ms)
- `Volume` ? 0.0?1.0 (convert from 0?255)
- `CanGoNext`, `CanGoPrevious`, `CanPlay`, `CanPause`, `CanSeek`, `CanControl` ? all true
- `Rate` = 1.0, `MinimumRate` = 1.0, `MaximumRate` = 1.0

Methods: `Play`, `Pause`, `PlayPause`, `Stop`, `Next`, `Previous`, `Seek`, `SetPosition` ? each queues a command for PE-side pickup.

### Phase 6: Build & install

```bash
make                    # produces gen_mpris.dll and gen_mpris_unixlib.so
cp gen_mpris.dll ~/.wine/drive_c/Program\ Files/WACUP/Plugins/
cp gen_mpris_unixlib.so /usr/lib/i386-linux-gnu/wine/  # or appropriate WINE lib path
```

### Known risks / brittleness

- **WINE version coupling**: `__wine_unix_call` ABI changed between WINE 7.x and 8.x and again in 9.x. The `.so` must be compiled against the same WINE version headers as the installed WINE.
- **32-bit requirement**: WACUP is x86, so the `.so` must also be 32-bit (`-m32`), requiring `libdbus-1-dev:i386` and 32-bit WINE development headers.
- **DLL loading path**: WINE needs to find the companion `.so` ? this depends on the WINE prefix and lib search path configuration.
- **D-Bus session bus discovery**: the Unix side needs `$DBUS_SESSION_BUS_ADDRESS`, which should be inherited from the WINE process environment but may not always be.
- **Thread safety**: D-Bus calls happen on the WACUP main thread (via timer callback ? unix_call). This is single-threaded and avoids race conditions but means D-Bus responsiveness depends on the timer interval.

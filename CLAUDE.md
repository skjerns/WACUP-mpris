Your task is to create a plugin for WACUP (which is installed via wine-11.0 32bit on this system) that enables native MPRIS integration.

**The core problem:** The DLL is a Windows PE binary running in Wine. It cannot directly call `libdbus` (a Linux ELF shared library). Three approaches were evaluated:

---

## Option A: Wine unixlib (`__wine_unix_call`)

**Status: Investigated — architecturally blocked for WACUP plugins.**

Wine 7.0+ has `__wine_unix_call` for PE → Unix bridging. The PE DLL calls into a companion `.so` that can use native Linux libraries (libdbus, libX11, etc.). This is how `winepulse.drv`, `winewayland.drv` etc. work internally.

### Requirements

- Build with `winegcc` (not plain `i686-w64-mingw32-gcc`), using a `.spec` file and `winebuild --builtin`
- PE DLL goes in `lib/wine/i386-windows/`, companion `.so` in `lib/wine/i386-unix/`
- DLL must have the "Wine builtin DLL" signature — only then does Wine load the companion `.so` and register the unixlib function table
- Need `libdbus-1-dev:i386` for the 32-bit `.so`
- Must bundle `wine/unixlib.h` headers (ABI not stable across Wine versions)

### Why it doesn't work for us

WACUP loads plugins via `LoadLibrary("C:\...\Plugins\gen_mpris.dll")` with a full path. Wine treats files on the virtual C: drive as "native" Windows DLLs, **not builtins**. A native DLL never gets its companion `.so` loaded and `__wine_unix_call` will fail.

Workarounds:
- `WINEDLLOVERRIDES="gen_mpris=b"` to force builtin loading — fragile, requires user config
- Stub DLL in Plugins/ that `LoadLibrary("gen_mpris")` by bare name — WACUP may not support this

### Third-party example

[wine-nvml](https://github.com/Saancreed/wine-nvml) — wraps NVIDIA's `libnvidia-ml.so` for Wine. Uses `winegcc`, `.spec` files, bundled Wine headers. Requires Wine >= 9.0.

Wine 11.0 release notes mention installing `wine/unixlib.h` "as a first step towards supporting use of the Unixlib interface in third-party modules" — still a work in progress.

### ABI stability

The `__wine_unix_call` ABI changed between Wine 7/8/9. The `.so` must be compiled against the same Wine version's headers as the installed Wine. Not suitable for distributable plugins.

---

## Option B: External helper process over TCP (current implementation)

**Status: Implemented and working.**

A native Linux process (`gen_mpris_host`) handles D-Bus/MPRIS2 registration and X11 window tagging. The PE DLL communicates with it over TCP (localhost:39158).

### Pros
- Simple, reliable, works with any Wine version
- DLL is a plain MinGW PE — no Wine internals dependency
- Host can use any native Linux library (libdbus, libX11)
- Clean separation of concerns

### Cons
- Extra process to manage (auto-launched by DLL, but can crash independently)
- TCP overhead (negligible in practice — state updates are tiny)
- Port collision risk (mitigated by using a fixed unusual port)

### Data flow
```
WACUP → gen_mpris.dll (PE, polls state via WM_WA_IPC)
       → TCP localhost:39158
       → gen_mpris_host (native ELF, D-Bus + X11)
       → D-Bus session bus (MPRIS2)
       → KDE/desktop media controls
```

---

## Option C: Raw D-Bus wire protocol over Unix sockets

**Status: Not yet implemented. Most promising "single-binary" alternative.**

Wine maps Unix domain sockets through its socket emulation, so the PE DLL can `connect()` to `$DBUS_SESSION_BUS_ADDRESS` directly and speak the D-Bus wire protocol. No libdbus needed — pure C socket code compiled with MinGW.

### Existing D-Bus libraries evaluated

- **udbus** (github.com/vincenthz/udbus) — ~900 lines C, implements wire protocol and auth, but client-only. Cannot register a bus name or act as a service. Would need significant extension.
- **adbus** (github.com/jmckaskill/adbus) — complete C/C++ implementation, full service side, claims mingw compatibility, but partially C++ and archived since 2023.

**Conclusion:** No ready-made solution exists. Option C requires implementing the D-Bus wire protocol from scratch, or heavily extending udbus.

### What changes

- `gen_mpris.c` gains a D-Bus implementation directly
- `gen_mpris_host.c` and TCP IPC become obsolete
- `ipc_protocol.h` becomes obsolete
- X11 window tagging needs a separate solution (see Phase 5)

### Pros
- No external process
- No Wine version coupling
- Completely self-contained in `gen_mpris.dll`
- Works with plain MinGW cross-compilation

### Cons
- Must reimplement D-Bus wire protocol (SASL auth, binary message marshaling, alignment, array/dict encoding)
- Estimated ~1000-1500 lines of D-Bus protocol code
- No fd passing support (not needed for MPRIS)
- Must handle `$DBUS_SESSION_BUS_ADDRESS` parsing (Unix socket path extraction)

### Phase 1 — Transport & Auth (~100 lines)

Wine 3.18+ supports `AF_UNIX` in Winsock, so the DLL can connect directly to the D-Bus session socket.

```c
// 1. GetEnvironmentVariableA("DBUS_SESSION_BUS_ADDRESS", ...)
//    → parse "unix:path=/run/user/1000/bus"
//      or   "unix:abstract=/tmp/dbus-XXXX"
// 2. socket(AF_UNIX, SOCK_STREAM, 0) + connect()
// 3. Auth handshake:
//      send: \0AUTH EXTERNAL\r\n
//      recv: OK <guid>\r\n
//      send: BEGIN\r\n
```

`AUTH EXTERNAL` without a UID argument relies on `SO_PEERCRED` — dbus-daemon checks credentials from the kernel, which works because wacup.exe is a real Linux process.

### Phase 2 — Message serialization (~400 lines)

D-Bus uses a binary wire format. Need a small builder:

| Message                          | Direction | Complexity |
|----------------------------------|-----------|------------|
| `Hello` call                     | outbound  | simple     |
| `RequestName(name, flags)` call  | outbound  | simple     |
| Method return (empty body)       | outbound  | simple     |
| `PropertiesChanged` signal       | outbound  | hard       |
| `GetAll` reply (`a{sv}` body)    | outbound  | hard       |

The hard part is marshaling `a{sv}` (array of string→variant dict entries) with correct 8-byte alignment padding. This is essentially reimplementing libdbus's DBusMessageIter.

D-Bus header layout (16 bytes fixed):

```
byte  endianness  ('l' = little-endian)
byte  message type (1=call, 2=return, 3=error, 4=signal)
byte  flags
byte  protocol version (1)
u32   body length
u32   serial
u32   header fields array length
... header fields (type byte + variant, padded to 8-byte boundary each)
... body (padded to 8-byte boundary from start of message)
```

### Phase 3 — Message parser (~300 lines)

Read incoming messages from the socket, extract:
- Header fields: serial, reply_serial, path, interface, member, sender
- Body arguments for incoming method calls:
  - `Seek` → int64 offset
  - `SetPosition` → object_path + int64
  - `Set` (volume) → string + string + variant(double)
  - `Play`, `Pause`, `Next`, etc. → no body

Non-blocking reads using `select()` with zero timeout, same pattern as current `ipc_recv_command()`.

### Phase 4 — Main loop integration (~200 lines)

Replace timer_proc's TCP calls with direct D-Bus I/O:

```
plugin init:
  → connect to D-Bus socket
  → auth handshake
  → send Hello, get unique name
  → send RequestName "org.mpris.MediaPlayer2.wacup"

each 250ms timer tick:
  → select(dbus_fd, timeout=0) — non-blocking check
  → read + dispatch any pending method calls → queue commands
  → gather WACUP state via SendMessage(WM_WA_IPC)
  → if state changed → marshal + send PropertiesChanged signal
  → dispatch queued commands via SendMessage(WM_COMMAND)

plugin quit:
  → send ReleaseName
  → closesocket
```

Static globals: socket fd, serial counter, unique D-Bus name, last known state.

### Phase 5 — X11 window tagging

The current `set_kde_desktop_file_hint()` uses Xlib (ELF). From the PE side, options:

1. **Keep a minimal host** for X11 tagging only — 10 lines, no D-Bus, fires once at startup. Much simpler than the current host.
2. **Shell out** via `CreateProcessA("/bin/sh", "sh -c \"xprop -root ...\"")` once at startup — no persistent process needed.
3. **Skip it** — `desktopFile` in KWin still works if `wacup.desktop` is installed in `~/.local/share/applications/`.

Option 2 is probably the right call: one-shot, no dependencies.

### Risk assessment

| Risk                                                    | Severity |
|---------------------------------------------------------|----------|
| `a{sv}` marshaling bugs (alignment, nested containers) | High     |
| 32-bit Wine Winsock `AF_UNIX` working correctly         | Medium   |
| dbus-daemon rejecting `EXTERNAL` auth without UID       | Low      |
| Blocking reads stalling WACUP main thread               | Low      |

The alignment rules are the biggest source of subtle bugs. Every struct/dict entry must start on an 8-byte boundary; every string on a 4-byte boundary; every int32 on 4 bytes. Off-by-one padding errors will cause dbus-daemon to silently drop or misparse messages.

**Total estimated new code:** ~1000–1500 lines of C replacing gen_mpris_host.c and the TCP layer in gen_mpris.c.

### References

- D-Bus specification: https://dbus.freedesktop.org/doc/dbus-specification.html
- udbus (wire protocol reference): https://github.com/vincenthz/udbus
- adbus (service-side reference): https://github.com/jmckaskill/adbus
- Wine winebth.sys (Option A reference): https://github.com/wine-mirror/wine/blob/master/dlls/winebth.sys/

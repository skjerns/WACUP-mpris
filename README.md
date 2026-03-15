# gen_mpris — MPRIS2 Integration for WACUP (Wine)

Exposes [WACUP](https://getwacup.com/) running under Wine as a native MPRIS2 media player on Linux. Desktop media controls, taskbar widgets (KDE, GNOME), and tools like `playerctl` can see and control WACUP playback.

## How it works

Two components communicate over TCP localhost:

- **gen_mpris.dll** — A Winamp general-purpose plugin loaded by WACUP. Polls playback state via Winamp IPC and relays media commands.
- **gen_mpris_host** — A native Linux process that registers on the D-Bus session bus as `org.mpris.MediaPlayer2.wacup` and bridges D-Bus method calls/signals to the DLL.

When WACUP loads the plugin, it automatically launches the host process. No manual setup is needed after installation.

## Prerequisites

### Build dependencies

**Debian/Ubuntu:**

```bash
sudo apt install gcc gcc-mingw-w64-i686 libdbus-1-dev pkg-config make
```

**Fedora:**

```bash
sudo dnf install gcc mingw32-gcc dbus-devel pkg-config make
```

**Arch Linux:**

```bash
sudo pacman -S gcc mingw-w64-gcc dbus pkg-config make
```

### Runtime dependencies

- Wine (any version that runs WACUP — typically Wine 7.0+)
- WACUP installed in a Wine prefix
- D-Bus session bus (present on any modern desktop Linux)
- `libdbus-1` (installed by default on most distros)

## Building

```bash
git clone https://github.com/skjerns/WACUP-mpris.git
cd WACUP-mpris
make
```

This produces:

- `gen_mpris.dll` — PE32 plugin for WACUP
- `gen_mpris_host` — native Linux binary

## Installation

### 1. Install the host binary

Copy `gen_mpris_host` somewhere on your `$PATH`:

```bash
cp gen_mpris_host ~/.local/bin/
```

Or system-wide:

```bash
sudo cp gen_mpris_host /usr/local/bin/
```

Make sure the target directory is in your `$PATH`. The plugin launches the host by name (`gen_mpris_host`), so it must be findable.

### 2. Install the plugin DLL

Copy `gen_mpris.dll` into WACUP's Plugins directory inside your Wine prefix:

```bash
cp gen_mpris.dll "$WINEPREFIX/drive_c/Program Files (x86)/WACUP/Plugins/"
```

If you use the default Wine prefix:

```bash
cp gen_mpris.dll "$HOME/.wine/drive_c/Program Files (x86)/WACUP/Plugins/"
```

The `Makefile` has convenience targets for both steps (edit `WACUP_PLUGINS` and `HOST_INSTALL_DIR` in the Makefile if your paths differ):

```bash
make install
```

### 3. Start WACUP

Launch WACUP normally through Wine. The plugin loads automatically and starts the host process.

## Verification

Check that the MPRIS2 bus name is registered:

```bash
busctl --user list | grep wacup
```

Query properties with `dbus-send`:

```bash
# Identity
dbus-send --session --print-reply \
  --dest=org.mpris.MediaPlayer2.wacup \
  /org/mpris/MediaPlayer2 \
  org.freedesktop.DBus.Properties.Get \
  string:'org.mpris.MediaPlayer2' string:'Identity'

# Playback status
dbus-send --session --print-reply \
  --dest=org.mpris.MediaPlayer2.wacup \
  /org/mpris/MediaPlayer2 \
  org.freedesktop.DBus.Properties.Get \
  string:'org.mpris.MediaPlayer2.Player' string:'PlaybackStatus'
```

Or with `playerctl` (install separately if not present):

```bash
playerctl -p wacup status
playerctl -p wacup metadata
playerctl -p wacup play-pause
playerctl -p wacup next
```

## Troubleshooting

**Host process doesn't start:**
Verify `gen_mpris_host` is on your `$PATH` and executable. You can test by running it manually in a terminal — it should print `listening on 127.0.0.1:39158` and `acquired bus name`.

**"Could not acquire bus name":**
Another instance of `gen_mpris_host` is already running. Kill it with `pkill gen_mpris_host` and restart WACUP.

**Port 39158 in use:**
Another process is using the TCP port. Check with `ss -tlnp | grep 39158`. If you need to change the port, edit `MPRIS_IPC_PORT` in `ipc_protocol.h` and rebuild both binaries.

**No D-Bus session bus:**
The host needs `$DBUS_SESSION_BUS_ADDRESS` to be set. This is standard on desktop sessions but may be missing in some minimal environments. Wine inherits the environment from the shell that launched it, so launch WACUP from a terminal or desktop shortcut where the session bus is available.

**Desktop widget doesn't update:**
WACUP state is polled every 250ms. If the widget appears stale, check that both `gen_mpris_host` and WACUP are running and connected (the host prints `client connected` to stderr when the DLL connects).

## License

Public domain. The Winamp SDK headers in `sdk/` are based on the public Winamp SDK.

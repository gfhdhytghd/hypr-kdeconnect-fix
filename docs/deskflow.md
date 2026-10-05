# Deskflow on Hyprland / Omarchy

Deskflow clients need the `org.freedesktop.portal.RemoteDesktop` interface to
receive keyboard and pointer input. An available `InputCapture` interface is
for sharing local input and does not provide the receiving side. This bridge
supplies RemoteDesktop on compositors that expose the virtual keyboard and
virtual pointer protocols.

## Build and install

Install Deskflow separately, then locate the actual `deskflow-core` executable.
Native Deskflow sessions may have an empty portal app id, so the bridge checks
the originating D-Bus process against `/usr/bin/deskflow-core` or an exact
configured executable path.

For a user-local Deskflow installation:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DHKCF_DESKFLOW_EXECUTABLE="$HOME/.local/bin/deskflow-core"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

The packaged `/usr/bin/deskflow-core` remains accepted. For other installation
locations, set `HKCF_DESKFLOW_EXECUTABLE` to the resolved absolute path. Its
default is `<CMAKE_INSTALL_PREFIX>/bin/deskflow-core`. If the binary is a
symlink, use its resolved target because authorization checks `/proc/<pid>/exe`.
Changing the installation path requires rebuilding with the new value. Source
builds under `deskflow-src/build/bin` also need their full path configured;
that suffix alone is no longer accepted.

## Portal routing

Back up the effective user portal configuration. Preserve its existing
providers and add only this entry under `[preferred]`:

```ini
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

Desktop-specific files, such as `hyprland-portals.conf`, can take precedence
over `portals.conf`. Check the configuration selected for the session's
`XDG_CURRENT_DESKTOP`. If creating a user override, copy the effective
configuration first so screen sharing and other interfaces retain their
existing providers.

Restart the services when no active screen-sharing session needs preserving:

```sh
systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal.service
systemctl --user restart hypr-kdeconnect-portal.service
busctl --user introspect org.freedesktop.portal.Desktop \
  /org/freedesktop/portal/desktop org.freedesktop.portal.RemoteDesktop
```

The public interface should expose `CreateSession`, `SelectDevices`, `Start`
and `ConnectToEIS`. Configure Deskflow as a client and connect it to your
Deskflow server. Keep TLS and peer verification enabled and compare the
server's fingerprint before trusting it. The server must include the client's
computer name in its layout and allow its TCP connection to the configured
Deskflow port (24800 by default).

## Validation and limits

The original patch was confirmed receiving Deskflow input on an Omarchy client
with Hyprland 0.56.2, libei 1.6.0, xdg-desktop-portal 1.22.1 and the Hyprland
portal 1.4.1. This revision builds on upstream's existing Deskflow and keyboard
modifier support. Automated tests use a real libei/libeis connection with a
recording input sink to check device discovery, pointer coordinates, wheel
units and direction, key/button delivery, and releases on emulation stop or
disconnect. The tests do not connect to a compositor; a live check of this
rebased revision remains pending.

The bridge provides input only. It does not expose the Clipboard portal or
enable file transfers. Multi-monitor and scaled-output layouts need further
live validation: the current backend uses the first advertised output.
Keyboard layout selection still follows the backend's default XKB keymap.

To roll back, stop `hypr-kdeconnect-portal.service`, restore the previous portal
configuration, reload user units, and restart `xdg-desktop-portal.service`.
The existing compositor portal and Deskflow installation can remain installed.

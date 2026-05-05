# hypr-kdeconnect-fix

User-level RemoteDesktop portal bridge for KDE Connect remote input on Hyprland.

KDE Connect already sends remote input through `org.freedesktop.portal.RemoteDesktop`
on Wayland. Hyprland's standard portal backend currently does not expose that
interface, so this service provides only that missing backend interface and
injects the resulting events through Hyprland's virtual keyboard and virtual
pointer Wayland protocols.

## Build and Install

Dependencies:

- Qt 6 Core/DBus
- `wayland-client`
- `wayland-scanner`
- `xkbcommon`
- CMake and a C++23 compiler

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake --install build
```

Then configure xdg-desktop-portal to use this backend for RemoteDesktop:

```ini
[preferred]
default=gtk
org.freedesktop.impl.portal.ScreenCast=hyprland
org.freedesktop.impl.portal.Screenshot=hyprland
org.freedesktop.impl.portal.GlobalShortcuts=hyprland
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

Restart the frontend after changing portal metadata:

```sh
systemctl --user restart xdg-desktop-portal
```

If an old D-Bus-activated bridge process is already running, restart it too:

```sh
pid="$(busctl --user status org.freedesktop.impl.portal.desktop.hypr_kdeconnect 2>/dev/null | awk -F= '/^PID=/{print $2; exit}')"
[ -n "$pid" ] && kill "$pid"
```

Quick checks:

```sh
busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop org.freedesktop.portal.RemoteDesktop
hypr-kdeconnect-portal --self-test-motion 120 0
hypr-kdeconnect-portal --self-test-absolute 1440 900
```

## Scope

Implemented:

- relative pointer motion
- absolute pointer motion
- left/middle/right button events
- smooth scrolling
- keyboard keycode and keysym input

Not implemented:

- libei `ConnectToEIS` transport
- touchscreen input
- InputCapture/share-input-devices edge capture

The bridge accepts KDE Connect app ids and Hyprland's `surface-transient`
fallback app id used by non-windowed local clients. Hyprland already exposes
the virtual keyboard and virtual pointer protocols to local Wayland clients, so
this bridge is intended as a compatibility shim rather than a permission UI.

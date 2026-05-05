# hypr-kdeconnect-fix

A small user-level `xdg-desktop-portal` RemoteDesktop backend that makes KDE
Connect remote input work on Hyprland.

KDE Connect already uses `org.freedesktop.portal.RemoteDesktop` for remote
mouse and keyboard input on Wayland. Hyprland's standard portal backend exposes
screenshots, screencast, and global shortcuts, but not RemoteDesktop input. This
bridge fills that one missing portal backend interface and injects events
through Hyprland's virtual keyboard and virtual pointer Wayland protocols.

This is a compatibility shim, not a Hyprland plugin.

## Status

Implemented:

- relative pointer motion
- absolute pointer motion
- pointer button events
- smooth and discrete scrolling
- keyboard keycode input
- keyboard keysym input through `xkbcommon`

Not implemented:

- libei `ConnectToEIS`
- touchscreen events
- InputCapture/share-input-devices edge capture
- a permission dialog

When KDE Connect tries `ConnectToEIS`, the bridge returns `NotSupported`. KDE
Connect then falls back to the RemoteDesktop `Notify*` methods, which this
bridge implements.

## Dependencies

Build-time:

- CMake
- a C++23 compiler
- Qt 6 Core/DBus
- `pkg-config`
- `wayland-client`
- `wayland-scanner`
- `xkbcommon`

Runtime:

- Hyprland exposing `zwlr_virtual_pointer_manager_v1`
- Hyprland exposing `zwp_virtual_keyboard_manager_v1`
- `xdg-desktop-portal`
- KDE Connect

On Arch-like systems the useful package set is roughly:

```sh
sudo pacman -S cmake gcc pkgconf qt6-base wayland libxkbcommon xdg-desktop-portal
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake --install build
```

This installs:

- `~/.local/bin/hypr-kdeconnect-portal`
- `~/.local/share/xdg-desktop-portal/portals/hypr-kdeconnect.portal`
- `~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service`

## Portal Configuration

Configure `xdg-desktop-portal` to route only RemoteDesktop to this backend, while
leaving the normal Hyprland portal in charge of screenshot, screencast, and
global shortcuts.

`~/.config/xdg-desktop-portal/portals.conf`:

```ini
[preferred]
default=gtk
org.freedesktop.impl.portal.ScreenCast=hyprland
org.freedesktop.impl.portal.Screenshot=hyprland
org.freedesktop.impl.portal.GlobalShortcuts=hyprland
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

Restart the portal frontend after changing metadata or `portals.conf`:

```sh
systemctl --user restart xdg-desktop-portal
```

If D-Bus already activated an older bridge process, kill it so the next request
starts the newly installed binary:

```sh
pid="$(busctl --user status org.freedesktop.impl.portal.desktop.hypr_kdeconnect 2>/dev/null | awk -F= '/^PID=/{print $2; exit}')"
[ -n "$pid" ] && kill "$pid"
```

## Verification

Check that the public portal frontend exposes RemoteDesktop:

```sh
busctl --user introspect \
  org.freedesktop.portal.Desktop \
  /org/freedesktop/portal/desktop \
  org.freedesktop.portal.RemoteDesktop
```

Check direct virtual pointer injection:

```sh
hypr-kdeconnect-portal --self-test-motion 120 0
hypr-kdeconnect-portal --self-test-absolute 1440 900
```

Check that Hyprland sees the virtual devices:

```sh
hyprctl devices | rg 'hypr-kdeconnect|virtual|unknown-device'
```

## How It Works

1. KDE Connect calls the public portal frontend at
   `org.freedesktop.portal.Desktop`.
2. `xdg-desktop-portal` reads `portals.conf` and forwards RemoteDesktop backend
   calls to `org.freedesktop.impl.portal.desktop.hypr_kdeconnect`.
3. This bridge accepts KDE Connect sessions, starts virtual input devices, and
   implements the RemoteDesktop `Notify*` calls.
4. Pointer events are sent through `zwlr_virtual_pointer_v1`.
5. Keyboard events are sent through `zwp_virtual_keyboard_v1`.

The protocol XML files are included in this repository and compiled with
`wayland-scanner`, so the build does not depend on a local Hyprland source tree.

## Security Notes

Hyprland exposes the virtual input protocols to local Wayland clients. This
bridge does not add a graphical permission prompt on top of that. It accepts KDE
Connect app ids and Hyprland's `surface-transient` fallback app id used by
non-windowed local clients.

Use it as a local-session compatibility bridge. Do not install it system-wide on
multi-user machines unless that trust model is acceptable.

## Troubleshooting

If KDE Connect still does not move the pointer:

```sh
busctl --user status org.freedesktop.impl.portal.desktop.hypr_kdeconnect
journalctl --user _COMM=hypr-kdeconnect --since '10 min ago' --no-pager
```

If the RemoteDesktop interface is missing from the public frontend, check:

```sh
cat ~/.config/xdg-desktop-portal/portals.conf
ls ~/.local/share/xdg-desktop-portal/portals/hypr-kdeconnect.portal
systemctl --user restart xdg-desktop-portal
```

If an old binary keeps running after reinstall:

```sh
pid="$(busctl --user status org.freedesktop.impl.portal.desktop.hypr_kdeconnect 2>/dev/null | awk -F= '/^PID=/{print $2; exit}')"
[ -n "$pid" ] && kill "$pid"
```

## Uninstall

```sh
rm -f ~/.local/bin/hypr-kdeconnect-portal
rm -f ~/.local/share/xdg-desktop-portal/portals/hypr-kdeconnect.portal
rm -f ~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service
systemctl --user restart xdg-desktop-portal
```

Also remove the `org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect` line
from `~/.config/xdg-desktop-portal/portals.conf`.

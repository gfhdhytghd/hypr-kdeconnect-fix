# hypr-kdeconnect-fix

A small user-level `xdg-desktop-portal` RemoteDesktop backend that makes KDE
Connect remote input work on Wayland compositors that expose virtual input
protocols.

KDE Connect already uses `org.freedesktop.portal.RemoteDesktop` for remote
mouse and keyboard input on Wayland. Some compositor-specific portal backends
expose screenshots, screencast, and global shortcuts, but not RemoteDesktop
input. This bridge fills that one missing portal backend interface and injects
events through `zwp_virtual_keyboard_manager_v1` and
`zwlr_virtual_pointer_manager_v1`.

This is a compatibility shim, not a compositor plugin. It was written for
Hyprland, but the core requirement is protocol support; it can also work on
wlroots or protocol-compatible compositors such as sway, river, Wayfire, labwc,
phosh, and niri when their portal routing is configured.

> [!WARNING]
> This software is 99% vibe coded with OpenAI CodeX, but have been manual audited, warn in case you mind it.

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
- Qt 6.5+ Core/DBus
- `pkg-config`
- `wayland-client` 1.20+
- `wayland-scanner`
- `xkbcommon` 1.5+

Runtime:

- a Wayland compositor exposing `zwlr_virtual_pointer_manager_v1`
- a Wayland compositor exposing `zwp_virtual_keyboard_manager_v1`
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
- `~/.local/share/systemd/user/hypr-kdeconnect-portal.service`

The default build enables compiler/linker hardening on Linux, including PIE,
RELRO/BIND_NOW, stack protector, and fortify checks when the compiler supports
them.

By default, the generated portal metadata is visible when `XDG_CURRENT_DESKTOP`
is one of `wlroots`, `Hyprland`, `sway`, `Wayfire`, `river`, `phosh`, `niri`, or
`labwc`. If your compatible compositor uses a different desktop id, set it at
configure time:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DHKCF_PORTAL_USE_IN='wlroots;Hyprland;sway;Wayfire;river;phosh;niri;labwc;your-desktop-id'
```

## Portal Configuration

Configure `xdg-desktop-portal` to route only RemoteDesktop to this backend, while
leaving your normal portal providers in charge of screenshot, screencast, global
shortcuts, file chooser, and other interfaces.

Hyprland example:

`~/.config/xdg-desktop-portal/portals.conf`:

```ini
[preferred]
default=gtk
org.freedesktop.impl.portal.ScreenCast=hyprland
org.freedesktop.impl.portal.Screenshot=hyprland
org.freedesktop.impl.portal.GlobalShortcuts=hyprland
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

niri example:

```ini
[preferred]
default=gnome;gtk;
org.freedesktop.impl.portal.Access=gtk
org.freedesktop.impl.portal.Notification=gtk
org.freedesktop.impl.portal.Secret=gnome-keyring
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

For other wlroots/protocol-compatible compositors, keep your existing
`portals.conf` defaults and add only:

```ini
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

Reload user units and restart the portal frontend after changing metadata or
`portals.conf`:

```sh
systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal
```

If D-Bus already activated an older bridge process, stop the user unit first:

```sh
systemctl --user stop hypr-kdeconnect-portal.service
```

If it was started by plain D-Bus activation instead of systemd, verify the
binary path before killing it:

```sh
pid="$(busctl --user status org.freedesktop.impl.portal.desktop.hypr_kdeconnect 2>/dev/null | awk -F= '/^PID=/{print $2; exit}')"
if [ -n "$pid" ] && [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = "$HOME/.local/bin/hypr-kdeconnect-portal" ]; then
  kill "$pid"
fi
```

## Verification

Check that the public portal frontend exposes RemoteDesktop:

```sh
busctl --user introspect \
  org.freedesktop.portal.Desktop \
  /org/freedesktop/portal/desktop \
  org.freedesktop.portal.RemoteDesktop
```

Check direct virtual pointer injection. These commands will move the pointer in
the current Wayland session:

```sh
hypr-kdeconnect-portal --self-test-motion 120 0
hypr-kdeconnect-portal --self-test-absolute 1440 900
hypr-kdeconnect-portal --self-test-scroll 0 120
hypr-kdeconnect-portal --self-test-scroll-discrete 0 3
```

On Hyprland, you can also check that the compositor sees the virtual devices:

```sh
hyprctl devices | rg 'hypr-kdeconnect|virtual|unknown-device'
```

## How It Works

1. KDE Connect calls the public portal frontend at
   `org.freedesktop.portal.Desktop`.
2. `xdg-desktop-portal` reads `portals.conf` and forwards RemoteDesktop backend
   calls to `org.freedesktop.impl.portal.desktop.hypr_kdeconnect`.
3. This bridge accepts KDE Connect sessions from the portal frontend, starts
   virtual input devices, and implements the RemoteDesktop `Notify*` calls.
4. Pointer events are sent through `zwlr_virtual_pointer_v1`.
5. Keyboard events are sent through `zwp_virtual_keyboard_v1`.

The protocol XML files are included in this repository and compiled with
`wayland-scanner`, so the build does not depend on a local compositor source
tree.

## Security Notes

Compositors that expose the virtual input protocols usually expose them to
trusted local Wayland clients. This bridge does not add a graphical permission
prompt on top of that, so it narrows the D-Bus attack surface instead:

- RemoteDesktop and Session methods are accepted only from the current
  `org.freedesktop.portal.Desktop` owner.
- Sessions are bound to that D-Bus sender and capped in number.
- Empty app ids and substring spoofing are rejected. Accepted ids are exact KDE
  Connect desktop ids plus the `surface-transient` fallback used by some
  non-windowed local clients.
- Notify calls are checked against the selected device mask and bounded before
  being forwarded to Wayland.

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
systemctl --user stop hypr-kdeconnect-portal.service
pid="$(busctl --user status org.freedesktop.impl.portal.desktop.hypr_kdeconnect 2>/dev/null | awk -F= '/^PID=/{print $2; exit}')"
if [ -n "$pid" ] && [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = "$HOME/.local/bin/hypr-kdeconnect-portal" ]; then
  kill "$pid"
fi
```

## Uninstall

```sh
rm -f ~/.local/bin/hypr-kdeconnect-portal
rm -f ~/.local/share/xdg-desktop-portal/portals/hypr-kdeconnect.portal
rm -f ~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service
rm -f ~/.local/share/systemd/user/hypr-kdeconnect-portal.service
systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal
```

Also remove the `org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect` line
from `~/.config/xdg-desktop-portal/portals.conf`.

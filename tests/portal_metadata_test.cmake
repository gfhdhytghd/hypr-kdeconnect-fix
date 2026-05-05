set(portal_file "${HKCF_BUILD_DIR}/hypr-kdeconnect.portal")
set(dbus_service_file "${HKCF_BUILD_DIR}/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service")
set(systemd_service_file "${HKCF_BUILD_DIR}/hypr-kdeconnect-portal.service")

foreach(path IN LISTS portal_file dbus_service_file systemd_service_file)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "missing generated metadata file: ${path}")
  endif()
endforeach()

file(READ "${portal_file}" portal_content)
if(NOT portal_content MATCHES "Interfaces=org\\.freedesktop\\.impl\\.portal\\.RemoteDesktop;")
  message(FATAL_ERROR "portal metadata does not expose RemoteDesktop")
endif()
if(portal_content MATCHES "ScreenCast|Screenshot|GlobalShortcuts")
  message(FATAL_ERROR "portal metadata exposes interfaces outside the intended RemoteDesktop scope")
endif()
foreach(desktop wlroots Hyprland sway Wayfire river phosh niri labwc)
  if(NOT portal_content MATCHES "(^|[=;])${desktop}(;|$)")
    message(FATAL_ERROR "portal metadata does not include compatible desktop id: ${desktop}")
  endif()
endforeach()

file(READ "${dbus_service_file}" dbus_service_content)
set(expected_exec "Exec=${HKCF_INSTALL_FULL_BINDIR}/hypr-kdeconnect-portal")
if(NOT dbus_service_content MATCHES "${expected_exec}")
  message(FATAL_ERROR "D-Bus service Exec does not match install bindir: expected ${expected_exec}")
endif()
if(NOT dbus_service_content MATCHES "SystemdService=hypr-kdeconnect-portal\\.service")
  message(FATAL_ERROR "D-Bus service does not point at the hardened systemd user unit")
endif()

file(READ "${systemd_service_file}" systemd_service_content)
foreach(required
  "Type=dbus"
  "BusName=org.freedesktop.impl.portal.desktop.hypr_kdeconnect"
  "NoNewPrivileges=true"
  "PrivateTmp=true"
  "RestrictAddressFamilies=AF_UNIX"
)
  if(NOT systemd_service_content MATCHES "${required}")
    message(FATAL_ERROR "systemd unit missing expected hardening or bus setting: ${required}")
  endif()
endforeach()

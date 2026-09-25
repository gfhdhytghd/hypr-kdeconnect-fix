{
  lib,
  stdenv,
  cmake,
  pkg-config,
  binutils,
  qt6,
  wayland,
  wayland-scanner,
  libxkbcommon,
  libei,
  portalUseIn ? [
    "wlroots"
    "Hyprland"
    "sway"
    "Wayfire"
    "river"
    "phosh"
    "niri"
    "labwc"
  ],
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "hypr-kdeconnect-fix";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  strictDeps = true;

  nativeBuildInputs = [
    cmake
    pkg-config
    wayland-scanner
    binutils
  ];

  buildInputs = [
    qt6.qtbase
    wayland
    libxkbcommon
    libei
  ];

  cmakeFlags = [
    (lib.cmakeFeature "HKCF_PORTAL_USE_IN" (lib.concatStringsSep ";" portalUseIn))
  ];

  doCheck = true;

  dontWrapQtApps = true;

  enableParallelBuilding = true;

  meta = {
    description = "RemoteDesktop portal backend for KDE Connect on virtual-input Wayland compositors";
    homepage = "https://github.com/danbulant/hypr-kdeconnect-fix";
    license = lib.licenses.mit;
    mainProgram = "hypr-kdeconnect-portal";
    platforms = lib.platforms.linux;
  };
})

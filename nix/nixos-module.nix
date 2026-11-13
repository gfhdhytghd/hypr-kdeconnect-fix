{
  self ? null,
}:

{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.hypr-kdeconnect-fix;
  system = pkgs.stdenv.hostPlatform.system;
  defaultPackage =
    if self != null && self ? packages && builtins.hasAttr system self.packages then
      self.packages.${system}.default
    else
      pkgs.callPackage ./package.nix { };
in
{
  options.services.hypr-kdeconnect-fix = {
    enable = lib.mkEnableOption "hypr-kdeconnect-fix RemoteDesktop portal backend";

    package = lib.mkOption {
      type = lib.types.package;
      default = defaultPackage.override { inherit (cfg) portalUseIn; };
      defaultText = lib.literalExpression "pkgs.hypr-kdeconnect-fix";
      description = "Package providing the hypr-kdeconnect-fix portal backend.";
    };

    configurePortal = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Add this backend to xdg-desktop-portal and route only the
        RemoteDesktop interface to it.
      '';
    };

    portalUseIn = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [
        "wlroots"
        "Hyprland"
        "sway"
        "Wayfire"
        "river"
        "phosh"
        "niri"
        "labwc"
      ];
      description = ''
        Desktop IDs written to the backend portal metadata when using the
        module's default package.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];
    services.dbus.packages = [ cfg.package ];
    systemd.packages = [ cfg.package ];

    xdg.portal = lib.mkIf cfg.configurePortal {
      enable = true;
      extraPortals = [ cfg.package ];
      config.common."org.freedesktop.impl.portal.RemoteDesktop" = [ "hypr-kdeconnect" ];
    };
  };
}

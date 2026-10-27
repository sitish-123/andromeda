{ xdg-desktop-portal-andromeda }:
{
  config,
  pkgs,
  lib,
  modulesPath,
  ...
}:
let
  cfg = config.programs.andromeda;
in
{
  # Disable the nixpkgs module to avoid conflicts
  disabledModules = [ "programs/wayland/andromeda.nix" ];

  options.programs.andromeda = {
    enable = lib.mkEnableOption "Andromeda, a Wayland compositor built on wlroots.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The andromeda package to install.";
    };

    portalPackage = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = xdg-desktop-portal-andromeda.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "the xdg-desktop-portal-andromeda flake's package";
      description = ''
        The xdg-desktop-portal-andromeda package to install.
      '';
    };
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        hardware.graphics.enable = lib.mkDefault true;

        assertions = [
          {
            assertion = cfg.package != null;
            message = "programs.andromeda.package cannot be null when programs.andromeda.enable is true";
          }
        ];
      }

      (lib.mkIf (cfg.package != null) {
        environment.systemPackages = [
          cfg.package
          # So `just debug` / meson outside `nix develop` can find headers via pkg-config.
          pkgs.tomlplusplus
        ];

        # Required for greetd / noctalia-greeter to discover the session (Name=Andromeda).
        # Plain systemPackages .desktop files are not enough; NixOS aggregates via this.
        services.displayManager.sessionPackages = [ cfg.package ];

        systemd.packages = [ cfg.package ];
        systemd.user.services.andromeda = {
          restartIfChanged = false;
          enableDefaultPath = false;
        };
      })

      (lib.mkIf (cfg.portalPackage != null) {
        xdg.portal = {
          enable = lib.mkDefault true;
          extraPortals = [ cfg.portalPackage ];
          config.andromeda.default = lib.mkDefault [
            "andromeda"
            "gtk"
          ];
        };
      })

      (import "${modulesPath}/programs/wayland/wayland-session.nix" {
        inherit lib pkgs;
        enableXWayland = false;
        enableWlrPortal = false;
      })
    ]
  );
}

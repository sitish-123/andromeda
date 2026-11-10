{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (lib.modules) mkIf;
  inherit (lib.options) mkEnableOption mkOption;
  inherit (lib.lists) optional;

  cfg = config.programs.noctalia;
  toml = pkgs.formats.toml { };

  configToml =
    let
      rawConfig = toml.generate "config.toml" cfg.settings;
    in
    if cfg.package != null && cfg.validateConfig then
      pkgs.runCommand "noctalia-config" { } ''
        ${lib.getExe cfg.package} config validate ${rawConfig}
        cp ${rawConfig} $out
      ''
    else
      rawConfig;
in
{
  options.programs.noctalia = {
    enable = mkEnableOption "Whether to enable noctalia, a lightweight Wayland shell and bar.";

    systemd = {
      enable = mkEnableOption "Enables a systemd user service for noctalia.";

      target = mkOption {
        type = lib.types.str;
        default = "graphical-session.target";
        example = "hyprland-session.target";
        description = "The systemd target for the noctalia service.";
      };
    };

    package = mkOption {
      type = lib.types.nullOr lib.types.package;
      description = "The noctalia package to use.";
    };

    validateConfig = mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
    };

    settings = mkOption {
      type = toml.type;
      default = { };
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/noctalia/config.toml`.
        See <https://docs.noctalia.dev/noctalia/configuration/> for more information and examples.
      '';
      example = lib.literalExpression ''
        shell = {
          font = "JetBrainsMono Nerd Font";
        };

        theme = {
          mode = "dark";
        };
      '';
    };
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package;

    systemd.services.noctalia = mkIf (cfg.systemd.enable) {
      description = "Noctalia - A lightweight Wayland shell and bar";
      documentation = [ "https://docs.noctalia.dev/noctalia/" ];
      partOf = [ cfg.systemd.target ];
      after = [ cfg.systemd.target ];
      wantedBy = [ cfg.systemd.target ];
      # without this the service will have the default
      # Environment="PATH=coreutils:…", clobbering the PATH that the DE
      # imported into the user manager.
      enableDefaultPath = false;
      restartTriggers = [
        cfg.package
      ]
      ++ optional (cfg.settings != { }) configToml;
      serviceConfig = {
        ExecStart = lib.getExe cfg.package;
        Restart = "on-failure";
      };
    };

    xdg.config.files = mkIf (cfg.settings != { }) {
      "noctalia/config.toml".source = configToml;
    };

    assertions = [
      {
        assertion = !cfg.systemd.enable || cfg.package != null;
        message = "programs.noctalia.package cannot be null when programs.noctalia.systemd.enable is true";
      }
    ];
  };

  _class = "hjem";
}

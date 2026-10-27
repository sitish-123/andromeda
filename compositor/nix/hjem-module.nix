{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (lib.modules) mkIf mkRemovedOptionModule;
  inherit (lib.options) mkEnableOption mkOption;
  inherit (lib.lists) optional;

  cfg = config.programs.andromeda;
  tomlFormat = pkgs.formats.toml { };
  generateToml =
    name: value:
    if lib.isString value then
      pkgs.writeText name value
    else if builtins.isPath value || lib.isStorePath value then
      value
    else
      tomlFormat.generate name value;
in
{
  imports = [
    (mkRemovedOptionModule [
      "programs"
      "andromeda"
      "validateConfig"
    ] "Build-time validation was removed. Andromeda reports configuration errors at runtime.")
  ];

  options.programs.andromeda = {
    enable = mkEnableOption "Andromeda, a Wayland compositor built on wlroots";

    package = mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The andromeda package to install.";
    };
    settings = mkOption {
      type =
        with lib.types;
        nullOr (oneOf [
          tomlFormat.type
          str
          path
        ]);

      default = null;
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/andromeda/config.toml`.
        Leave null to use the configuration packaged with Andromeda.

        Can be written as:
          - A Nix attrset (converted to TOML via nixpkgs' tomlFormat)
          - A raw TOML string
          - A path to a `.toml` file

        See {file}`examples/config.toml` in the Andromeda repository for every available option.
      '';
      example = lib.literalExpression ''
        {
          general.autostart = [ "noctalia" ];
          layout.gap = 5;
          input.keyboard.layout = "de";
          keybinds = {
            "Mod+Return" = "spawn:kitty";
            "Mod+Q" = "window-close";
            "Mod+R" = "spawn:noctalia msg panel-toggle launcher";
          };
        }
      '';
    };
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package;

    xdg.config.files = mkIf (cfg.settings != null) {
      "andromeda/config.toml".source = generateToml "andromeda-config.toml" cfg.settings;
    };
  };

  _class = "hjem";
}

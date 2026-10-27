{
  description = "xdg-desktop-portal backend for the Andromeda compositor.";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz";
  };

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs.lib) genAttrs;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forEachSystem =
        perSystem:
        genAttrs systems (
          system:
          let
            pkgs = nixpkgs.legacyPackages.${system};
          in
          perSystem { inherit pkgs system; }
        );
    in
    {
      overlays.default = final: prev: {
        xdg-desktop-portal-andromeda = final.callPackage ./nix/package.nix { };
      };

      packages = forEachSystem (
        { pkgs, ... }:
        {
          default = pkgs.callPackage ./nix/package.nix { };
        }
      );

      devShells = forEachSystem (
        { pkgs, system, ... }:
        {
          default = pkgs.callPackage ./nix/devshell.nix {
            xdg-desktop-portal-andromeda = self.packages.${system}.default;
          };
        }
      );
    };
}

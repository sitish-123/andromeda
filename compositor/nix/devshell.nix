{
  pkgs,
  umbriel,
}:
pkgs.mkShell {
  inputsFrom = [ umbriel ];

  nativeBuildInputs = with pkgs; [
    just
    lefthook
    meson
    ninja
    pkg-config
    wayland-scanner
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    gnugrep
    gnused
    findutils
    gdb
    grim
    jq
    foot
    python3
    imagemagick
    procps
    xwayland-satellite
  ];

  shellHook = ''
    echo " Umbriel dev-shell | 'just --list' to see available tasks"
  '';
}

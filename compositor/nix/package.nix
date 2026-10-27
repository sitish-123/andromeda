{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  wlroots_0_20,
  libxkbcommon,
  libinput,
  systemd,
  pixman,
  cairo,
  pango,
  libGL,
  libdrm,
  libgbm,
  libxcb,
  libxcb-wm,
  lcms2,
  jemalloc,
  tomlplusplus,
  nlohmann_json,
  xwayland-satellite,
  makeBinaryWrapper,
  rev ? "unknown",
}:
let
  version = lib.trim (builtins.readFile ../VERSION);
in
stdenv.mkDerivation {
  pname = "andromeda";
  inherit version;

  src = ../.;

  nativeBuildInputs = [
    makeBinaryWrapper
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    wayland-protocols
    wlroots_0_20
    libxkbcommon
    libinput
    # Supplies libudev for the optional native DRM policy support.
    systemd
    pixman
    tomlplusplus
    libGL
    nlohmann_json
    libdrm
    libgbm
    libxcb
    libxcb-wm
    lcms2
    jemalloc
    cairo
    pango
  ];

  mesonBuildType = "release";

  mesonFlags = [ (lib.mesonEnable "tests" false) ];

  postPatch = ''
    substituteInPlace meson.build \
      --replace-fail "andromeda_git_revision_config.set('VCS_TAG', 'unknown')" "andromeda_git_revision_config.set('VCS_TAG', '${rev}')"
  '';

  postInstall = ''
    if [ -f "$out/share/wayland-sessions/andromeda.desktop" ]; then
      substituteInPlace "$out/share/wayland-sessions/andromeda.desktop" \
        --replace-fail 'Exec=start-andromeda' "Exec=$out/bin/start-andromeda"
    fi
    wrapProgram $out/bin/andromeda \
      --prefix PATH : ${lib.makeBinPath [ xwayland-satellite ]} \
  '';

  passthru.providedSessions = [ "andromeda" ];

  meta = with lib; {
    description = "A Wayland compositor built on wlroots";
    homepage = "https://github.com/noctalia-dev/andromeda";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "andromeda";
  };
}

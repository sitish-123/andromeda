project('andromeda-preview', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Quick Look window (docs/search.md §5). Decoding of untrusted PDF and text runs in helper/ (Rust, sandboxed).
# Same standard-library settings as libdesk-ui: they link one archive, so the flags must match.
cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

desk_app_dep = subproject('libdesk-app').get_variable('desk_app_dep')
glycin_dep = dependency('glycin-2')
gio_dep = dependency('gio-2.0')
wayland_client_dep = dependency('wayland-client')

wayland_scanner = find_program('wayland-scanner')
_foreign_xml = dependency('wayland-protocols').get_variable('pkgdatadir') / 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'
_foreign_h = custom_target('xdg-foreign-v2-client-header',
  input: _foreign_xml,
  output: 'xdg-foreign-unstable-v2-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
_foreign_c = custom_target('xdg-foreign-v2-client-source',
  input: _foreign_xml,
  output: 'xdg-foreign-unstable-v2-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

helper_path = get_option('prefix') / get_option('libexecdir') / 'andromeda' / 'andromeda-preview-helper'

_core = static_library('preview_core',
  'src/file_kind.cpp',
  'src/helper_client.cpp',
  dependencies: [desk_app_dep, gio_dep],
  cpp_args: ['-DANDROMEDA_PREVIEW_HELPER_PATH="' + helper_path + '"'],
)

executable('andromeda-preview',
  'src/foreign_parent.cpp',
  'src/image_loader.cpp',
  'src/main.cpp',
  'src/preview_service.cpp',
  'src/preview_window.cpp',
  _foreign_c, _foreign_h,
  link_with: _core,
  dependencies: [desk_app_dep, glycin_dep, gio_dep, wayland_client_dep],
  install: true,
)

configure_file(
  input: 'data/org.gnome.NautilusPreviewer.service.in',
  output: 'org.gnome.NautilusPreviewer.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

# The Rust helper builds with cargo into this build dir and installs next to the other Andromeda helpers.
cargo = find_program('cargo')
_cargo_profile = get_option('buildtype') == 'debug' ? 'debug' : 'release'
helper_bin = custom_target('andromeda-preview-helper',
  output: 'andromeda-preview-helper',
  build_always_stale: true,
  console: true,
  command: [
    'sh', '-c', '"$1" build --quiet --locked $2 --manifest-path "$3" --target-dir "$4" && cp "$4/$5/andromeda-preview-helper" "$6"',
    '_', cargo, _cargo_profile == 'release' ? '--release' : '--profile=dev',
    meson.current_source_dir() / 'helper' / 'Cargo.toml', meson.current_build_dir() / 'cargo-target', _cargo_profile,
    '@OUTPUT@',
  ],
  install: true,
  install_dir: get_option('libexecdir') / 'andromeda',
)

thumbnailer_bin = custom_target('andromeda-thumbnailer',
  output: 'andromeda-thumbnailer',
  build_always_stale: true,
  console: true,
  command: [
    'sh', '-c', '"$1" build --quiet --locked $2 --manifest-path "$3" --target-dir "$4" && cp "$4/$5/andromeda-thumbnailer" "$6"',
    '_', cargo, _cargo_profile == 'release' ? '--release' : '--profile=dev',
    meson.current_source_dir() / 'thumbnailer' / 'Cargo.toml', meson.current_build_dir() / 'cargo-target', _cargo_profile,
    '@OUTPUT@',
  ],
  install: true,
  install_dir: get_option('libexecdir') / 'andromeda',
)

configure_file(
  input: 'data/org.freedesktop.thumbnails.Thumbnailer1.service.in',
  output: 'org.freedesktop.thumbnails.Thumbnailer1.service',
  configuration: {'libexecdir': get_option('prefix') / get_option('libexecdir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

test('file_kind', executable('file_kind_test', 'tests/file_kind_test.cpp', link_with: _core,
  dependencies: [desk_app_dep, gio_dep]))
test('helper_client', executable('helper_client_test', 'tests/helper_client_test.cpp', link_with: _core,
  dependencies: [desk_app_dep, gio_dep]), env: ['ANDROMEDA_PREVIEW_HELPER=' + helper_bin.full_path()],
  depends: helper_bin)

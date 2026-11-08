project('andromeda-player', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Player (docs/apps.md §5, .agent/TASK-player.md): a libdesk-app client; libmpv is loaded with dlopen.
cpp = meson.get_compiler('cpp')
is_libcxx = cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
if is_libcxx
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif
if get_option('buildtype') != 'release'
  if is_libcxx
    add_project_arguments('-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE', language: 'cpp')
  else
    add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
  endif
endif

_desk_app_dep = subproject('libdesk-app').get_variable('desk_app_dep')
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_scanner = find_program('wayland-scanner')
_inhibit_xml = dependency('wayland-protocols').get_variable('pkgdatadir') / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'
_inhibit = [
  custom_target('idle-inhibit-client-header', input: _inhibit_xml,
    output: 'idle-inhibit-unstable-v1-client-protocol.h', command: [_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('idle-inhibit-client-source', input: _inhibit_xml,
    output: 'idle-inhibit-unstable-v1-client-protocol.c', command: [_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]
# Headers only: libmpv.so.2 is opened at run time.
_mpv_headers = dependency('mpv').partial_dependency(compile_args: true, includes: true)

executable('andromeda-player',
  _inhibit,
  sources: files(
    'src/main.cpp',
    'src/mpris_server.cpp',
    'src/music_library.cpp',
    'src/mpv_lib.cpp',
    'src/mpv_player.cpp',
    'src/player_app.cpp',
    'src/video_surface.cpp',
  ),
  dependencies: [_desk_app_dep, _mpv_headers, dependency('libavformat'), dependency('libavutil'), dependency('wayland-egl'), dependency('egl'), dependency('dl')],
  cpp_args: _warnings,
  install: true,
)

install_data('data/dev.andromeda.Player.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/icons/dev.andromeda.Player.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

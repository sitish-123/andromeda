project('andromeda-files', ['cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Files (docs/apps.md §3, .agent/DESIGN.md "Files app P2"): a libdesk-app client.
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

_files_lib = static_library('files_core',
  sources: files('src/trash.cpp'),
  dependencies: _desk_app_dep,
  cpp_args: _warnings,
)

executable('andromeda-files',
  sources: files(
    'src/file_row.cpp',
    'src/files_app.cpp',
    'src/files_window.cpp',
    'src/files_window_picker.cpp',
    'src/picker_portal.cpp',
    'src/main.cpp',
  ),
  link_with: _files_lib,
  dependencies: _desk_app_dep,
  cpp_args: _warnings,
  install: true,
)

test('trash', executable('trash_test', 'tests/trash_test.cpp', link_with: _files_lib, dependencies: _desk_app_dep,
  include_directories: include_directories('src'), cpp_args: _warnings))

install_data('data/dev.andromeda.Files.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/andromeda-files.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')
configure_file(
  input: 'data/org.freedesktop.impl.portal.desktop.andromeda.Files.service.in',
  output: 'org.freedesktop.impl.portal.desktop.andromeda.Files.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

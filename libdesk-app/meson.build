project('desk-app', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# The app frame for first-party apps (docs/apps.md §2): run loop, window frame, commands, state, single instance.
# Same standard-library settings as libdesk-ui: they link one archive, so the flags must match.
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
if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
endif

_desk_ui_dep = subproject('libdesk-ui').get_variable('desk_ui_dep')
_desk_system_dep = subproject('libdesk-system').get_variable('desk_system_dep')

_deps = [
  _desk_ui_dep,
  _desk_system_dep,
  dependency('sdbus-c++'),
  dependency('nlohmann_json'),
  dependency('xkbcommon'),
  dependency('wayland-client'),
  dependency('gio-2.0'),
  dependency('threads'),
]

_inc = include_directories('src')
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_scanner = find_program('wayland-scanner')
_foreign_xml = dependency('wayland-protocols').get_variable('pkgdatadir') / 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'
_foreign = [
  custom_target('xdg-foreign-v2-client-header', input: _foreign_xml,
    output: 'xdg-foreign-unstable-v2-client-protocol.h', command: [_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-foreign-v2-client-source', input: _foreign_xml,
    output: 'xdg-foreign-unstable-v2-client-protocol.c', command: [_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

desk_app_lib = static_library('desk_app',
  _foreign,
  sources: files(
    'src/app/foreign_parent.cpp',
    'src/app/app_loop.cpp',
    'src/app/app_window.cpp',
    'src/app/command_table.cpp',
    'src/app/desk_app.cpp',
    'src/app/single_instance.cpp',
    'src/app/state_store.cpp',
    'src/files/file_ops_client.cpp',
  ),
  dependencies: _deps,
  include_directories: _inc,
  cpp_args: _warnings,
)

desk_app_dep = declare_dependency(
  link_with: desk_app_lib,
  dependencies: _deps,
  include_directories: _inc,
)

if meson.is_subproject()
  subdir_done()
endif

executable('desk-app-sample', 'examples/sample_app.cpp',
  dependencies: desk_app_dep,
  cpp_args: _warnings,
)

_dbus_run_session = find_program('dbus-run-session')
foreach _t : ['command_table', 'state_store', 'file_model', 'type_ahead']
  test(_t, executable(_t + '_test', 'tests/' + _t + '_test.cpp', dependencies: desk_app_dep, cpp_args: _warnings))
endforeach
test('single_instance', _dbus_run_session,
  args: ['--', executable('single_instance_test', 'tests/single_instance_test.cpp', dependencies: desk_app_dep,
    cpp_args: _warnings)],
  timeout: 30,
)

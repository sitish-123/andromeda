project('desk-system', ['cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# D-Bus clients shared by the shell, the greeter and libdesk-app apps (docs/architecture-v2.md §12b).
# Same standard-library settings as libdesk-ui and the shell: they link one archive, so the flags must match.
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

# core/log, core/poll_source and util/* live in libdesk-ui for now.
_desk_ui_dep = subproject('libdesk-ui').get_variable('desk_ui_dep')

_deps = [
  _desk_ui_dep,
  dependency('sdbus-c++'),
  dependency('gio-2.0'),
  dependency('polkit-agent-1'),
  dependency('polkit-gobject-1'),
]

_sources = files(
  'src/core/process/process.cpp',
  'src/dbus/logind/logind_service.cpp',
  'src/dbus/logind/power_config_service.cpp',
  'src/dbus/power/power_profiles_service.cpp',
  'src/dbus/session_bus.cpp',
  'src/dbus/system_bus.cpp',
  'src/dbus/system_interactive_call.cpp',
  'src/dbus/accounts/accounts_service.cpp',
  'src/dbus/idle/screensaver_service.cpp',
  'src/dbus/locale/locale_service.cpp',
  'src/dbus/systemd/systemd_unit_name.cpp',
  'src/dbus/systemd/systemd_units_service.cpp',
  'src/dbus/timedate/timedate_service.cpp',
  'src/dbus/modem/modem_manager_service.cpp',
  'src/dbus/polkit/polkit_agent.cpp',
  'src/dbus/polkit/polkit_session_support.cpp',
  'src/dbus/secret/secret_collection_probe.cpp',
  'src/dbus/tray/fcitx_status.cpp',
  'src/dbus/tray/tray_service.cpp',
  'src/dbus/upower/upower_charge_limit_support.cpp',
  'src/dbus/portal/global_shortcuts_portal.cpp',
  'src/dbus/portal/background_portal.cpp',
  'src/dbus/portal/inhibit_portal.cpp',
  'src/dbus/portal/settings_portal.cpp',
  'src/dbus/udisks/udisks_service.cpp',
  'src/dbus/upower/upower_service.cpp',
)

_inc = include_directories('src')

desk_system_lib = static_library('desk_system',
  sources: _sources,
  dependencies: _deps,
  include_directories: _inc,
  cpp_args: ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow'],
)

desk_system_dep = declare_dependency(
  link_with: desk_system_lib,
  dependencies: _deps,
  include_directories: _inc,
)

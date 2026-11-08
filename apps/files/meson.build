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
# Gallery video in place: the preview's subsurface player and sandboxed media helper (audio loads lazily).
_preview_media_dep = subproject('andromeda-preview').get_variable('preview_media_dep')
_gio_unix_dep = dependency('gio-unix-2.0') # GUnixMountMonitor, GDesktopAppInfo
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_files_lib = static_library('files_core',
  sources: files('src/trash.cpp', 'src/archive_listing.cpp', 'src/permission_access.cpp', 'src/server_url.cpp',
    'src/tags.cpp', 'src/exif_date.cpp', 'src/state_file.cpp'),
  dependencies: _desk_app_dep,
  cpp_args: _warnings,
)

executable('andromeda-files',
  sources: files(
    'src/files_app.cpp', 'src/offline_map.cpp', 'src/gallery_video.cpp',
    'src/batch_rename_sheet.cpp',
    'src/tag_manager_sheet.cpp',
    'src/terminal_pane.cpp',
    'src/select_pattern_sheet.cpp',
    'src/folder_compare.cpp',
    'src/smart_folders.cpp',
    'src/smart_folder_sheet.cpp',
    'src/compress_sheet.cpp',
    'src/extract_sheet.cpp',
    'src/get_info_sheet.cpp',
    'src/starred.cpp',
    'src/terminal.cpp',
    'src/index_search.cpp',
    'src/file_manager1.cpp',
    'src/link_share.cpp',
    'src/phone_page.cpp',
    'src/storage_page.cpp',
    'src/vaults.cpp',
    'src/vault_sheet.cpp',
    'src/drives.cpp',
    'src/network_page.cpp',
    'src/conflict_sheet.cpp',
    'src/connect_sheet.cpp',
    'src/details_pane.cpp',
    'src/job_pill.cpp',
    'src/files_window.cpp',
    'src/view_store.cpp',
    'src/service_menus.cpp',
    'src/files_window_picker.cpp',
    'src/app_chooser_portal.cpp',
    'src/app_chooser_sheet.cpp',
    'src/picker_portal.cpp',
    'src/portal_sheets.cpp',
    'src/system_portals.cpp',
    'src/main.cpp',
  ),
  link_with: _files_lib,
  dependencies: [_desk_app_dep, _gio_unix_dep, dependency('vterm'), _preview_media_dep],
  cpp_args: _warnings,
  install: true,
)

test('permission_access', executable('permission_access_test', 'tests/permission_access_test.cpp',
  link_with: _files_lib, dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('archive_listing', executable('archive_listing_test', 'tests/archive_listing_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('server_url', executable('server_url_test', 'tests/server_url_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('exif_date', executable('exif_date_test', 'tests/exif_date_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('safe_regex', executable('safe_regex_test', 'tests/safe_regex_test.cpp', dependencies: _desk_app_dep,
  cpp_args: _warnings))
test('picker_match', executable('picker_match_test', 'tests/picker_match_test.cpp', 'src/picker_portal.cpp',
  dependencies: [_desk_app_dep, _gio_unix_dep], include_directories: include_directories('src'), cpp_args: _warnings))
test('trash', executable('trash_test', 'tests/trash_test.cpp', link_with: _files_lib, dependencies: _desk_app_dep,
  include_directories: include_directories('src'), cpp_args: _warnings))

install_data('data/dev.andromeda.Files.desktop', 'data/dev.andromeda.Photos.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data/andromeda-files.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')
configure_file(
  input: 'data/org.freedesktop.impl.portal.desktop.andromeda.Files.service.in',
  output: 'org.freedesktop.impl.portal.desktop.andromeda.Files.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
configure_file(
  input: 'data/org.freedesktop.FileManager1.service.in',
  output: 'org.freedesktop.FileManager1.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
configure_file(
  input: 'data/andromeda-files-reconnect.service.in',
  output: 'andromeda-files-reconnect.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)
install_data('data/icons/dev.andromeda.Files.svg', 'data/icons/dev.andromeda.Photos.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

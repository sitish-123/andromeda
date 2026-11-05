project('andromeda-viewer', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Viewer (docs/apps.md §4, .agent/DESIGN-viewer.md): a libdesk-app client. PDFs decode in andromeda-preview's
# sandboxed helper; images through glycin, as in Quick Look.
cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_model = static_library('viewer_model', 'src/document_model.cpp', cpp_args: _warnings)

test('document_model', executable('document_model_test', 'tests/document_model_test.cpp', link_with: _model,
  include_directories: include_directories('src'), cpp_args: _warnings))

_desk_app_dep = subproject('libdesk-app').get_variable('desk_app_dep')
_preview_dep = subproject('andromeda-preview').get_variable('preview_client_dep')

executable('andromeda-viewer',
  'src/annotations.cpp',
  'src/clipboard_source.cpp',
  'src/main.cpp',
  'src/viewer_app.cpp',
  'src/viewer_window.cpp',
  link_with: _model,
  dependencies: [_desk_app_dep, _preview_dep, dependency('wayland-client')],
  cpp_args: _warnings,
  install: true,
)

install_data('data/dev.andromeda.Viewer.desktop', install_dir: get_option('datadir') / 'applications')

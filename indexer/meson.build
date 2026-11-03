project('andromeda-index', 'cpp',
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=true', 'b_ndebug=if-release'],
)

sqlite_dep = dependency('sqlite3', version: '>=3.43') # FTS5 contentless_delete
extract_deps = [dependency('poppler-glib'), dependency('libzip')]

core = static_library('index-core',
  'src/index_db.cpp',
  'src/crawler.cpp',
  'src/skip_rules.cpp',
  'src/folder_reader.cpp',
  'src/live_index.cpp',
  'src/query_server.cpp',
  'src/document_kind.cpp',
  'src/text_extract.cpp',
  'src/content_worker.cpp',
  dependencies: [sqlite_dep, dependency('threads')],
)
core_dep = declare_dependency(link_with: core, include_directories: include_directories('src'), dependencies: [sqlite_dep, dependency('threads')])

index_exe = executable('andromeda-index', 'src/main.cpp', dependencies: core_dep, install: true)
extract_exe = executable('andromeda-index-extract', 'src/extract_main.cpp', 'src/extract.cpp', dependencies: [core_dep, extract_deps],
                         install: true)

user_unit_dir = get_option('prefix') / 'lib' / 'systemd' / 'user'
configure_file(
  input: 'data/andromeda-index.service.in',
  output: 'andromeda-index.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: user_unit_dir,
)
install_data('data/andromeda-index.socket', install_dir: user_unit_dir)

test('index_core', executable('index_core_test', 'tests/index_core_test.cpp', dependencies: core_dep))
test('live_index', executable('live_index_test', 'tests/live_index_test.cpp', dependencies: core_dep))
test('serve', find_program('python3'), args: [files('tests/serve_test.py'), index_exe], depends: extract_exe, timeout: 60)
benchmark('query_100k', executable('query_bench', 'tests/query_bench.cpp', dependencies: core_dep), timeout: 300)

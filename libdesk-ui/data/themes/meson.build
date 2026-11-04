# Theme files for third-party toolkits, generated from the design tokens (docs/m0-foundation.md §1). The shell's
# runtime accent file imports the GTK file of the current scheme.
_tokens_gen = [find_program('python3'), meson.project_source_root() / 'protocols/gen-design-tokens.py',
               meson.project_source_root() / 'protocols/design-tokens.toml']
_tokens_toml = meson.project_source_root() / 'protocols/design-tokens.toml'

foreach scheme : ['light', 'dark']
  foreach gtk : ['3', '4']
    custom_target('andromeda-gtk@0@-@1@'.format(gtk, scheme),
      output: 'andromeda-gtk@0@-@1@.css'.format(gtk, scheme),
      depend_files: _tokens_toml,
      command: [_tokens_gen, '@OUTPUT@'],
      install: true,
      install_dir: get_option('datadir') / 'andromeda/gtk',
    )
  endforeach
  custom_target('andromeda-qt-@0@'.format(scheme),
    output: 'andromeda-qt-@0@.conf'.format(scheme),
    depend_files: _tokens_toml,
    command: [_tokens_gen, '@OUTPUT@'],
    install: true,
    install_dir: get_option('datadir') / 'qt6ct/colors',
    install_mode: 'rw-r--r--',
  )
  # KDE resolves a scheme by file name, which must match its ColorScheme key.
  custom_target('andromeda-kde-@0@'.format(scheme),
    output: 'Andromeda@0@.colors'.format(scheme == 'light' ? 'Light' : 'Dark'),
    depend_files: _tokens_toml,
    command: [_tokens_gen, '@OUTPUT@'],
    install: true,
    install_dir: get_option('datadir') / 'color-schemes',
  )
endforeach

subdir('fcitx5-light')
subdir('fcitx5-dark')

custom_target('andromeda-fcitx5-dark',
  output: ['theme.conf', 'panel.svg', 'highlight.svg'],
  depend_files: _tokens_toml,
  command: [_tokens_gen, '@OUTPUT0@'],
  install: true,
  install_dir: get_option('datadir') / 'fcitx5/themes/andromeda-dark',
)

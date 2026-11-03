project('desk-ui', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# With dev_tokens, tokens are writable globals so the shell can reload them live (ANDROMEDA_DEV_TOKENS).
_tokens_runtime = get_option('dev_tokens') ? ['-DTOKENS_RUNTIME'] : []
add_project_arguments(_tokens_runtime, language: 'cpp')

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Same standard-library settings as the shell: both link one archive, so the flags must match.
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
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: ['c', 'cpp'])
endif

egl_dep = dependency('egl', required: false)
if not egl_dep.found()
  egl_dep = dependency('wayland-egl', required: false)
endif
if not egl_dep.found()
  egl_dep = dependency('epoxy')
endif
gles2_dep = dependency('glesv2', required: false)
if not gles2_dep.found()
  gles2_dep = dependency('epoxy')
endif

foreach _h : ['stb/stb_image_resize2.h', 'stb/stb_image_write.h']
  if not cc.has_header(_h)
    error(_h + ' not found')
  endif
endforeach

_system_deps = [
  dependency('wayland-client'),
  dependency('wayland-egl'),
  egl_dep,
  gles2_dep,
  dependency('freetype2'),
  dependency('fontconfig'),
  dependency('cairo'),
  dependency('cairo-ft'),
  dependency('pango', version: '>=1.50'),
  dependency('pangocairo'),
  dependency('pangoft2'),
  dependency('harfbuzz'),
  dependency('librsvg-2.0'),
  dependency('xkbcommon'),
  dependency('glib-2.0'),
  dependency('gobject-2.0'),
  dependency('md4c'),
  dependency('nlohmann_json'),
  dependency('libwebp'),
  dependency('libwebpdemux'),
  dependency('libjxl'),
  dependency('libjxl_threads'),
  cpp.find_library('dl', required: false),
]

# ── Vendored: fzy matcher ─────────────────────────────────────────────────────
_fzy_lib = static_library('fzy',
  'third_party/fzy/src/match.c',
  c_args: ['-Wno-pedantic', '-Wno-conversion'],
  override_options: ['warning_level=0'],
)
_fzy_inc = include_directories('third_party/fzy/src', is_system: true)

# ── Wayland client protocols the toolkit binds ────────────────────────────────
wayland_scanner    = find_program('wayland-scanner')
wayland_protos_dir = dependency('wayland-protocols').get_variable('pkgdatadir')

_protocol_sources = []
_protocol_headers = []
foreach _p : [
  ['xdg-output-unstable-v1', wayland_protos_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
  ['xdg-shell', wayland_protos_dir / 'stable/xdg-shell/xdg-shell.xml'],
  ['xdg-activation-v1', wayland_protos_dir / 'staging/xdg-activation/xdg-activation-v1.xml'],
  ['ext-idle-notify-v1', wayland_protos_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'],
  ['ext-background-effect-v1', wayland_protos_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml'],
  ['fractional-scale-v1', wayland_protos_dir / 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['viewporter', wayland_protos_dir / 'stable/viewporter/viewporter.xml'],
  ['idle-inhibit-unstable-v1', wayland_protos_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'],
  ['text-input-unstable-v3', 'protocols/text-input-unstable-v3.xml'],
  ['cursor-shape-v1', 'protocols/cursor-shape-v1.xml'],
  ['hyprland-focus-grab-v1', 'protocols/hyprland-focus-grab-v1.xml'],
  ['wlr-output-management-unstable-v1', 'protocols/wlr-output-management-unstable-v1.xml'],
  ['dsk-surface-v1', 'protocols/dsk-surface-v1.xml'],
]
  _h = custom_target(_p[0] + '-client-header',
    input: _p[1],
    output: _p[0] + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  _protocol_sources += custom_target(_p[0] + '-client-source',
    input: _p[1],
    output: _p[0] + '-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: _h,
  )
  _protocol_headers += _h
endforeach

# 'namespace' is a C++ keyword: rename it in the generated header only.
_wlr_ls_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'
_wlr_ls_h = custom_target('wlr-layer-shell-unstable-v1-client-header',
  input: _wlr_ls_xml,
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [
    'sh', '-c', '"$1" client-header "$2" "$3" && sed -i s/namespace/name_space/g "$3"',
    '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@',
  ],
)
_protocol_sources += custom_target('wlr-layer-shell-unstable-v1-client-source',
  input: _wlr_ls_xml,
  output: 'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  depends: _wlr_ls_h,
)
_protocol_headers += _wlr_ls_h

_protocol_headers += custom_target('design-tokens-header',
  input: ['protocols/gen-design-tokens.py', 'protocols/design-tokens.toml'],
  output: 'design_tokens.h',
  command: [find_program('python3'), '@INPUT0@', '@INPUT1@', '@OUTPUT@'],
)

if get_option('install_toolkit_themes')
  subdir('data/themes')
endif

# ── Library ───────────────────────────────────────────────────────────────────
_desk_ui_sources = files(
  'src/core/deferred_call.cpp',
  'src/core/files/directory_scanner.cpp',
  'src/core/files/resource_paths.cpp',
  'src/core/input/keybind_matcher.cpp',
  'src/core/input/key_chord.cpp',
  'src/core/log.cpp',
  'src/core/process/process_fds.cpp',
  'src/core/timer_manager.cpp',
  'src/core/ui_phase.cpp',
  'src/i18n/i18n.cpp',
  'src/i18n/i18n_service.cpp',
  'src/i18n/language_tag.cpp',
  'src/motion/desk_motion.cpp',
  'src/render/animation/animation.cpp',
  'src/render/animation/animation_manager.cpp',
  'src/render/animation/motion_service.cpp',
  'src/render/backend/gles_framebuffer.cpp',
  'src/render/backend/gles_render_backend.cpp',
  'src/render/backend/gles_texture_manager.cpp',
  'src/render/backend/render_backend.cpp',
  'src/render/core/async_texture_cache.cpp',
  'src/render/core/cached_layer.cpp',
  'src/render/core/color.cpp',
  'src/render/core/damage_region.cpp',
  'src/render/core/heif_library.cpp',
  'src/render/core/image_decoder.cpp',
  'src/render/core/image_encoder.cpp',
  'src/render/core/image_file_loader.cpp',
  'src/render/core/image_orientation.cpp',
  'src/render/core/shader_program.cpp',
  'src/render/core/startup_metrics.cpp',
  'src/render/core/shared_texture_cache.cpp',
  'src/render/core/stb_image_resize_impl.cpp',
  'src/render/core/thumbnail_service.cpp',
  'src/render/gl_shared_context.cpp',
  'src/render/programs/audio_spectrum_program.cpp',
  'src/render/programs/blur_program.cpp',
  'src/render/programs/countdown_ring_program.cpp',
  'src/render/programs/effect_program.cpp',
  'src/render/programs/fancy_audio_visualizer_program.cpp',
  'src/render/programs/glyph_program.cpp',
  'src/render/programs/graph_program.cpp',
  'src/render/programs/image_program.cpp',
  'src/render/programs/linear_gradient_program.cpp',
  'src/render/programs/lockscreen_transition_program.cpp',
  'src/render/programs/rect_program.cpp',
  'src/render/programs/screen_corner_program.cpp',
  'src/render/programs/spinner_program.cpp',
  'src/render/programs/wallpaper_mask_program.cpp',
  'src/render/programs/wallpaper_program.cpp',
  'src/render/render_context.cpp',
  'src/render/render_target.cpp',
  'src/render/scene/fancy_audio_visualizer_node.cpp',
  'src/render/scene/graph_node.cpp',
  'src/render/scene/input_area.cpp',
  'src/render/scene/input_dispatcher.cpp',
  'src/render/scene/node.cpp',
  'src/render/text/cairo_glyph_renderer.cpp',
  'src/render/text/cairo_text_renderer.cpp',
  'src/render/text/font_registry.cpp',
  'src/render/text/font_weight_catalog.cpp',
  'src/render/text/glyph_registry.cpp',
  'src/render/wallpaper_renderer.cpp',
  'src/time/time_format.cpp',
  'src/ui/app_icon_colorization.cpp',
  'src/ui/builders.cpp',
  'src/ui/auth/auth_column.cpp',
  'src/ui/auth/ryoku_auth_scene.cpp',
  'src/ui/controls/box.cpp',
  'src/ui/controls/button.cpp',
  'src/ui/controls/checkbox.cpp',
  'src/ui/controls/collapsible.cpp',
  'src/ui/controls/color_picker.cpp',
  'src/ui/controls/color_swatch_preview.cpp',
  'src/ui/controls/context_menu.cpp',
  'src/ui/controls/context_menu_popup.cpp',
  'src/ui/controls/countdown_ring.cpp',
  'src/ui/controls/drag_source.cpp',
  'src/ui/controls/drop_zone.cpp',
  'src/ui/controls/flex.cpp',
  'src/ui/controls/glyph.cpp',
  'src/ui/controls/glyph_picker.cpp',
  'src/ui/controls/graph.cpp',
  'src/ui/controls/grid_tile.cpp',
  'src/ui/controls/grid_view.cpp',
  'src/ui/controls/image.cpp',
  'src/ui/controls/input.cpp',
  'src/ui/controls/label.cpp',
  'src/ui/controls/list_editor.cpp',
  'src/ui/controls/lockscreen_transition_cover.cpp',
  'src/ui/controls/markdown_view.cpp',
  'src/ui/controls/progress_bar.cpp',
  'src/ui/controls/radio_button.cpp',
  'src/ui/controls/range_slider.cpp',
  'src/ui/controls/roving_list_nav.cpp',
  'src/ui/controls/screen_corner.cpp',
  'src/ui/controls/scrollbar.cpp',
  'src/ui/controls/scroll_view.cpp',
  'src/ui/controls/search_picker.cpp',
  'src/ui/controls/segmented.cpp',
  'src/ui/controls/select.cpp',
  'src/ui/controls/select_dropdown_popup.cpp',
  'src/ui/controls/separator.cpp',
  'src/ui/controls/slider.cpp',
  'src/ui/controls/spacer.cpp',
  'src/ui/controls/spinner.cpp',
  'src/ui/controls/stepper.cpp',
  'src/ui/controls/toggle.cpp',
  'src/ui/controls/virtual_grid_view.cpp',
  'src/ui/controls/virtual_list_view.cpp',
  'src/ui/dialogs/color_picker_dialog.cpp',
  'src/ui/dialogs/color_picker_dialog_popup.cpp',
  'src/ui/dialogs/dialog_popup_host.cpp',
  'src/ui/dialogs/file_dialog.cpp',
  'src/ui/dialogs/file_dialog_popup.cpp',
  'src/ui/dialogs/file_dialog_view.cpp',
  'src/ui/dialogs/file_entry_row.cpp',
  'src/ui/dialogs/file_entry_tile.cpp',
  'src/ui/dialogs/glyph_picker_dialog.cpp',
  'src/ui/dialogs/glyph_picker_dialog_popup.cpp',
  'src/ui/dialogs/layer_popup_host.cpp',
  'src/ui/dialogs/search_picker_popup.cpp',
  'src/ui/drag_drop_controller.cpp',
  'src/ui/palette.cpp',
  'src/ui/popup_chrome.cpp',
  'src/ui/scroll_into_view.cpp',
  'src/ui/split_pane_focus.cpp',
  'src/ui/style.cpp',
  'src/ui/surface_shadow.cpp',
  'src/ui/visuals/audio_visualizer.cpp',
  'src/ui/visuals/fancy_audio_visualizer.cpp',
  'src/util/clamp.cpp',
  'src/util/fuzzy_match.cpp',
  'src/wayland/dsk_surface_client.cpp',
  'src/wayland/hyprland/focus_grab_service.cpp',
  'src/wayland/layer_surface.cpp',
  'src/wayland/output_probe.cpp',
  'src/wayland/popup_surface.cpp',
  'src/wayland/subsurface.cpp',
  'src/wayland/surface.cpp',
  'src/wayland/text_input_service.cpp',
  'src/wayland/toplevel_surface.cpp',
  'src/wayland/wayland_connection.cpp',
  'src/wayland/wayland_seat.cpp',
)

# '.' adds the source root and the build root, where the generated headers land.
_desk_ui_inc = [
  include_directories('src'),
  include_directories('.'),
  include_directories('third_party/wuffs', is_system: true),
]

desk_ui_lib = static_library('desk_ui',
  sources: _desk_ui_sources + _protocol_sources + _protocol_headers,
  dependencies: _system_deps,
  include_directories: _desk_ui_inc + [_fzy_inc],
  link_with: _fzy_lib,
  cpp_args: [
    '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
    # paths::assetsRoot() looks for the installed noctalia asset bundle (fonts, translations).
    '-DNOCTALIA_INSTALL_PREFIX="' + get_option('prefix') + '"',
    '-DNOCTALIA_INSTALL_DATADIR="' + get_option('datadir') + '"',
    '-DNOCTALIA_SOURCE_ASSETS_DIR="' + (meson.project_source_root() / '..' / 'shell' / 'assets') + '"',
  ],
)

desk_ui_dep = declare_dependency(
  link_with: [desk_ui_lib, _fzy_lib],
  dependencies: _system_deps,
  include_directories: _desk_ui_inc,
  sources: _protocol_headers,
  compile_args: _tokens_runtime,
)

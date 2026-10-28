# andromedafx is a hard fork of wlrfx/scenefx (MIT, see LICENSE), built as a
# static library rather than joined into the compositor's source lists: it is C
# compiled against wlroots' private struct layouts (-DWLR_PRIVATE=), and those
# flags must not leak into Andromeda's C++23 units.

cc = meson.get_compiler('c')

# dladdr, for the scene helper guard. Folded into libc since glibc 2.34.
andromedafx_dl_dep = cc.find_library('dl', required: false)
andromedafx_egl_dep = dependency('egl')
andromedafx_gbm_dep = dependency('gbm')
andromedafx_glesv2_dep = dependency('glesv2')
andromedafx_math_dep = cc.find_library('m')
andromedafx_rt_dep = cc.find_library('rt')

# Older EGL headers cannot express the dmabuf import and fence attributes
# fx_texture.c and egl.c rely on.
andromedafx_eglext_version = cc.get_define(
  'EGL_EGLEXT_VERSION',
  dependencies: andromedafx_egl_dep,
  prefix: '#include <EGL/eglext.h>',
).to_int()
if andromedafx_eglext_version < 20210604
  error('andromedafx requires EGL headers with EGL_EGLEXT_VERSION >= 20210604')
endif

andromedafx_sources = files(
  'render/color.c',
  'render/egl.c',
  'render/pixel_format.c',
  'render/fx_renderer/fx_framebuffer.c',
  'render/fx_renderer/fx_offscreen_buffers.c',
  'render/fx_renderer/fx_pass.c',
  'render/fx_renderer/fx_renderer.c',
  'render/fx_renderer/fx_texture.c',
  'render/fx_renderer/effect_shader.c',
  'render/fx_renderer/pixel_format.c',
  'render/fx_renderer/shaders.c',
  'render/fx_renderer/tracy.c',
  'render/fx_renderer/util.c',
  'types/fx/blur_data.c',
  'types/fx/clipped_region.c',
  'types/linux_drm_syncobj.c',
  'types/output/output.c',
  'types/scene/capture.c',
  'types/scene/drag_icon.c',
  'types/scene/layer_shell_v1.c',
  'types/scene/linked_node.c',
  'types/scene/output_layout.c',
  'types/scene/scene_guard.c',
  'types/scene/subsurface_tree.c',
  'types/scene/surface.c',
  'types/scene/wlr_scene.c',
  'types/scene/xdg_shell.c',
  'util/array.c',
  'util/env.c',
  'util/matrix.c',
  'util/time.c',
)

andromedafx_deps = [
  libdrm_dep,
  pixman_dep,
  andromedafx_dl_dep,
  andromedafx_egl_dep,
  andromedafx_gbm_dep,
  andromedafx_glesv2_dep,
  andromedafx_math_dep,
  andromedafx_rt_dep,
  wayland_server_dep,
  wlroots_dep,
]

# ICC profile evaluation for client-supplied color management. Without lcms2
# the fallback rejects ICC profiles and leaves the parametric transforms.
andromedafx_lcms2_dep = dependency('lcms2', required: false)
if andromedafx_lcms2_dep.found()
  andromedafx_deps += andromedafx_lcms2_dep
  andromedafx_sources += files('render/color_lcms2.c')
else
  andromedafx_sources += files('render/color_fallback.c')
endif

# wlroots' layer-shell header pulls in the generated server protocol header.
andromedafx_sources += layer_shell_protocol_h

andromedafx_little_endian = target_machine.endian() == 'little'

andromedafx_c_args = [
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  '-DWLR_PRIVATE=',
  '-DWLR_LITTLE_ENDIAN=@0@'.format(andromedafx_little_endian.to_int()),
  '-DWLR_BIG_ENDIAN=@0@'.format((not andromedafx_little_endian).to_int()),
]

# Keep wlr_log's __FILE__ output relative to the source root rather than
# leaking the absolute build path.
andromedafx_relative_dir = fs.relative_to(
  meson.current_source_dir(),
  meson.global_build_root(),
) + '/'
if cc.has_argument('-fmacro-prefix-map=/prefix/to/hide=')
  andromedafx_c_args += '-fmacro-prefix-map=@0@='.format(andromedafx_relative_dir)
else
  andromedafx_c_args += '-D_WLR_REL_SRC_DIR="@0@"'.format(andromedafx_relative_dir)
endif

andromedafx_c_args += cc.get_supported_arguments([
  '-Wundef',
  '-Wlogical-op',
  '-Wmissing-include-dirs',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Winit-self',
  '-Wstrict-prototypes',
  '-Wimplicit-fallthrough=2',
  '-Wendif-labels',
  '-Wstrict-aliasing=2',
  '-Woverflow',
  '-Wmissing-prototypes',
  '-Walloca',

  '-Wno-missing-braces',
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
])

# Without these, tracy.c and every TRACY_* call site compile away.
if tracy_dep.found()
  andromedafx_deps += tracy_dep
  andromedafx_c_args += ['-DTRACY_ENABLE', '-DTRACY_ON_DEMAND']
endif

# Shaders are compiled at runtime, so they ship as generated char arrays rather
# than data files an installed binary would have to locate.
andromedafx_shader_embed = find_program(
  'render/fx_renderer/shaders/embed.sh',
  native: true,
)
foreach shader : [
  'blur1.frag',
  'blur2.frag',
  'border.frag',
  'box_shadow.frag',
  'common.vert',
  'corner_alpha.frag',
  'effect_light.frag',
  'gradient.frag',
  'output.frag',
  'quad.frag',
  'quad_grad.frag',
  'quad_grad_round.frag',
  'quad_round.frag',
  'tex.frag',
]
  andromedafx_sources += custom_target(
    shader.underscorify() + '_src.h',
    command: [andromedafx_shader_embed, shader.underscorify() + '_src'],
    input: 'render/fx_renderer/shaders' / shader,
    output: shader.underscorify() + '_src.h',
    feed: true,
    capture: true,
  )
endforeach

# Only `include` reaches the compositor. `internal` holds headers whose paths
# (`render/`, `types/`, `util/`) would otherwise sit at the root of every
# compositor translation unit's include path.
andromedafx_inc = include_directories('include')
andromedafx_internal_inc = include_directories('internal')

andromedafx_lib = static_library(
  'andromedafx',
  andromedafx_sources,
  c_args: andromedafx_c_args,
  dependencies: andromedafx_deps,
  include_directories: [andromedafx_inc, andromedafx_internal_inc],
  override_options: [
    'c_std=' + (meson.version().version_compare('>=1.4.0') ? 'c23,c11' : 'c11'),
  ],
  install: false,
)

# link_whole: the scene graph registers node type handlers and format tables
# through constructors and tables the compositor never names directly, so
# archive member pruning would drop them.
andromedafx_dep = declare_dependency(
  link_whole: andromedafx_lib,
  dependencies: andromedafx_deps,
  include_directories: andromedafx_inc,
)

if build_tests
  andromedafx_color_test = executable(
    'andromedafx-color-test',
    'tests/color.c',
    c_args: andromedafx_c_args,
    dependencies: andromedafx_deps,
    include_directories: [andromedafx_inc, andromedafx_internal_inc],
    link_with: andromedafx_lib,
    build_by_default: false,
  )
  foreach case_name : [
    'implicit-srgb-target',
    'scene-unset-primaries',
    'scene-custom-color',
    'scene-custom-white',
    'scene-custom-bt1886',
    'scene-scrgb-absolute-luminance',
    'scene-hdr-sdr-fallback',
    'hdr-white-point-math',
    'hdr-white-point',
    'sdr-white-point-alpha',
    'sdr-curve-consistency',
    'sdr-transfer-policy',
    'optimized-blur-sdr-curve',
    'optimized-blur-encoded-cache',
    'optimized-blur-size-change',
    'optimized-blur-copy-failure',
    'gamma22-to-srgb',
    'pq-roundtrip',
    'bt2020-to-srgb',
    'scrgb-negative-channels',
    'fp16-blending',
    'fp16-blur',
    'fp16-blur-effects',
    'fp16-save-restore',
    'sdr10-blur-buffer-format',
    'optimized-blur-format-cache',
    'shared-output-buffers',
    'output-lut-cache',
    'shared-sdr-capture',
    'shared-output-transform-transition',
    'shared-sdr-capture-lock',
    'shared-sdr-capture-storage-change',
    'capture-read-format',
    'framebuffer-texture-binding',
  ]
    test(
      'color-' + case_name,
      andromedafx_color_test,
      args: case_name,
      suite: 'andromedafx',
    )
  endforeach

  andromedafx_capture_pacing_test = executable(
    'andromedafx-capture-pacing-test',
    'tests/capture_pacing.c',
    c_args: andromedafx_c_args,
    dependencies: andromedafx_deps,
    include_directories: [andromedafx_inc, andromedafx_internal_inc],
    link_with: andromedafx_lib,
    build_by_default: false,
  )

  test(
    'capture-pacing',
    andromedafx_capture_pacing_test,
    suite: 'andromedafx',
  )

  andromedafx_effects_test = executable(
    'andromedafx-effects-test',
    'tests/effects.c',
    c_args: andromedafx_c_args,
    dependencies: andromedafx_deps,
    include_directories: [andromedafx_inc, andromedafx_internal_inc],
    link_with: andromedafx_lib,
    build_by_default: false,
  )
  foreach case_name : [
    'kinds',
    'reads',
    'uniforms',
    'long-running-trig',
    'expand',
    'renderer-destroy',
    'persistent-scene',
    'occlusion',
    'background-clear',
    'damage-confinement',
    'whole-box-invalidation',
    'transient-policy',
    'margin-damage',
    'move-margin-damage',
    'offscreen-margin',
    'effect-bounds',
    'border-geometry',
    'border-geometry-tree',
    'border-light',
    'border-light-hdr-low-alpha',
    'border-light-lifecycle',
    'border-light-motion',
    'visible-in-box',
    'in-place',
    'damage-expansion',
    'capture-policy',
    'capture-policy-encoding',
    'capture-feedback',
    'workspace-reveal',
    'in-place-feedback',
    'capture-composition',
    'in-place-shape',
    'output-effects',
    'audio-spectrum',
  ]
    test('effects-' + case_name, andromedafx_effects_test, args: case_name, suite: 'andromedafx')
  endforeach

  # Renderer ownership and descriptor stability against a real GPU. It takes
  # DRM render nodes as arguments, so it is a tool run by `just gpu-test`
  # rather than a suite entry. Built by default so the warning gate compiles
  # it.
  executable(
    'andromedafx-renderer-test',
    'tests/renderer.c',
    c_args: andromedafx_c_args,
    dependencies: andromedafx_deps,
    include_directories: [andromedafx_inc, andromedafx_internal_inc],
    link_with: andromedafx_lib,
  )

  if cc.get_define('WLR_HAS_GLES2_RENDERER', prefix: '#include <wlr/config.h>', dependencies: wlroots_dep) == '1'
    andromedafx_egl_abi_test = executable(
      'andromedafx-egl-abi-test',
      'tests/egl_abi.c',
      c_args: andromedafx_c_args,
      dependencies: andromedafx_deps,
      include_directories: andromedafx_internal_inc,
      export_dynamic: true,
      build_by_default: false,
    )
    test('andromedafx-egl-abi', andromedafx_egl_abi_test, suite: 'andromedafx')
  endif
  # andromedafx's scene structs must extend wlroots' as a strict prefix. One source,
  # built against each header, proves the offsets still agree.
  andromedafx_abi_probes = []
  foreach probe : [
    ['wlroots', '<wlr/types/wlr_scene.h>', [wlroots_dep]],
    ['andromedafx', '<andromedafx/types/wlr_scene.h>', [wlroots_dep, andromedafx_dep]],
  ]
    andromedafx_abi_probes += executable(
      'andromedafx-abi-probe-' + probe[0],
      'tests/abi.c',
      c_args: andromedafx_c_args + ['-DABI_PROBE_HEADER=' + probe[1]],
      dependencies: probe[2],
      build_by_default: false,
    )
  endforeach

  test(
    'scene-abi',
    find_program('tests/abi_compare.sh', native: true),
    args: andromedafx_abi_probes,
    suite: 'andromedafx',
  )
endif

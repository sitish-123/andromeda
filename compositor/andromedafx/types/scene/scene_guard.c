#define _GNU_SOURCE
#include "andromedafx/types/wlr_scene.h"

#include <dlfcn.h>
#include <stdio.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_output_layout.h>

// andromedafx's scene structs carry trailing fields wlroots knows nothing about,
// so a node a libwlroots helper allocated is short and andromedafx reads those
// fields past the end of it. Whether a borrowed helper resolves here or into
// libwlroots depends on how the distribution built libwlroots, and the failure
// is silent, so check the resolution once at startup rather than trusting the
// link. docs/design/scene-helper-ownership.md has the full account.

static const char* module_of(const void* fn, const void** base) {
  Dl_info info;
  if (dladdr(fn, &info) == 0 || info.dli_fbase == NULL) {
    *base = NULL;
    return "<unknown>";
  }
  *base = info.dli_fbase;
  return info.dli_fname != NULL ? info.dli_fname : "<anonymous>";
}

const char* andromedafx_scene_check_helpers(void) {
  static char message[512];

  // Taken from inside andromedafx, so it names whichever module this
  // translation unit was linked into.
  const void* want = NULL;
  const char* want_name = module_of((const void*)&andromedafx_scene_check_helpers, &want);

  static const struct {
    const char* name;
    const void* fn;
  } helpers[] = {
      {"wlr_scene_buffer_create", (const void*)&wlr_scene_buffer_create},
      {"wlr_scene_node_set_clip_region", (const void*)&wlr_scene_node_set_clip_region},
      {"wlr_scene_node_get_clip_region", (const void*)&wlr_scene_node_get_clip_region},
      {"wlr_ext_image_capture_source_v1_create_with_scene_node",
       (const void*)&wlr_ext_image_capture_source_v1_create_with_scene_node},
      {"wlr_scene_surface_create", (const void*)&wlr_scene_surface_create},
      {"wlr_scene_subsurface_tree_create", (const void*)&wlr_scene_subsurface_tree_create},
      {"wlr_scene_xdg_surface_create", (const void*)&wlr_scene_xdg_surface_create},
      {"wlr_scene_layer_surface_v1_create", (const void*)&wlr_scene_layer_surface_v1_create},
      {"wlr_scene_drag_icon_create", (const void*)&wlr_scene_drag_icon_create},
      {"wlr_scene_attach_output_layout", (const void*)&wlr_scene_attach_output_layout},
      {"wlr_scene_output_layout_add_output", (const void*)&wlr_scene_output_layout_add_output},
  };

  for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++) {
    const void* got = NULL;
    const char* got_name = module_of(helpers[i].fn, &got);
    if (got == want) {
      continue;
    }
    snprintf(
        message, sizeof(message),
        "%s resolves to %s instead of andromedafx (%s). Scene nodes would be "
        "allocated at wlroots' size and andromedafx would read its own fields "
        "past the end of them. andromedafx must define every scene helper; see "
        "andromedafx/README.md.",
        helpers[i].name, got_name, want_name
    );
    return message;
  }

  // dladdr resolving nothing at all leaves every module NULL, which would
  // compare equal above and pass an unchecked build.
  if (want == NULL) {
    snprintf(
        message, sizeof(message),
        "could not resolve the module backing andromedafx's scene helpers, so "
        "they went unverified."
    );
    return message;
  }

  return NULL;
}

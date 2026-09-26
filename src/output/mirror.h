#pragma once

extern "C" {
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>
}

struct wlr_buffer;
struct wlr_output;
struct wlr_renderer;

namespace umbriel {

  struct MirrorPlacement {
    wlr_box dst;                   // in the target's buffer coordinates
    wl_output_transform transform; // applied to the source texture
  };

  // Fits the source image inside the target, centred with letterboxing, correcting for both outputs' transforms.
  // Buffer sizes are the outputs' mode sizes; the source buffer holds the source image in its own transform.
  [[nodiscard]] MirrorPlacement mirrorPlacement(
      int sourceWidth, int sourceHeight, wl_output_transform sourceTransform, int targetWidth, int targetHeight,
      wl_output_transform targetTransform
  );

  // Renders `frame` onto `target` letterboxed on black and commits it. Returns false when rendering or the commit
  // fails.
  bool
  renderMirrorFrame(wlr_output* target, wlr_renderer* renderer, wlr_buffer* frame, wl_output_transform sourceTransform);

} // namespace umbriel

#pragma once

extern "C" {
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>
}

#include <functional>

struct wlr_buffer;
struct wlr_output;
struct wlr_output_state;
struct wlr_render_pass;
struct wlr_renderer;
struct wlr_texture;

namespace andromeda {

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

  // The layout region a device covering the whole target (a touchscreen) maps to so that the letterboxed image lands on
  // `sourceBox`, the source's layout box; the bars fall outside it. When the image is rotated relative to the target's
  // buffer no axis-aligned region fits, and this returns `sourceBox`. Empty for unconfigured outputs.
  [[nodiscard]] wlr_box mirrorInputRegion(
      const wlr_box& sourceBox, int sourceWidth, int sourceHeight, wl_output_transform sourceTransform, int targetWidth,
      int targetHeight, wl_output_transform targetTransform
  );

  // Draws `frame` letterboxed on black into `pass`, whose buffer is a target of `targetWidth` x `targetHeight` pixels.
  // Returns the texture the pass reads, for the caller to destroy after the submit; null when `frame` has none.
  [[nodiscard]] wlr_texture* drawMirrorFrame(
      wlr_render_pass* pass, wlr_renderer* renderer, wlr_buffer* frame, wl_output_transform sourceTransform,
      int targetWidth, int targetHeight, wl_output_transform targetTransform
  );

  // Renders `frame` onto `target` letterboxed on black and hands the state to `commit`. Returns false when rendering or
  // the commit fails.
  bool renderMirrorFrame(
      wlr_output* target, wlr_renderer* renderer, wlr_buffer* frame, wl_output_transform sourceTransform,
      const std::function<bool(const wlr_output_state&)>& commit
  );

} // namespace andromeda

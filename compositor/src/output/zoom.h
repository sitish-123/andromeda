#pragma once

#include "wlr.h"

namespace umbriel {

  // The screen magnifier's view: the part of the output, in output-local layout coordinates, that fills the output
  // at `factor`. It is centered on the pointer and pushed inward at the edges so it never leaves the output.
  [[nodiscard]] wlr_fbox zoomBox(double pointerX, double pointerY, double width, double height, double factor);

  // Scales `box` (output-local layout coordinates, `scale` buffer pixels per unit) of `frame` up to fill a buffer from
  // `swapchain`, which then replaces the frame in `state`. Returns false, leaving `state` as it was, if it cannot.
  bool renderZoom(
      wlr_renderer* renderer, wlr_swapchain* swapchain, wlr_output_state& state, const wlr_fbox& box, float scale
  );

} // namespace umbriel

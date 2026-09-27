#include "output/zoom.h"

#include <algorithm>

namespace umbriel {

  wlr_fbox zoomBox(double pointerX, double pointerY, double width, double height, double factor) {
    const double w = width / factor;
    const double h = height / factor;
    return {
        .x = std::clamp(pointerX - (w / 2.0), 0.0, width - w),
        .y = std::clamp(pointerY - (h / 2.0), 0.0, height - h),
        .width = w,
        .height = h,
    };
  }

  bool renderZoom(
      wlr_renderer* renderer, wlr_swapchain* swapchain, wlr_output_state& state, const wlr_fbox& box, float scale
  ) {
    if ((state.committed & WLR_OUTPUT_STATE_BUFFER) == 0 || state.buffer == nullptr) {
      return false;
    }
    wlr_texture* frame = wlr_texture_from_buffer(renderer, state.buffer);
    if (frame == nullptr) {
      return false;
    }
    wlr_buffer* target = wlr_swapchain_acquire(swapchain);
    if (target == nullptr) {
      wlr_texture_destroy(frame);
      return false;
    }
    wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(renderer, target, nullptr);
    bool ok = pass != nullptr;
    if (ok) {
      wlr_render_texture_options image{};
      image.texture = frame;
      image.src_box = {
          .x = box.x * scale,
          .y = box.y * scale,
          .width = box.width * scale,
          .height = box.height * scale,
      };
      image.dst_box = {.x = 0, .y = 0, .width = target->width, .height = target->height};
      image.filter_mode = WLR_SCALE_FILTER_BILINEAR;
      wlr_render_pass_add_texture(pass, &image);
      ok = wlr_render_pass_submit(pass);
    }
    wlr_texture_destroy(frame);
    if (ok) {
      wlr_output_state_set_buffer(&state, target);
    }
    wlr_buffer_unlock(target);
    return ok;
  }

} // namespace umbriel

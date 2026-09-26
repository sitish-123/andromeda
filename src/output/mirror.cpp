#include "output/mirror.h"

#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <utility>

extern "C" {
#include <wlr/util/transform.h>
}

namespace umbriel {

  namespace {

    // Odd wl_output_transform values rotate by 90 or 270 degrees.
    bool swapsAxes(wl_output_transform transform) { return (transform & 1) != 0; }

  } // namespace

  MirrorPlacement mirrorPlacement(
      int sourceWidth, int sourceHeight, wl_output_transform sourceTransform, int targetWidth, int targetHeight,
      wl_output_transform targetTransform
  ) {
    auto [imageWidth, imageHeight] = std::pair{sourceWidth, sourceHeight};
    if (swapsAxes(sourceTransform)) {
      std::swap(imageWidth, imageHeight);
    }
    auto [viewWidth, viewHeight] = std::pair{targetWidth, targetHeight};
    if (swapsAxes(targetTransform)) {
      std::swap(viewWidth, viewHeight);
    }

    MirrorPlacement placement{.dst = {}, .transform = WL_OUTPUT_TRANSFORM_NORMAL};
    if (imageWidth <= 0 || imageHeight <= 0 || viewWidth <= 0 || viewHeight <= 0) {
      return placement;
    }
    const double fit =
        std::min(static_cast<double>(viewWidth) / imageWidth, static_cast<double>(viewHeight) / imageHeight);
    const int width = static_cast<int>(std::lround(imageWidth * fit));
    const int height = static_cast<int>(std::lround(imageHeight * fit));
    const wlr_box viewBox{
        .x = (viewWidth - width) / 2,
        .y = (viewHeight - height) / 2,
        .width = width,
        .height = height,
    };
    wlr_box_transform(&placement.dst, &viewBox, wlr_output_transform_invert(targetTransform), viewWidth, viewHeight);
    placement.transform = wlr_output_transform_compose(wlr_output_transform_invert(sourceTransform), targetTransform);
    return placement;
  }

  bool renderMirrorFrame(
      wlr_output* target, wlr_renderer* renderer, wlr_buffer* frame, wl_output_transform sourceTransform
  ) {
    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_render_pass* pass = wlr_output_begin_render_pass(target, &state, nullptr);
    if (pass == nullptr) {
      wlr_output_state_finish(&state);
      return false;
    }

    wlr_render_rect_options background{};
    background.box = {.x = 0, .y = 0, .width = target->width, .height = target->height};
    background.color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
    wlr_render_pass_add_rect(pass, &background);
    wlr_texture* texture = wlr_texture_from_buffer(renderer, frame);
    if (texture != nullptr) {
      const MirrorPlacement placement = mirrorPlacement(
          frame->width, frame->height, sourceTransform, target->width, target->height, target->transform
      );
      wlr_render_texture_options image{};
      image.texture = texture;
      image.dst_box = placement.dst;
      image.transform = placement.transform;
      image.filter_mode = WLR_SCALE_FILTER_BILINEAR;
      wlr_render_pass_add_texture(pass, &image);
    }
    bool ok = wlr_render_pass_submit(pass);
    if (texture != nullptr) {
      wlr_texture_destroy(texture);
    }
    ok = ok && wlr_output_commit_state(target, &state);
    wlr_output_state_finish(&state);
    return ok;
  }

} // namespace umbriel

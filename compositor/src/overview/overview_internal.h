#pragma once

// Shared by the overview translation units (controller, cards, mirrors, input, drag); not part of the API.
#include "overview/overview.h"

extern "C" {
#include <andromedafx/render/effect.h>
}

#include "config/config.h"
#include "core/log.h"
#include "core/tracy.h"
#include "input/cursor.h"
#include "input/event_time.h"
#include "input/gesture_physics.h"
#include "input/gestures.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/drop_target.h"
#include "layout/layout.h"
#include "layout/master.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/interact.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/effect_registry.h"
#include "scene/hint_rect.h"
#include "scene/ssd_frame.h"
#include "scene/surface_shape.h"
#include "scene/text_buffer.h"
#include "server/color_manager.h"
#include "server/desktop_shell.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace andromeda::ovimpl {
  inline constexpr Logger kLog("overview");
  inline constexpr double kMiddleScrollStepPx = 105.0;

  // Overview progress over which the desktop's shadows and pinned windows fade, starting from the desktop: about half
  // the zoom, so they leave with the windows instead of before them.
  inline constexpr double kDesktopChromeFade = 0.5;
  // Below this a card is neither drawn nor hit.
  inline constexpr float kHiddenOpacity = 0.004F;
  // A card the filter misses keeps this share of its opacity.
  inline constexpr float kFilterMissOpacity = 0.25F;

  static_assert(static_cast<int>(OverviewStyle::Grid) == static_cast<int>(ov::Style::Grid));
  inline ov::Style configuredStyle() { return static_cast<ov::Style>(config().overview.style); }

  inline std::string_view arrangementLabel(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Floating:
      return "Free";
    case LayoutMode::Scrolling:
      return "Scrolling";
    case LayoutMode::Dwindle:
      return "Split";
    case LayoutMode::Master:
      return "Main + Stack";
    }
    return "";
  }

  // The arrangement menu's rows, top to bottom (ov::kMenuRows).
  inline constexpr std::array<LayoutMode, ov::kMenuRows> kMenuModes{
      LayoutMode::Floating, LayoutMode::Scrolling, LayoutMode::Dwindle, LayoutMode::Master
  };

  inline ov::Arrangement arrangementOf(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Floating:
      return ov::Arrangement::Free;
    case LayoutMode::Scrolling:
      return ov::Arrangement::Scrolling;
    case LayoutMode::Dwindle:
    case LayoutMode::Master:
      return ov::Arrangement::Tiled;
    }
    return ov::Arrangement::Scrolling;
  }

  // Whole layout pixels, each edge rounded once (ov::snapEdges): a settled card sits on the pixel grid and a moving
  // one never steps an edge back.
  inline wlr_box layoutBox(const ov::Rect& rect, const wlr_box& output) {
    const ov::PixelBox box = ov::snapEdges({output.x + rect.x, output.y + rect.y, rect.w, rect.h});
    return {.x = box.x, .y = box.y, .width = box.width, .height = box.height};
  }

  inline ov::Rect localRect(const wlr_box& box, const wlr_box& output) {
    return {
        static_cast<double>(box.x - output.x), static_cast<double>(box.y - output.y), static_cast<double>(box.width),
        static_cast<double>(box.height)
    };
  }

  // A view with a box to draw: a tiled opener waiting for the arrange that places it, and a hidden tab sharing its
  // box with the tab on show, have none.
  inline bool laidOut(const View* view) {
    const wlr_box& geometry = view->geometryBox();
    const wlr_box& world = view->presentedBox();
    return geometry.width > 0
        && geometry.height > 0
        && world.width > 0
        && world.height > 0
        && !view->presentationSuppressed();
  }
  // Pointer travel that promotes a press on a card into a relocate drag.
  inline constexpr double kDragThreshold = 10.0;
  // Edge auto-scroll while dragging: the band along an output edge, the hold before the first row step, the step
  // period after it, and the pan speed of a scrolling workspace (no hold; it runs on the frame clock).
  inline constexpr double kEdgeZone = 72.0;
  inline constexpr int kEdgeDelayMs = 380;
  inline constexpr int kEdgeRepeatMs = 650;
  inline constexpr double kEdgePanPxPerMs = 0.55; // 22 px every 40 ms in the prototype, spread over every frame
  // Corner radius of the ring over the new-workspace target.
  inline constexpr int kPlusHintRadius = 14;
  // A drop on the middle of a tiled window swaps the two; the outer 28 % on each side inserts beside it (prototype),
  // and the outer 28 % at the top and bottom stacks above or below it.
  inline constexpr double kSwapZoneStart = 0.28;
  // How much of the focused border color mixes into the unfocused one for a landing target that is not the live one.
  inline constexpr float kLandingTargetBlend = 0.4F;

  inline std::array<float, 4> mixColor(const std::array<float, 4>& from, const std::array<float, 4>& to, float amount) {
    std::array<float, 4> out{};
    for (size_t index = 0; index < out.size(); ++index) {
      out[index] = std::lerp(from[index], to[index], amount);
    }
    return out;
  }

  inline bool boxContains(const wlr_box& box, double x, double y) {
    return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
  }

  // wlr_scene_rect colors are premultiplied; straight alpha renders as an
  // over-bright wash (scene/color.h).
  inline std::array<float, 4> tint(const std::array<float, 4>& base, double opacity) {
    std::array<float, 4> out{};
    // Overshooting curves push the overview progress past [0, 1].
    premultiplied(out.data(), base, static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
    return out;
  }
  // A scene buffer's opaque region is in its destination coordinates, so a scaled copy of a surface needs the
  // surface's region scaled with it. Edges round inward: a pixel the opaque area only partly covers stays
  // see-through.
  inline void setScaledOpaqueRegion(
      wlr_scene_buffer* buffer, const pixman_region32_t& source, int sourceWidth, int sourceHeight, int destWidth,
      int destHeight
  ) {
    pixman_region32_t scaled;
    pixman_region32_init(&scaled);
    if (sourceWidth > 0 && sourceHeight > 0) {
      const double sx = static_cast<double>(destWidth) / sourceWidth;
      const double sy = static_cast<double>(destHeight) / sourceHeight;
      int count = 0;
      const pixman_box32_t* rects = pixman_region32_rectangles(&source, &count);
      for (int index = 0; index < count; ++index) {
        const auto x1 = static_cast<int>(std::ceil(rects[index].x1 * sx));
        const auto y1 = static_cast<int>(std::ceil(rects[index].y1 * sy));
        const auto x2 = static_cast<int>(std::floor(rects[index].x2 * sx));
        const auto y2 = static_cast<int>(std::floor(rects[index].y2 * sy));
        if (x2 > x1 && y2 > y1) {
          pixman_region32_union_rect(&scaled, &scaled, x1, y1, x2 - x1, y2 - y1);
        }
      }
    }
    wlr_scene_buffer_set_opaque_region(buffer, &scaled);
    pixman_region32_fini(&scaled);
  }

  inline void layoutWorkspaceBackground(
      wlr_scene_rect* background, const wlr_box& box, int radius, const std::array<float, 4>& color
  ) {
    if (background == nullptr) {
      return;
    }
    if (color[3] <= 0.001F) {
      wlr_scene_node_set_enabled(&background->node, false);
      return;
    }

    wlr_scene_node_set_enabled(&background->node, true);
    wlr_scene_node_set_position(&background->node, box.x, box.y);
    wlr_scene_rect_set_size(background, box.width, box.height);
    wlr_scene_rect_set_color(background, color.data());
    wlr_scene_rect_set_clipped_region(background, clipped_region_get_default());
    wlr_scene_rect_set_corner_radii(background, corner_radii_all(radius));
  }

  // Cards are pure output: hit testing runs off Overview's own boxes, so scene
  // input must never land on them (Server::viewAt then sees layer surfaces only).
  inline bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }

  inline wlr_scene_buffer* sourceBufferForSurface(wlr_scene_node* node, wlr_surface* surface) {
    if (node->type == WLR_SCENE_NODE_BUFFER) {
      wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
      wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
      return sceneSurface != nullptr && sceneSurface->surface == surface ? buffer : nullptr;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
      return nullptr;
    }
    wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
    wlr_scene_node* child = nullptr;
    wl_list_for_each(child, &tree->children, link) {
      if (wlr_scene_buffer* buffer = sourceBufferForSurface(child, surface)) {
        return buffer;
      }
    }
    return nullptr;
  }

  // The size `surface` is shown at in its view, through the view's scene buffer `source`. It differs from the surface
  // size when the view draws the surface scaled.
  inline void viewSurfaceSize(wlr_scene_buffer* source, const wlr_surface* surface, int* width, int* height) {
    if (wlr_scene_surface* sceneSurface = source != nullptr ? wlr_scene_surface_try_from_buffer(source) : nullptr) {
      wlr_scene_surface_get_size(sceneSurface, width, height);
      return;
    }
    *width = surface->current.width;
    *height = surface->current.height;
  }
} // namespace andromeda::ovimpl

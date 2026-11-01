#pragma once

#include "layer/layer_surface.h"
#include "output/output.h"
#include "server/server.h"
#include "workspace/workspace.h"

// clang-format off
#include "wlr.h"
// clang-format on

#include <optional>

namespace andromeda::cursor_detail {

  // Panels (top/overlay) keep working inside the overview. Background- and bottom-layer surfaces are part of the
  // inert backdrop behind the filmstrip, so their clicks belong to the overview instead.
  inline bool overviewPassthroughLayer(const LayerSurface* layer) {
    if (layer == nullptr) {
      return false;
    }
    const uint32_t which = layer->layerSurface()->current.layer;
    return which == ZWLR_LAYER_SHELL_V1_LAYER_TOP || which == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
  }

  inline bool isXdgPopupSurface(wlr_surface* surface) {
    return surface != nullptr && wlr_xdg_popup_try_from_wlr_surface(wlr_surface_get_root_surface(surface)) != nullptr;
  }

  // The tab bar slot under a layout point on that output's active workspace. A bar is compositor-drawn, so the scene
  // hit test never reports it: what the hit test did find wins when it sits above the bar. Windows, their popups, X11
  // menus, and top or overlay panels do; background and bottom layer surfaces, a wallpaper, lie below it.
  inline std::optional<TabHit> tabBarHitAt(
      Server& server, double lx, double ly, const wlr_surface* hitSurface, const View* hitView,
      const LayerSurface* hitLayer
  ) {
    const bool covered =
        hitView != nullptr || (hitLayer != nullptr ? overviewPassthroughLayer(hitLayer) : hitSurface != nullptr);
    if (covered) {
      return std::nullopt;
    }
    Output* output = server.outputFromWlr(wlr_output_layout_output_at(server.outputLayout(), lx, ly));
    WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    Workspace* workspace = group != nullptr ? group->active() : nullptr;
    return workspace != nullptr ? workspace->tabs().tabAt(lx, ly) : std::nullopt;
  }

  // `scale` is the surface-local units per layout unit in `target`.
  inline bool surfaceLocalCoordinates(
      wlr_scene* scene, wlr_surface* target, double scale, double lx, double ly, double* sx, double* sy
  ) {
    if (target == nullptr) {
      return false;
    }

    struct SurfacePosition {
      wlr_surface* target;
      int x = 0;
      int y = 0;
      bool found = false;
    } position{target};

    wlr_scene_node_for_each_buffer(
        &scene->tree.node,
        [](wlr_scene_buffer* buffer, int x, int y, void* data) {
          auto* position = static_cast<SurfacePosition*>(data);
          if (position->found) {
            return;
          }
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface != nullptr && sceneSurface->surface == position->target) {
            position->x = x;
            position->y = y;
            position->found = true;
          }
        },
        &position
    );

    if (!position.found) {
      return false;
    }
    *sx = (lx - position.x) * scale;
    *sy = (ly - position.y) * scale;
    return true;
  }

} // namespace andromeda::cursor_detail

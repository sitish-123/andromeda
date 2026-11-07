#pragma once

#include "scene/surface_blur.h"
#include "server/dsk_surface.h"

#include <cstdint>

struct wlr_scene_buffer;
struct wlr_scene_tree;
struct wlr_surface;

namespace andromeda {

  // dsk_surface_v1.set_glass_mask (v13): glass in the shape of a client mask (clock digits) over a surface. A hidden
  // scene buffer holds the mask; a live blur above the surface content draws the blurred scene under it, with a light
  // tint, only where the mask alpha is at least 0.5 (the SceneFX alpha stencil).
  class GlassMask {
  public:
    // Matches the surface's committed mask (`state`: its dsk_surface_v1 state, may be null); `tree` holds the surface
    // tree, and the blur goes on top of it.
    void update(wlr_scene_tree* tree, wlr_surface* surface, const DskSurfaceManager::State* state);
    void hide();

  private:
    wlr_scene_buffer* m_mask = nullptr;
    uint32_t m_serial = 0;
    SurfaceBlur m_blur;
  };

} // namespace andromeda

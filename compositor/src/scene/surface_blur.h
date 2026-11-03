#pragma once

#include <pixman.h>
#include <optional>

struct fx_corner_radii;
struct wlr_box;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wlr_scene_tree;
struct wlr_surface;

namespace andromeda {

  struct SurfaceBlurOptions {
    float ignoreAlpha = 0.0F;
    std::optional<bool> enabled;
    std::optional<bool> optimized;
  };

  // True when the surface's opaque region does not cover surfaceBox, given in surface-local coordinates.
  [[nodiscard]] bool surfaceTransparent(wlr_surface* surface, const wlr_box& surfaceBox);

  // Owns the desired-state logic for one SceneFX backdrop-blur node. The node itself is a child of the owner's scene
  // tree and is freed by scene-tree teardown, never by this class (no destructor).
  class SurfaceBlur {
  public:
    // Creates/updates/disables the node to match config and surface state. nodeBox: content box in `parent` coordinates
    // (position + size of the node). surfaceBox: content box in surface-local coordinates (opaque-region test).
    // clipBox: optional visible subset in `parent` coordinates; nullptr = full nodeBox. surfaceOpacity: opacity applied
    // to the surface buffers, used only to decide whether an opaque client reveals the blur. maskSource: optional scene
    // buffer to use instead of finding the surface's regular scene buffer. clipRegion: optional exact blur area in
    // surface-local coordinates (a client blur region); the blur draws only inside it.
    void update(
        wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
        fx_corner_radii corners, const wlr_box* clipBox = nullptr, const SurfaceBlurOptions& options = {},
        float surfaceOpacity = 1.0F, wlr_scene_buffer* maskSource = nullptr,
        const pixman_region32_t* clipRegion = nullptr
    );
    // Opacity of the blur effect itself. Keep this independent from surface
    // opacity so compositor opacity matches equivalent client alpha.
    void setAlpha(float alpha);
    // Liquid glass strength, 0 to 1 (dsk_surface_v1 property liquid).
    void setLiquid(float liquid);
    // Forces a live blur of everything below, overriding the optimized option, for a surface drawn over scene
    // content the cached background blur lacks.
    void setLive(bool live);
    // Disable the node (unmap path); update() re-enables.
    void hide();
    // True while the blur node is shown.
    [[nodiscard]] bool active() const;

  private:
    wlr_scene_blur* m_node = nullptr;
    bool m_masked = false;
    bool m_optimized = false;
    bool m_live = false;
    float m_alpha = 1.0F;
    float m_liquid = 0.0F;
  };

} // namespace andromeda

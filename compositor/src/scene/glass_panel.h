#pragma once

#include "protocols/glass_material.h"

#include <array>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_blur;
struct wlr_scene_rect;
struct wlr_scene_shadow;
struct wlr_scene_tree;

namespace andromeda {
  // A compositor-drawn glass popover (rulebook 9.9): E3 shadow, live backdrop blur with the material's rim and lens
  // level, the scheme's thick tint at the slider's density, and a hairline stroke. The scene owns its nodes.
  class GlassPanel {
  public:
    GlassPanel(wlr_scene_tree* parent, glass::Kind kind);
    GlassPanel(const GlassPanel&) = delete;
    GlassPanel& operator=(const GlassPanel&) = delete;

    // Places the panel at `box` (layout coordinates) with corner `radius`, in the current scheme and glass slider;
    // `opacity` (0..1) fades every layer together for open and close motion.
    void place(const wlr_box& box, int radius, float opacity = 1.0F);
    void setEnabled(bool enabled);
    // A flat scrim of `color` (premultiplied) over a plain blur, with no shadow, rim or hairline: for dimming an area.
    void setScrim(const std::array<float, 4>& color);

  private:
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_shadow* m_shadow = nullptr;
    wlr_scene_blur* m_blur = nullptr;
    wlr_scene_rect* m_tint = nullptr;
    wlr_scene_rect* m_hairline = nullptr;
    glass::Kind m_kind;
    std::array<float, 4> m_scrim{};
    bool m_useScrim = false;
  };
} // namespace andromeda

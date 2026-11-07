#include "scene/glass_panel.h"

#include "config/config.h"
#include "design_tokens.h"
#include "scene/surface_blur.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>

namespace andromeda {
  namespace {
    std::array<float, 4> rgba(std::uint32_t value, float alpha) {
      const auto channel = [value](int shift) { return static_cast<float>((value >> shift) & 0xFFU) / 255.0F; };
      const float a = alpha;
      return {channel(24) * a, channel(16) * a, channel(8) * a, a};
    }
  } // namespace

  GlassPanel::GlassPanel(wlr_scene_tree* parent, glass::Kind kind) : m_tree(wlr_scene_tree_create(parent)), m_kind(kind) {
    if (m_tree == nullptr) {
      return;
    }
    const float clear[4]{};
    m_shadow = wlr_scene_shadow_create(m_tree, 1, 1, 0, 1.0F, clear);
    m_blur = wlr_scene_blur_create(m_tree, 1, 1);
    m_tint = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_hairline = wlr_scene_rect_create(m_tree, 1, 1, clear);
  }

  void GlassPanel::setEnabled(bool enabled) {
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, enabled);
    }
  }

  void GlassPanel::setScrim(const std::array<float, 4>& color) {
    m_scrim = color;
    m_useScrim = true;
  }

  void GlassPanel::place(const wlr_box& box, int radius) {
    if (m_tree == nullptr || m_shadow == nullptr || m_blur == nullptr || m_tint == nullptr || m_hairline == nullptr) {
      return;
    }
    const bool light = config().appearance.scheme == ColorScheme::Light;
    const auto& scheme = light ? Tokens::schemeLight : Tokens::schemeDark;
    const glass::Material material = configGlass(m_kind);

    // E3: key shadow 12 down, 32 blur; the hole keeps it from showing through the translucent glass.
    const auto& e3 = Tokens::elevationE3;
    const int sigma = static_cast<int>(std::lround(e3.keyBlur / 2.0F));
    const int offsetY = static_cast<int>(std::lround(e3.keyY));
    wlr_scene_node_set_enabled(&m_shadow->node, !m_useScrim);
    wlr_scene_node_set_position(&m_shadow->node, box.x - sigma, box.y - sigma + offsetY);
    wlr_scene_shadow_set_size(m_shadow, box.width + 2 * sigma, box.height + 2 * sigma);
    wlr_scene_shadow_set_blur_sigma(m_shadow, static_cast<float>(sigma));
    wlr_scene_shadow_set_corner_radius(m_shadow, radius);
    wlr_scene_shadow_set_color(m_shadow, rgba(scheme.shadow | 0xFFU, light ? e3.keyLight : e3.keyDark).data());
    const fx_corner_radii corners = corner_radii_new(radius, radius, radius, radius);
    wlr_scene_shadow_set_clipped_region(
        m_shadow,
        clipped_region{.area = {sigma, sigma - offsetY, box.width, box.height}, .corners = corners}
    );

    wlr_scene_node_set_position(&m_blur->node, box.x, box.y);
    wlr_scene_blur_set_size(m_blur, box.width, box.height);
    wlr_scene_blur_set_corner_radius(m_blur, radius);
    wlr_scene_blur_set_should_only_blur_bottom_layer(m_blur, false);
    wlr_scene_blur_set_liquid(m_blur, m_useScrim ? 0.0F : material.level);
    wlr_scene_blur_set_rim(m_blur, m_useScrim ? 0.0F : material.rim);
    wlr_scene_blur_set_strength(m_blur, material.blurStrength);
    wlr_scene_blur_set_alpha(m_blur, 1.0F);

    wlr_scene_node_set_position(&m_tint->node, box.x, box.y);
    wlr_scene_rect_set_size(m_tint, box.width, box.height);
    wlr_scene_rect_set_corner_radius(m_tint, radius);
    wlr_scene_rect_set_color(m_tint, (m_useScrim ? m_scrim : rgba(scheme.tintThick, material.tint)).data());

    // Hairline: the stroke colour with the inside cut away, 1 px wide.
    wlr_scene_node_set_enabled(&m_hairline->node, !m_useScrim);
    wlr_scene_node_set_position(&m_hairline->node, box.x, box.y);
    wlr_scene_rect_set_size(m_hairline, box.width, box.height);
    wlr_scene_rect_set_corner_radius(m_hairline, radius);
    const std::uint32_t stroke = scheme.stroke;
    wlr_scene_rect_set_color(m_hairline, rgba(stroke | 0xFFU, static_cast<float>(stroke & 0xFFU) / 255.0F).data());
    const int inner = std::max(0, radius - 1);
    wlr_scene_rect_set_clipped_region(
        m_hairline,
        clipped_region{
            .area = {1, 1, std::max(0, box.width - 2), std::max(0, box.height - 2)},
            .corners = corner_radii_new(inner, inner, inner, inner),
        }
    );
  }
} // namespace andromeda

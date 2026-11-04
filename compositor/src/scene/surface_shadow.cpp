#include "scene/surface_shadow.h"

#include "config/config.h"
#include "design_tokens.h"

#include <algorithm>
extern "C" {
#include <andromedafx/render/effect.h>
}
// clang-format off
#include <cstring> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

namespace andromeda {

  void SurfaceShadow::update(
      wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius, int topExtension,
      int originX, int originY
  ) {
    const auto& cfg = config().appearance.shadow;
    const ShadowStyle style = m_style.value_or(
        ShadowStyle{.softness = cfg.softness, .offsetX = cfg.offsetX, .offsetY = cfg.offsetY, .opacity = {}, .contactOpacity = {}}
    );
    std::array<float, 4> shadow = baseColor(style.opacity);

    // Decorated box origin (in parent coords): (-borderTotal, -borderTotal)
    // Decorated box size: (contentWidth + 2*borderTotal, contentHeight + 2*borderTotal)
    const int decWidth = contentWidth + 2 * borderTotal;
    const int decHeight = contentHeight + topExtension + 2 * borderTotal;

    const bool enabled = m_style.has_value() || m_enabled.value_or(cfg.enabled);
    const bool want = enabled && shadow[3] > 0.0F && contentWidth > 0 && contentHeight > 0;
    if (!want) {
      hide();
      return;
    }

    shadow[3] *= m_alpha;
    place(m_node, parent, style.softness, style.offsetX, style.offsetY, shadow, decWidth, decHeight, borderTotal,
          topExtension, originX, originY, cornerRadius);

    if (m_style.has_value() && (m_style->contactOpacity.light > 0.0F || m_style->contactOpacity.dark > 0.0F) &&
        m_style->contactSoftness > 0) {
      std::array<float, 4> contact = baseColor(m_style->contactOpacity);
      contact[3] *= m_alpha;
      place(m_contact, parent, m_style->contactSoftness, 0, m_style->contactOffsetY, contact, decWidth, decHeight,
            borderTotal, topExtension, originX, originY, cornerRadius);
    } else if (m_contact != nullptr) {
      wlr_scene_node_set_enabled(&m_contact->node, false);
    }
  }

  void SurfaceShadow::place(
      wlr_scene_shadow*& node, wlr_scene_tree* parent, int sigma, int offsetX, int offsetY,
      const std::array<float, 4>& color, int decWidth, int decHeight, int borderTotal, int topExtension, int originX,
      int originY, int cornerRadius
  ) {
    // Lazy create: first time only.
    if (node == nullptr) {
      node = wlr_scene_shadow_create(parent, 0, 0, 0, static_cast<float>(sigma), color.data());
      if (node == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&node->node);
    }

    // Node geometry: shadow extends sigma px beyond the decorated box on each side.
    int nodeWidth = decWidth + 2 * sigma;
    int nodeHeight = decHeight + 2 * sigma;
    int nodeX = originX - borderTotal - sigma + offsetX;
    int nodeY = originY - borderTotal - sigma + offsetY - topExtension;

    // Hole punched over the window+borders so the shadow never shows through
    // transparent surfaces. Node-local coordinates.
    wlr_box hole{
        .x = sigma - offsetX,
        .y = sigma - offsetY,
        .width = decWidth,
        .height = decHeight,
    };

    wlr_scene_node_set_position(&node->node, nodeX, nodeY);
    if (node->width != nodeWidth || node->height != nodeHeight) {
      wlr_scene_shadow_set_size(node, nodeWidth, nodeHeight);
    }

    // Sync parameters when changed.
    if (node->blur_sigma != static_cast<float>(sigma)) {
      wlr_scene_shadow_set_blur_sigma(node, static_cast<float>(sigma));
    }
    if (std::memcmp(node->color, color.data(), sizeof(node->color)) != 0) {
      wlr_scene_shadow_set_color(node, color.data());
    }
    if (node->corner_radius != cornerRadius) {
      wlr_scene_shadow_set_corner_radius(node, cornerRadius);
    }

    const fx_corner_radii corners = corner_radii_new(cornerRadius, cornerRadius, cornerRadius, cornerRadius);
    wlr_scene_shadow_set_clipped_region(node, clipped_region{.area = hole, .corners = corners});

    wlr_scene_node_set_enabled(&node->node, true);
  }

  void SurfaceShadow::setEnabled(std::optional<bool> enabled) { m_enabled = enabled; }

  void SurfaceShadow::setStyle(std::optional<ShadowStyle> style) { m_style = style; }

  void SurfaceShadow::hide() {
    if (m_node != nullptr) {
      wlr_scene_node_set_enabled(&m_node->node, false);
    }
    if (m_contact != nullptr) {
      wlr_scene_node_set_enabled(&m_contact->node, false);
    }
  }

  void SurfaceShadow::setAlpha(float alpha) {
    m_alpha = alpha;
    if (m_node != nullptr) {
      auto shadow = baseColor(m_style.has_value() ? m_style->opacity : ShadowOpacity{});
      shadow[3] *= m_alpha;
      wlr_scene_shadow_set_color(m_node, shadow.data());
    }
    if (m_contact != nullptr && m_style.has_value()) {
      auto shadow = baseColor(m_style->contactOpacity);
      shadow[3] *= m_alpha;
      wlr_scene_shadow_set_color(m_contact, shadow.data());
    }
  }

  std::array<float, 4> SurfaceShadow::baseColor(ShadowOpacity opacity) const {
    if (!m_style.has_value()) {
      return config().colors.shadow;
    }
    const bool light = config().appearance.scheme == ColorScheme::Light;
    const std::uint32_t rgba = light ? Tokens::schemeLight.shadow : Tokens::schemeDark.shadow;
    const auto channel = [rgba](int shift) { return static_cast<float>((rgba >> shift) & 0xFFU) / 255.0F; };
    return {channel(24), channel(16), channel(8), channel(0) * (light ? opacity.light : opacity.dark)};
  }

  void SurfaceShadow::reset() {
    m_node = nullptr;
    m_contact = nullptr;
  }

  void SurfaceShadow::setAnimationSource(wlr_scene_node* source) {
    if (m_node != nullptr) {
      wlr_scene_shadow_set_animation_source(m_node, source, config().colors.shadow.data());
    }
  }

  ShadowSnapshot SurfaceShadow::snapshot(wlr_scene_tree* parent, wlr_scene_node* source, bool inPool) const {
    if (m_node == nullptr || !m_node->node.enabled) {
      return {};
    }
    auto* tree = wlr_scene_tree_create(parent);
    if (tree == nullptr) {
      return {};
    }
    auto* node = wlr_scene_shadow_create(
        tree, m_node->width, m_node->height, m_node->corner_radius, m_node->blur_sigma, m_node->color
    );
    if (node == nullptr) {
      wlr_scene_node_destroy(&tree->node);
      return {};
    }
    wlr_scene_node_set_position(&tree->node, source->x, source->y);
    if (inPool) {
      wlr_scene_node_lower_to_bottom(&tree->node);
    } else {
      wlr_scene_node_place_below(&tree->node, source);
    }
    wlr_scene_node_set_position(&node->node, m_node->node.x, m_node->node.y);
    wlr_scene_shadow_set_clipped_region(node, m_node->clipped_region);
    wlr_scene_shadow_set_animation_source(node, source, config().colors.shadow.data());
    ShadowSnapshot result{
        .tree = tree,
        .node = node,
    };
    std::copy_n(m_node->color, 4, result.color.begin());
    return result;
  }
} // namespace andromeda

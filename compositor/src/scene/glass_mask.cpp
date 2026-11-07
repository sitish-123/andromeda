#include "scene/glass_mask.h"

#include "wlr.h"

namespace andromeda {

  namespace {
    // Frosted digits: the blurred scene mixed this far toward white reads as glass on a dimmed backdrop.
    constexpr float kTint = 0.18F;
    constexpr float kAlphaTest = 0.5F;
  } // namespace

  void GlassMask::hide() {
    if (m_mask != nullptr) {
      wlr_scene_node_destroy(&m_mask->node);
      m_mask = nullptr;
    }
    m_serial = 0;
    m_blur.hide();
  }

  void GlassMask::update(wlr_scene_tree* tree, wlr_surface* surface, const DskSurfaceManager::State* state) {
    if (tree == nullptr || state == nullptr || state->glassMask == nullptr || wlr_box_empty(&state->glassMaskBox)) {
      hide();
      return;
    }
    if (m_mask == nullptr || m_serial != state->glassMaskSerial) {
      // A new node per mask: changing the blur's mask source damages it, a new buffer on a hidden node would not.
      if (m_mask != nullptr) {
        wlr_scene_node_destroy(&m_mask->node);
      }
      m_mask = wlr_scene_buffer_create(tree, state->glassMask);
      if (m_mask == nullptr) {
        m_blur.hide();
        return;
      }
      wlr_scene_node_set_enabled(&m_mask->node, false);
      m_serial = state->glassMaskSerial;
    }
    const wlr_box& box = state->glassMaskBox;
    m_blur.setMaterial(configGlass(glass::Kind::Shape));
    m_blur.setRim(0.0F);
    m_blur.setLens(0.0F);
    m_blur.setLive(true);
    m_blur.setTintMix({1.0F, 1.0F, 1.0F}, kTint);
    m_blur.update(
        tree, surface, box, box, corner_radii_none(), nullptr,
        SurfaceBlurOptions{.ignoreAlpha = kAlphaTest, .enabled = true, .optimized = false, .always = true}, 1.0F,
        m_mask
    );
    m_blur.raiseToTop();
  }

} // namespace andromeda

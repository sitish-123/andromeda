#include "scene/hint_rect.h"

#include "config/config.h"
#include "output/output.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "server/server.h"

// clang-format off
#include <algorithm>
#include <array>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace andromeda {

  namespace {
    constexpr int kFadeMs = 100;
    constexpr int kMorphMs = 75;
    constexpr int kRingWidth = 3;
    constexpr float kRingFill = 0.18F;
    constexpr int kGlowSigma = 8;
    constexpr float kGlowAlpha = 0.7F;

    bool sameBox(const wlr_box& left, const wlr_box& right) {
      return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
    }
  } // namespace

  HintRect::HintRect(Server& server, wlr_scene_tree* parent) : m_server(&server), m_parent(parent) {
    m_server->registerAnimatable(this);
  }

  HintRect::~HintRect() {
    m_server->unregisterAnimatable(this);
    hideImmediate();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_rect = nullptr;
      m_ring = nullptr;
      m_glow = nullptr;
    }
  }

  void HintRect::ensureScene() {
    if (m_tree != nullptr) {
      return;
    }
    m_tree = wlr_scene_tree_create(m_parent);
    float color[4]{};
    premultiplied(color, config().colors.insertHint, 0.0F);
    m_rect = wlr_scene_rect_create(m_tree, 1, 1, color);
    m_rect->accepts_input = false;
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  void HintRect::show(Output* output, const wlr_box& box, int cornerRadius, HintLook look) {
    if (box.width <= 0 || box.height <= 0) {
      hideImmediate();
      return;
    }

    ensureScene();
    if (cornerRadius != m_radius) {
      m_radius = cornerRadius;
      m_ringWidth = 0; // rebuild the ring for the new radius
    }
    wlr_scene_rect_set_corner_radius(m_rect, cornerRadius);
    if (look != m_look) {
      m_look = look;
      m_ringWidth = 0;
      if (look == HintLook::Ring && m_ring == nullptr) {
        const float clear[4]{};
        // In its own tree, inset by its width, so the ring sits inside the box like the prototype's inset shadow.
        wlr_scene_tree* ringTree = wlr_scene_tree_create(m_tree);
        if (ringTree != nullptr) {
          wlr_scene_node_set_position(&ringTree->node, kRingWidth, kRingWidth);
          m_ring = wlr_scene_border_create(ringTree, clear, clear);
        }
      }
      if (look == HintLook::Glow && m_glow == nullptr) {
        const float clear[4]{};
        m_glow = wlr_scene_shadow_create(m_tree, 1, 1, 0, static_cast<float>(kGlowSigma), clear);
        if (m_glow != nullptr) {
          wlr_scene_node_lower_to_bottom(&m_glow->node);
        }
      }
      if (m_ring != nullptr) {
        wlr_scene_node_set_enabled(&m_ring->node.parent->node, look == HintLook::Ring);
      }
      if (m_glow != nullptr) {
        wlr_scene_node_set_enabled(&m_glow->node, look == HintLook::Glow);
      }
    }

    if (!m_visible) {
      wlr_scene_node_set_enabled(&m_tree->node, true);
      m_x.snap(box.x);
      m_y.snap(box.y);
      m_w.snap(box.width);
      m_h.snap(box.height);
      m_targetBox = box;
      m_alpha.snap(0.0);
      m_alpha.retarget(1.0, kFadeMs, Easing::EaseOutCubic);
      m_visible = true;
    } else if (output != m_output) {
      m_x.snap(box.x);
      m_y.snap(box.y);
      m_w.snap(box.width);
      m_h.snap(box.height);
      m_targetBox = box;
    } else if (!sameBox(box, m_targetBox)) {
      retargetGeometry(box);
      m_targetBox = box;
    }

    m_output = output;
    applyState();
    wlr_scene_node_raise_to_top(&m_tree->node);
    wlr_output_schedule_frame(output->wlr());
  }

  void HintRect::hideImmediate() {
    m_alpha.snap(0.0);
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    m_visible = false;
    m_output = nullptr;
  }

  bool HintRect::tickAnimations(double nowMsec) {
    const bool alphaTicked = m_alpha.tick(nowMsec);
    const bool xTicked = m_x.tick(nowMsec);
    const bool yTicked = m_y.tick(nowMsec);
    const bool widthTicked = m_w.tick(nowMsec);
    const bool heightTicked = m_h.tick(nowMsec);
    const bool geometryTicked = xTicked || yTicked || widthTicked || heightTicked;
    if (alphaTicked || geometryTicked) {
      applyState();
    }
    return hasActiveAnimations();
  }

  bool HintRect::hasActiveAnimations() const {
    return m_alpha.animating() || m_x.animating() || m_y.animating() || m_w.animating() || m_h.animating();
  }

  void HintRect::retargetGeometry(const wlr_box& box) {
    m_x.retarget(box.x, kMorphMs, Easing::EaseOutCubic);
    m_y.retarget(box.y, kMorphMs, Easing::EaseOutCubic);
    m_w.retarget(box.width, kMorphMs, Easing::EaseOutCubic);
    m_h.retarget(box.height, kMorphMs, Easing::EaseOutCubic);
  }

  void HintRect::applyState() {
    wlr_scene_node_set_position(&m_tree->node, std::lround(m_x.current()), std::lround(m_y.current()));
    const int width = static_cast<int>(std::max(1L, std::lround(m_w.current())));
    const int height = static_cast<int>(std::max(1L, std::lround(m_h.current())));
    wlr_scene_rect_set_size(m_rect, width, height);
    const auto alpha = static_cast<float>(m_alpha.current());
    const std::array<float, 4>& hint = config().colors.insertHint;
    // Ring and Glow draw the insert-hint colour at full strength, so a user's own hint colour carries into them.
    std::array<float, 4> solid = hint;
    solid[3] = 1.0F;
    float color[4]{};
    switch (m_look) {
    case HintLook::Fill:
      premultiplied(color, hint, alpha);
      break;
    case HintLook::Ring:
      premultiplied(color, solid, alpha * kRingFill);
      break;
    case HintLook::Glow:
      premultiplied(color, solid, alpha);
      break;
    }
    wlr_scene_rect_set_color(m_rect, color);

    if (m_look == HintLook::Ring && m_ring != nullptr) {
      // The ring's geometry is rebuilt only while the box changes size, not on every fade step.
      if (width != m_ringWidth || height != m_ringHeight) {
        m_ringWidth = width;
        m_ringHeight = height;
        const int innerWidth = std::max(1, width - 2 * kRingWidth);
        const int innerHeight = std::max(1, height - 2 * kRingWidth);
        applyBorderGeometry(m_ring, makeBorderRing(innerWidth, innerHeight, m_radius, kRingWidth, 0), kRingWidth, 0);
      }
      float ring[4]{};
      premultiplied(ring, solid, alpha);
      wlr_scene_border_set_colors(m_ring, ring, ring);
    } else if (m_look == HintLook::Glow && m_glow != nullptr) {
      wlr_scene_node_set_position(&m_glow->node, -kGlowSigma, -kGlowSigma);
      if (m_glow->width != width + 2 * kGlowSigma || m_glow->height != height + 2 * kGlowSigma) {
        wlr_scene_shadow_set_size(m_glow, width + 2 * kGlowSigma, height + 2 * kGlowSigma);
      }
      if (m_glow->corner_radius != m_radius) {
        wlr_scene_shadow_set_corner_radius(m_glow, m_radius);
      }
      float glow[4]{};
      premultiplied(glow, solid, alpha * kGlowAlpha);
      wlr_scene_shadow_set_color(m_glow, glow);
    }
  }

} // namespace andromeda

#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

// The one lens of a track (rulebook 6, P1): a lifted pane that travels between items. The edge toward the
// target leads on `snap`, the other edge trails on `trail`, so the pane stretches across the gap and settles.
// Lives in a parent that does not lay out its children; the caller owns the parent and the AnimationManager.
class LensTrack {
public:
  void attach(Node& parent, AnimationManager& animations, bool vertical, float scale) {
    m_scale = scale;
    m_startId = 0;
    m_endId = 0;
    m_start = m_targetStart;
    m_end = m_targetEnd;
    m_animations = &animations;
    m_vertical = vertical;
    auto shadow = ui::box({.visible = false, .participatesInLayout = false});
    shadow->setSoftness(Tokens::elevationE1.keyBlur * scale);
    m_shadow = static_cast<Box*>(parent.addChild(std::move(shadow)));
    auto lens = ui::box({.visible = false, .participatesInLayout = false});
    m_lens = static_cast<Box*>(parent.addChild(std::move(lens)));
    m_lens->setBorder(colorSpecFromRole(ColorRole::LensEdge), 0.5F * scale);
    m_lens->setFill(colorSpecFromRole(ColorRole::Lens));
    m_shadow->setFill(colorSpecFromRole(
        ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE1.keyLight : Tokens::elevationE1.keyDark
    ));
    if (m_shown) {
      apply();
    }
  }

  // Order the lens and its shadow under the track's items.
  void setZIndex(int z) {
    m_shadow->setZIndex(z);
    m_lens->setZIndex(z);
  }

  // `start`/`end` are main-axis edges; `cross`/`crossSize` place the pane on the other axis.
  void moveTo(float start, float end, float cross, float crossSize, float radius, bool animate) {
    if (m_lens == nullptr) {
      return;
    }
    if (m_shown && start == m_targetStart && end == m_targetEnd && cross == m_cross && crossSize == m_crossSize
        && radius == m_radius) {
      return;
    }
    m_cross = cross;
    m_crossSize = crossSize;
    m_radius = radius;
    m_targetStart = start;
    m_targetEnd = end;
    const bool first = !m_shown;
    m_shown = true;
    if (!animate || first || m_animations == nullptr) {
      cancel();
      m_start = start;
      m_end = end;
      apply();
      return;
    }
    const bool forward = end > m_end || start > m_start;
    const auto snap = Style::spring(Tokens::springSnap);
    const auto trail = Style::spring(Tokens::springTrail);
    m_startId = m_animations->drive(
        m_startId, m_start, start, forward ? trail : snap,
        [this](float v) {
          m_start = v;
          apply();
        },
        [this] { m_startId = 0; }, m_lens, 0.0F, Unit::Px, true
    );
    m_endId = m_animations->drive(
        m_endId, m_end, end, forward ? snap : trail,
        [this](float v) {
          m_end = v;
          apply();
        },
        [this] { m_endId = 0; }, m_lens, 0.0F, Unit::Px, true
    );
    if (m_startId == 0) {
      m_start = start;
    }
    if (m_endId == 0) {
      m_end = end;
    }
    apply();
  }

  void hide() {
    cancel();
    m_shown = false;
    if (m_lens != nullptr) {
      m_lens->setVisible(false);
      m_shadow->setVisible(false);
    }
  }

private:
  void cancel() {
    if (m_animations == nullptr) {
      return;
    }
    if (m_startId != 0) {
      m_animations->cancel(m_startId);
      m_startId = 0;
    }
    if (m_endId != 0) {
      m_animations->cancel(m_endId);
      m_endId = 0;
    }
  }

  void apply() {
    const float len = std::max(0.0F, m_end - m_start);
    const float w = m_vertical ? m_crossSize : len;
    const float h = m_vertical ? len : m_crossSize;
    const float x = m_vertical ? m_cross : m_start;
    const float y = m_vertical ? m_start : m_cross;
    const float r = std::min(m_radius, std::min(w, h) * 0.5F);
    m_lens->setVisible(true);
    m_lens->setRadius(r);
    m_lens->setPosition(x, y);
    m_lens->setFrameSize(w, h);
    m_shadow->setVisible(true);
    m_shadow->setRadius(r);
    m_shadow->setPosition(x, y + Tokens::elevationE1.keyY * m_scale);
    m_shadow->setFrameSize(w, h);
  }

  AnimationManager* m_animations = nullptr;
  Box* m_lens = nullptr;
  Box* m_shadow = nullptr;
  bool m_vertical = false;
  float m_scale = 1.0F;
  bool m_shown = false;
  float m_start = 0.0F;
  float m_end = 0.0F;
  float m_targetStart = 0.0F;
  float m_targetEnd = 0.0F;
  float m_cross = 0.0F;
  float m_crossSize = 0.0F;
  float m_radius = 0.0F;
  AnimationManager::Id m_startId = 0;
  AnimationManager::Id m_endId = 0;
};

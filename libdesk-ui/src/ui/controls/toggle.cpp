#include "ui/controls/toggle.h"

#include "core/input/keybind_matcher.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "render/scene/accessible.h"

#include <algorithm>
#include <memory>

Toggle::Toggle() {
  setAccessible({.role = AccessibleRole::ToggleButton, .checked = false, .actionName = "toggle"});
  setAlign(FlexAlign::Center);
  setDirection(FlexDirection::Horizontal);
  setMirrorInRtl(false);
  setBorder(colorSpecFromRole(ColorRole::Stroke), Style::borderWidth);

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this] {
    updatePressWiden();
    applyState();
  });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) {
    updatePressWiden();
    applyState();
  });
  area->setOnCancel([this] { updatePressWiden(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled) {
      return;
    }
    activateFromInput();
  });
  area->setFocusable(true);
  area->setOnFocusGain([this] { applyState(); });
  area->setOnFocusLoss([this] { applyState(); });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      activateFromInput();
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);

  applySize();
  applyState();
  m_paletteConn = paletteChanged().connect([this] { applyAnimatedState(m_animationProgress); });
}

void Toggle::setChecked(bool checked) {
  accessibleState().checked = checked;
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;

  if (animationManager() != nullptr) {
    m_animId = animationManager()->drive(
        m_animId, m_animationProgress, m_checked ? 1.0F : 0.0F, Style::spring(Tokens::springSnap),
        [this](float t) { applyAnimatedState(t); }, [this] { m_animId = 0; }, this, 0.0F, Unit::Scale, true
    );
    // P4: the track colour runs on fade, apart from the knob's snap travel.
    m_colorAnimId = animationManager()->drive(
        m_colorAnimId, m_colorProgress, m_checked ? 1.0F : 0.0F, Style::spring(Tokens::springFade),
        [this](float t) {
          m_colorProgress = t;
          applyAnimatedState(m_animationProgress);
        },
        [this] { m_colorAnimId = 0; }, this, 0.0F, Unit::Progress, true
    );
    // Mark dirty so the surface's frame loop restarts and ticks the animation
    markPaintDirty();
  } else {
    applyState();
  }
}

void Toggle::setCheckedImmediate(bool checked) {
  accessibleState().checked = checked;
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;
  if (m_animId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animId);
    m_animId = 0;
  }
  if (m_colorAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_colorAnimId);
    m_colorAnimId = 0;
  }
  applyState();
}

void Toggle::setEnabled(bool enabled) {
  accessibleState().disabled = !enabled;
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyState();
}

void Toggle::setToggleSize(ToggleSize size) {
  if (m_size == size) {
    return;
  }
  m_size = size;
  applySize();
  applyState();
}

void Toggle::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applySize();
  applyState();
  markLayoutDirty();
}

void Toggle::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void Toggle::setTabFocusKey(std::string key) {
  if (m_inputArea != nullptr) {
    m_inputArea->setTabFocusKey(std::move(key));
  }
}

void Toggle::activateFromInput() {
  const bool next = !m_checked;
  setChecked(next);
  if (m_onChange) {
    m_onChange(next);
  }
}

bool Toggle::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Toggle::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Toggle::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setFrameSize(width(), height());
  }
}

LayoutSize Toggle::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Toggle::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Toggle::applySize() {
  switch (m_size) {
  case ToggleSize::Small:
    m_thumbSize = Style::toggleThumbSizeSm * m_scale;
    m_inset = Style::toggleInsetSm * m_scale;
    m_travel = Style::toggleTravelSm * m_scale;
    break;
  case ToggleSize::Medium:
    m_thumbSize = Style::toggleThumbSizeMd * m_scale;
    m_inset = Style::toggleInsetMd * m_scale;
    m_travel = Style::toggleTravelMd * m_scale;
    break;
  case ToggleSize::Large:
    m_thumbSize = Style::toggleThumbSizeLg * m_scale;
    m_inset = Style::toggleInsetLg * m_scale;
    m_travel = Style::toggleTravelLg * m_scale;
    break;
  }

  m_thumb->setFrameSize(m_thumbSize, m_thumbSize);
  setRadius((m_thumbSize + (m_inset * 2.0F)) * 0.5F);
}

void Toggle::applyState() {
  if (m_animId == 0 && m_colorAnimId == 0) {
    m_colorProgress = m_checked ? 1.0F : 0.0F;
    applyAnimatedState(m_checked ? 1.0F : 0.0F);
  } else {
    applyAnimatedState(m_animationProgress);
  }
}

// P4: while pressed the knob widens toward its travel side, on snap.
void Toggle::updatePressWiden() {
  const float target = m_enabled && pressed() ? 1.0F : 0.0F;
  if (animationManager() == nullptr) {
    m_widen = target;
    applyAnimatedState(m_animationProgress);
    return;
  }
  m_widenAnimId = animationManager()->drive(
      m_widenAnimId, m_widen, target, Style::spring(Tokens::springSnap),
      [this](float w) {
        m_widen = w;
        applyAnimatedState(m_animationProgress);
      },
      [this] { m_widenAnimId = 0; }, this, 0.0F, Unit::Progress, true
  );
  markPaintDirty();
}

void Toggle::applyAnimatedState(float t) {
  m_animationProgress = t;
  const float mixT = std::clamp(m_colorProgress, 0.0F, 1.0F);
  const Color trackColor = lerpColor(colorForRole(ColorRole::Stroke), colorForRole(ColorRole::Accent), mixT);
  const Color thumbColor = lerpColor(colorForRole(ColorRole::OnAccent), colorForRole(ColorRole::OnAccent), mixT);
  const float along = Style::rtl() ? 1.0F - t : t;
  const float extra = Tokens::componentTogglePressWiden * m_scale * m_widen;
  const float thumbX = m_inset + m_travel * along - extra * along;
  const float thumbW = m_thumbSize + extra;
  ColorSpec borderColor = colorSpecFromRole(ColorRole::Stroke);

  if (m_enabled) {
    if (m_inputArea != nullptr && m_inputArea->focused()) {
      borderColor = focusRingColorSpec();
    } else if (hovered()) {
      borderColor = colorSpecFromRole(ColorRole::Fill3);
    } else if (m_checked) {
      borderColor = colorSpecFromRole(ColorRole::Accent);
    }
  }

  setFill(trackColor);
  setBorder(borderColor, m_inputArea != nullptr && m_inputArea->focused() ? Style::focusRingWidth : Style::borderWidth);
  m_thumb->setPosition(thumbX, m_inset);

  auto thumbStyle = m_thumb->style();
  thumbStyle.fillMode = FillMode::Solid;
  thumbStyle.radius = m_thumbSize * 0.5F;
  m_thumb->setFrameSize(thumbW, m_thumbSize);
  thumbStyle.softness = 1.0F;
  thumbStyle.borderWidth = 0.0F;
  thumbStyle.fill = thumbColor;
  m_thumb->setStyle(thumbStyle);

  // Padding keeps Flex size consistent regardless of thumb position
  const float rightPad = m_inset + m_travel - (thumbX - m_inset) - extra;
  setPadding(m_inset, rightPad, m_inset, thumbX);

  if (m_enabled) {
    setOpacity(1.0F);
  } else {
    setOpacity(0.55F);
  }
}

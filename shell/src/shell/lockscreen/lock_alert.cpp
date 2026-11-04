#include "shell/lockscreen/lock_alert.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>

namespace {

  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  // Dark scheme popover and danger (protocol/design-tokens.toml).
  constexpr Color kPopover = rgba(40.0F / 255.0F, 40.0F / 255.0F, 44.0F / 255.0F, 1.0F);
  constexpr Color kDanger = rgba(217.0F / 255.0F, 48.0F / 255.0F, 37.0F / 255.0F, 1.0F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 26.0F / 255.0F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kScrim = rgba(0.0F, 0.0F, 0.0F, 0.40F);
  constexpr Color kFocusRing = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);

  float snap(float v) { return std::round(v); }

  RectNode* addRect(Node& parent) {
    auto rect = std::make_unique<RectNode>();
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

  Label* addLabel(Node& parent, FontWeight weight) {
    auto label = std::make_unique<Label>();
    label->setFontWeight(weight);
    return static_cast<Label*>(parent.addChild(std::move(label)));
  }

  bool inside(const AuthRect& r, float x, float y) { return x >= r.x && x < r.right() && y >= r.y && y < r.bottom(); }

} // namespace

void LockAlert::build(Node& parent) {
  m_group = parent.addChild(std::make_unique<Node>());
  // Above the session chrome (z 7) and the editor's panels (z 9).
  m_group->setZIndex(20);
  m_scrim = addRect(*m_group);
  m_panel = addRect(*m_group);
  m_title = addLabel(*m_group, FontWeight::SemiBold);
  m_body = addLabel(*m_group, FontWeight::Normal);
  m_cancel = addRect(*m_group);
  m_cancelLabel = addLabel(*m_group, FontWeight::SemiBold);
  m_confirm = addRect(*m_group);
  m_confirmLabel = addLabel(*m_group, FontWeight::SemiBold);
}

void LockAlert::setState(State state) { m_state = std::move(state); }

void LockAlert::layout(Renderer& renderer, float sw, float sh, float scale) {
  if (m_group == nullptr) {
    return;
  }
  m_group->setVisible(shown());
  m_panelRect = m_cancelRect = m_confirmRect = {};
  if (!shown()) {
    return;
  }
  const float s = std::max(0.1F, scale);
  const float pad = 24.0F * s;
  const float w = 320.0F * s;
  const float inner = w - pad * 2.0F;
  m_scrim->setPosition(0.0F, 0.0F);
  m_scrim->setSize(sw, sh);
  m_scrim->setStyle(RoundedRectStyle{.fill = kScrim});

  m_title->setFontSize(Tokens::typeTitle3.size * s);
  m_title->setLineHeight(Tokens::typeTitle3.line * s);
  m_title->setColor(kLabel1);
  (void)m_title->setText(m_state.title);
  m_title->setMaxWidth(inner);
  m_title->setMaxLines(2);
  m_title->measure(renderer);
  m_body->setFontSize(13.0F * s);
  m_body->setLineHeight(20.0F * s);
  m_body->setColor(kLabel2);
  (void)m_body->setText(m_state.body);
  m_body->setMaxWidth(inner);
  m_body->setMaxLines(4);
  m_body->measure(renderer);
  const float button = 32.0F * s;
  const float h = pad + m_title->height() + 8.0F * s + m_body->height() + 24.0F * s + button + pad;
  m_panelRect = {snap((sw - w) * 0.5F), snap((sh - h) * 0.5F), w, snap(h)};
  m_panel->setPosition(m_panelRect.x, m_panelRect.y);
  m_panel->setSize(m_panelRect.w, m_panelRect.h);
  m_panel->setStyle(RoundedRectStyle{.fill = kPopover, .border = kStroke, .radius = 24.0F * s, .borderWidth = 0.5F * s});
  const float x = m_panelRect.x + pad;
  float y = m_panelRect.y + pad;
  m_title->setPosition(x, y);
  y += m_title->height() + 8.0F * s;
  m_body->setPosition(x, y);
  y += m_body->height() + 24.0F * s;

  const float bw = snap((inner - 8.0F * s) * 0.5F);
  m_cancelRect = {x, y, bw, button};
  m_confirmRect = {x + bw + 8.0F * s, y, bw, button};
  m_cancel->setPosition(m_cancelRect.x, y);
  m_cancel->setSize(bw, button);
  const float ring = 2.0F * s;
  m_cancel->setStyle(RoundedRectStyle{
      .fill = kFill2, .border = kFocusRing, .radius = 8.0F * s, .borderWidth = m_state.confirmFocused ? 0.0F : ring
  });
  m_confirm->setPosition(m_confirmRect.x, y);
  m_confirm->setSize(bw, button);
  m_confirm->setStyle(RoundedRectStyle{
      .fill = kDanger, .border = kFocusRing, .radius = button * 0.5F, .borderWidth = m_state.confirmFocused ? ring : 0.0F
  });
  for (auto [label, rect, text] : {std::tuple{m_cancelLabel, m_cancelRect, i18n::tr("lockscreen.alert.cancel")},
                                   std::tuple{m_confirmLabel, m_confirmRect, m_state.confirm}}) {
    label->setFontSize(Tokens::typeBody.size * s);
    label->setColor(kLabel1);
    (void)label->setText(text);
    label->setMaxWidth(bw - 16.0F * s);
    label->measure(renderer);
    label->setPosition(snap(rect.x + (rect.w - label->width()) * 0.5F), snap(rect.y + (rect.h - label->height()) * 0.5F));
  }
}

LockAlert::Hit LockAlert::hitTest(float x, float y) const {
  if (!shown()) {
    return Hit::None;
  }
  if (inside(m_cancelRect, x, y)) {
    return Hit::Cancel;
  }
  if (inside(m_confirmRect, x, y)) {
    return Hit::Confirm;
  }
  return inside(m_panelRect, x, y) ? Hit::Inside : Hit::None;
}

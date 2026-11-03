#include "shell/lockscreen/lock_call_card.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.56F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 26.0F / 255.0F);
  // Dark scheme danger and success (protocol/design-tokens.toml).
  constexpr Color kDanger = rgba(217.0F / 255.0F, 48.0F / 255.0F, 37.0F / 255.0F, 1.0F);
  constexpr Color kSuccess = rgba(63.0F / 255.0F, 185.0F / 255.0F, 80.0F / 255.0F, 1.0F);
  constexpr Color kWhite = rgba(1.0F, 1.0F, 1.0F, 1.0F);

  float snap(float v) { return std::round(v); }

  RectNode* addRect(Node& parent) {
    auto rect = std::make_unique<RectNode>();
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

  Label* addLabel(Node& parent, FontWeight weight) {
    auto label = std::make_unique<Label>();
    label->setFontWeight(weight);
    label->setEllipsize(TextEllipsize::End);
    return static_cast<Label*>(parent.addChild(std::move(label)));
  }

  Glyph* addGlyph(Node& parent) { return static_cast<Glyph*>(parent.addChild(std::make_unique<Glyph>())); }

  void centreGlyph(Renderer& renderer, Glyph* glyph, const char* name, float size, Color color, float x, float y, float box) {
    glyph->setGlyph(name);
    glyph->setGlyphSize(size);
    glyph->setColor(color);
    glyph->measure(renderer);
    glyph->setPosition(snap(x + (box - glyph->width()) * 0.5F), snap(y + (box - glyph->height()) * 0.5F));
  }

} // namespace

void LockCallCard::build(Node& parent) {
  m_group = parent.addChild(std::make_unique<Node>());
  // Above the auth column (z 6): the card may overlap the Glance sheet edge.
  m_group->setZIndex(7);
  m_bg = addRect(*m_group);
  m_avatar = addRect(*m_group);
  m_avatarGlyph = addGlyph(*m_group);
  m_title = addLabel(*m_group, FontWeight::SemiBold);
  m_detail = addLabel(*m_group, FontWeight::Normal);
  m_decline = addRect(*m_group);
  m_declineGlyph = addGlyph(*m_group);
  m_declineLabel = addLabel(*m_group, FontWeight::SemiBold);
  m_answer = addRect(*m_group);
  m_answerLabel = addLabel(*m_group, FontWeight::SemiBold);
}

void LockCallCard::setState(State state) { m_state = std::move(state); }

void LockCallCard::layout(Renderer& renderer, float cx, float bottom, float scale) {
  if (m_group == nullptr) {
    return;
  }
  const bool shown = !m_state.deviceId.empty();
  m_group->setVisible(shown);
  m_declineRect = {};
  if (!shown) {
    return;
  }
  const float s = std::max(0.1F, scale);
  const float pad = 16.0F * s;
  const float w = 400.0F * s;
  const float avatar = 48.0F * s;
  const float button = 40.0F * s;
  const float h = pad + avatar + pad + button + pad;
  const float x = snap(cx - w * 0.5F);
  const float y = snap(bottom - h);
  m_bg->setPosition(x, y);
  m_bg->setSize(w, h);
  m_bg->setStyle(RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 24.0F * s, .borderWidth = 0.5F * s});

  m_avatar->setPosition(x + pad, y + pad);
  m_avatar->setSize(avatar, avatar);
  m_avatar->setStyle(RoundedRectStyle{.fill = kFill2, .radius = avatar * 0.5F});
  centreGlyph(renderer, m_avatarGlyph, "phone-call", 24.0F * s, kLabel1, x + pad, y + pad, avatar);

  const float textX = x + pad + avatar + 12.0F * s;
  const float textW = x + w - pad - textX;
  const float titleLine = Tokens::typeTitle3.line * s;
  const float footLine = Tokens::typeFootnote.line * s;
  m_title->setFontSize(Tokens::typeTitle3.size * s);
  m_title->setLineHeight(titleLine);
  m_title->setColor(kLabel1);
  (void)m_title->setText(m_state.title);
  m_title->setMaxWidth(textW);
  m_title->measure(renderer);
  m_detail->setFontSize(Tokens::typeFootnote.size * s);
  m_detail->setLineHeight(footLine);
  m_detail->setColor(kLabel2);
  (void)m_detail->setText(m_state.detail);
  m_detail->setMaxWidth(textW);
  m_detail->measure(renderer);
  const float textTop = snap(y + pad + (avatar - titleLine - footLine) * 0.5F);
  m_title->setPosition(textX, textTop);
  m_detail->setPosition(textX, textTop + titleLine);

  // Two equal capsules, g 8.
  const float by = y + pad + avatar + pad;
  const float bw = snap((w - pad * 2.0F - 8.0F * s) * 0.5F);
  m_declineRect = {x + pad, by, bw, button};
  m_decline->setPosition(m_declineRect.x, by);
  m_decline->setSize(bw, button);
  m_decline->setStyle(RoundedRectStyle{.fill = kDanger, .radius = button * 0.5F});
  m_declineLabel->setFontSize(Tokens::typeBody.size * s);
  m_declineLabel->setColor(kWhite);
  (void)m_declineLabel->setText(i18n::tr("lockscreen.call.decline"));
  m_declineLabel->measure(renderer);
  const float glyphBox = 16.0F * s;
  const float declineContent = glyphBox + 8.0F * s + m_declineLabel->width();
  const float dx = snap(m_declineRect.x + (bw - declineContent) * 0.5F);
  centreGlyph(renderer, m_declineGlyph, "lock", 14.0F * s, kWhite, dx, by + (button - glyphBox) * 0.5F, glyphBox);
  m_declineLabel->setPosition(dx + glyphBox + 8.0F * s, snap(by + (button - m_declineLabel->height()) * 0.5F));

  const float ax = m_declineRect.x + bw + 8.0F * s;
  m_answer->setPosition(ax, by);
  m_answer->setSize(bw, button);
  m_answer->setStyle(RoundedRectStyle{.fill = kSuccess, .radius = button * 0.5F});
  m_answerLabel->setFontSize(Tokens::typeBody.size * s);
  m_answerLabel->setColor(kWhite);
  (void)m_answerLabel->setText(i18n::tr("lockscreen.call.answer-on-phone"));
  m_answerLabel->setMaxWidth(bw - 16.0F * s);
  m_answerLabel->measure(renderer);
  m_answerLabel->setPosition(snap(ax + (bw - m_answerLabel->width()) * 0.5F), snap(by + (button - m_answerLabel->height()) * 0.5F));
}

bool LockCallCard::declineHit(float x, float y) const {
  const AuthRect& r = m_declineRect;
  return !r.empty() && x >= r.x && x < r.right() && y >= r.y && y < r.bottom();
}

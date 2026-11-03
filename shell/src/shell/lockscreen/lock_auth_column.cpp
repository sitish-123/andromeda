#include "shell/lockscreen/lock_auth_column.h"

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
#include <numbers>
#include <utility>

namespace {

  // The lock screen is always the dark scheme over the wallpaper (rulebook 9.15): white text, dark glass.
  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kLabel3 = rgba(1.0F, 1.0F, 1.0F, 0.46F);
  constexpr Color kDanger = rgba(1.0F, 107.0F / 255.0F, 94.0F / 255.0F, 1.0F);
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.56F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kAccent = rgba(97.0F / 255.0F, 89.0F / 255.0F, 245.0F / 255.0F, 1.0F);
  constexpr Color kTextShadow = rgba(0.0F, 0.0F, 0.0F, 0.30F);
  constexpr Color kAvatarFrom = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  constexpr Color kAvatarTo = rgba(1.0F, 95.0F / 255.0F, 162.0F / 255.0F, 1.0F);

  Node* addGroup(Node& parent) { return parent.addChild(std::make_unique<Node>()); }

  RectNode* addRect(Node& parent) {
    auto rect = std::make_unique<RectNode>();
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

  Label* addLabel(Node& parent, FontWeight weight, bool shadow) {
    auto label = std::make_unique<Label>();
    label->setFontWeight(weight);
    if (shadow) {
      label->setShadow(kTextShadow, 0.0F, 1.0F);
    }
    return static_cast<Label*>(parent.addChild(std::move(label)));
  }

  Glyph* addGlyph(Node& parent) { return static_cast<Glyph*>(parent.addChild(std::make_unique<Glyph>())); }

  // First letter of the name (UTF-8 aware), upper-cased when ASCII.
  std::string initialOf(const std::string& name) {
    if (name.empty()) {
      return {};
    }
    const auto lead = static_cast<unsigned char>(name[0]);
    const std::size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    std::string out = name.substr(0, len);
    if (len == 1 && out[0] >= 'a' && out[0] <= 'z') {
      out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
  }

  std::string dots(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += "●";
    }
    return out;
  }

} // namespace

void LockAuthColumn::build(Node& root, AnimationManager& animations) {
  m_animations = &animations;
  m_group = addGroup(root);
  m_group->setZIndex(6);

  m_date = addLabel(*m_group, FontWeight::SemiBold, true);
  m_clock = addLabel(*m_group, FontWeight::Light, true);
  m_clock->setFontFeatures("tnum");

  m_card = addRect(*m_group);
  m_cardGlyph = addGlyph(*m_group);
  m_cardTitle = addLabel(*m_group, FontWeight::SemiBold, false);
  m_cardBody = addLabel(*m_group, FontWeight::Medium, false);

  m_avatar = addRect(*m_group);
  m_initial = addLabel(*m_group, FontWeight::SemiBold, false);
  m_name = addLabel(*m_group, FontWeight::SemiBold, true);

  m_capsuleGroup = addGroup(*m_group);
  m_capsule = addRect(*m_capsuleGroup);
  m_field = addLabel(*m_capsuleGroup, FontWeight::Medium, false);
  m_go = addRect(*m_capsuleGroup);
  m_goGlyph = addGlyph(*m_capsuleGroup);
  m_trailingGlyph = addGlyph(*m_capsuleGroup);

  m_hint = addLabel(*m_group, FontWeight::Medium, true);
  m_owner = addLabel(*m_group, FontWeight::Medium, true);
}

void LockAuthColumn::setState(State state) { m_state = std::move(state); }

void LockAuthColumn::layout(Renderer& renderer, float sw, float sh, float scale) {
  // An empty placeholder means the session is not locked (or blacked out): draw nothing.
  m_group->setVisible(!m_state.placeholder.empty());
  if (m_state.placeholder.empty()) {
    return;
  }
  const float s = std::max(0.1F, scale);
  const float cx = sw * 0.5F;
  const auto snap = [](float v) { return std::round(v); };

  // Capsule: 240 x 40, its centre at 3/4 of the height.
  const float capW = 240.0F * s;
  const float capH = 40.0F * s;
  const float capX = snap(cx - capW * 0.5F);
  const float capY = snap(sh * 0.75F - capH * 0.5F);
  m_capsuleRect = {capX, capY, capW, capH};

  m_capsuleGroup->setPosition(capX, capY);
  m_capsuleGroup->setOpacity(m_state.inputOff ? 0.6F : 1.0F);
  m_capsule->setPosition(0.0F, 0.0F);
  m_capsule->setSize(capW, capH);
  m_capsule->setStyle(
      RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = capH * 0.5F, .borderWidth = 0.5F * s}
  );

  const bool typing = m_state.passwordLength > 0;
  m_field->setFontSize((typing ? 15.0F : Tokens::typeBody.size) * s);
  m_field->setColor(typing ? kLabel1 : kLabel3);
  m_field->setText(typing ? dots(m_state.passwordLength) : m_state.placeholder);
  const float trailingW = m_state.trailing == Trailing::None ? 0.0F : 24.0F * s + 8.0F * s;
  m_field->setMaxWidth(capW - 16.0F * s - trailingW - 8.0F * s);
  m_field->setEllipsize(TextEllipsize::End);
  m_field->measure(renderer);
  m_field->setPosition(snap(16.0F * s), snap((capH - m_field->height()) * 0.5F));

  const bool go = m_state.trailing == Trailing::Go || m_state.trailing == Trailing::Checking;
  const float goSize = 24.0F * s;
  const float goX = capW - 8.0F * s - goSize;
  const float goY = (capH - goSize) * 0.5F;
  m_go->setVisible(go);
  m_goGlyph->setVisible(go);
  if (go) {
    m_go->setPosition(goX, goY);
    m_go->setSize(goSize, goSize);
    m_go->setStyle(RoundedRectStyle{.fill = kAccent, .radius = goSize * 0.5F});
    m_go->setOpacity(m_state.trailing == Trailing::Checking ? 0.5F : 1.0F);
    m_goGlyph->setGlyph("arrow-right");
    m_goGlyph->setGlyphSize(12.0F * s);
    m_goGlyph->setColor(rgba(1.0F, 1.0F, 1.0F, 1.0F));
    m_goGlyph->measure(renderer);
    m_goGlyph->setPosition(
        snap(goX + (goSize - m_goGlyph->width()) * 0.5F), snap(goY + (goSize - m_goGlyph->height()) * 0.5F)
    );
  }
  const bool symbol = m_state.trailing == Trailing::Face || m_state.trailing == Trailing::Fingerprint;
  m_trailingGlyph->setVisible(symbol);
  if (symbol) {
    m_trailingGlyph->setGlyph(m_state.trailing == Trailing::Face ? "face-id" : "fingerprint");
    m_trailingGlyph->setGlyphSize(16.0F * s);
    m_trailingGlyph->setColor(kLabel2);
    m_trailingGlyph->measure(renderer);
    m_trailingGlyph->setPosition(
        snap(goX + (goSize - m_trailingGlyph->width()) * 0.5F),
        snap((capH - m_trailingGlyph->height()) * 0.5F)
    );
  }

  // Name (title-3) 16 above the capsule, avatar 64 12 above the name.
  m_name->setFontSize(Tokens::typeTitle3.size * s);
  m_name->setLineHeight(Tokens::typeTitle3.line * s);
  m_name->setColor(kLabel1);
  m_name->setText(m_state.user);
  m_name->setMaxWidth(320.0F * s);
  m_name->setEllipsize(TextEllipsize::End);
  m_name->measure(renderer);
  const float nameH = Tokens::typeTitle3.line * s;
  const float nameY = capY - 16.0F * s - nameH;
  m_name->setPosition(snap(cx - m_name->width() * 0.5F), snap(nameY + (nameH - m_name->height()) * 0.5F));

  const float avatar = 64.0F * s;
  const float avatarY = snap(nameY - 12.0F * s - avatar);
  m_avatar->setPosition(snap(cx - avatar * 0.5F), avatarY);
  m_avatar->setSize(avatar, avatar);
  m_avatar->setStyle(
      RoundedRectStyle{
          .fill = kAvatarFrom,
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = GradientDirection::Horizontal,
          .gradientStops = {GradientStop{0.0F, kAvatarFrom}, GradientStop{1.0F, kAvatarTo}, GradientStop{1.0F, kAvatarTo},
                            GradientStop{1.0F, kAvatarTo}},
          .radius = avatar * 0.5F,
      }
  );
  m_initial->setFontSize(26.0F * s);
  m_initial->setColor(rgba(1.0F, 1.0F, 1.0F, 1.0F));
  m_initial->setText(initialOf(m_state.user));
  m_initial->measure(renderer);
  m_initial->setPosition(
      snap(cx - m_initial->width() * 0.5F), snap(avatarY + (avatar - m_initial->height()) * 0.5F)
  );

  // Hint 12 below the capsule; owner message 24 below the hint line. Both keep their place when empty.
  const float footLine = Tokens::typeFootnote.line * s;
  const float hintY = capY + capH + 12.0F * s;
  m_hint->setVisible(!m_state.hint.empty());
  if (!m_state.hint.empty()) {
    m_hint->setFontSize(Tokens::typeFootnote.size * s);
    m_hint->setLineHeight(footLine);
    m_hint->setColor(m_state.hintError ? kDanger : kLabel2);
    m_hint->setText(m_state.hint);
    m_hint->setMaxWidth(320.0F * s);
    m_hint->setEllipsize(TextEllipsize::End);
    m_hint->measure(renderer);
    m_hint->setPosition(snap(cx - m_hint->width() * 0.5F), snap(hintY));
  }
  m_owner->setVisible(!m_state.ownerMessage.empty());
  if (!m_state.ownerMessage.empty()) {
    m_owner->setFontSize(Tokens::typeFootnote.size * s);
    m_owner->setLineHeight(footLine);
    m_owner->setColor(kLabel2);
    m_owner->setText(m_state.ownerMessage);
    m_owner->setMaxWidth(320.0F * s);
    m_owner->setMaxLines(2);
    m_owner->setTextAlign(TextAlign::Center);
    m_owner->measure(renderer);
    m_owner->setPosition(snap(cx - m_owner->width() * 0.5F), snap(hintY + footLine + 24.0F * s));
  }

  // Reason card (lock from phone): 360 wide, 32 above the avatar.
  const bool card = !m_state.reasonTitle.empty();
  m_card->setVisible(card);
  m_cardGlyph->setVisible(card);
  m_cardTitle->setVisible(card);
  m_cardBody->setVisible(card);
  if (card) {
    const float pad = 16.0F * s;
    const float cardW = 360.0F * s;
    const float titleLine = Tokens::typeTitle3.line * s;
    const float cardH = pad * 2.0F + titleLine + footLine;
    const float cardX = snap(cx - cardW * 0.5F);
    const float cardY = snap(avatarY - 32.0F * s - cardH);
    m_card->setPosition(cardX, cardY);
    m_card->setSize(cardW, cardH);
    m_card->setStyle(RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 24.0F * s, .borderWidth = 0.5F * s});
    m_cardGlyph->setGlyph("device-mobile");
    m_cardGlyph->setGlyphSize(24.0F * s);
    m_cardGlyph->setColor(kLabel1);
    m_cardGlyph->measure(renderer);
    m_cardGlyph->setPosition(cardX + pad, snap(cardY + (cardH - m_cardGlyph->height()) * 0.5F));
    const float textX = cardX + pad + 24.0F * s + 12.0F * s;
    const float textW = cardX + cardW - pad - textX;
    m_cardTitle->setFontSize(Tokens::typeTitle3.size * s);
    m_cardTitle->setLineHeight(titleLine);
    m_cardTitle->setColor(kLabel1);
    m_cardTitle->setText(m_state.reasonTitle);
    m_cardTitle->setMaxWidth(textW);
    m_cardTitle->setEllipsize(TextEllipsize::End);
    m_cardTitle->measure(renderer);
    m_cardTitle->setPosition(textX, cardY + pad);
    m_cardBody->setFontSize(Tokens::typeFootnote.size * s);
    m_cardBody->setLineHeight(footLine);
    m_cardBody->setColor(kLabel2);
    m_cardBody->setText(m_state.reasonBody);
    m_cardBody->setMaxWidth(textW);
    m_cardBody->setEllipsize(TextEllipsize::End);
    m_cardBody->measure(renderer);
    m_cardBody->setPosition(textX, cardY + pad + titleLine);
  }

  // Clock: hero 80 / 80 with its top at 1/4 of the height; the date line 8 above it.
  const float heroLine = Tokens::typeHero.line * s;
  const float clockY = snap(sh * 0.25F);
  m_clock->setVisible(!m_state.time.empty());
  m_date->setVisible(!m_state.date.empty());
  m_clock->setFontSize(Tokens::typeHero.size * s);
  m_clock->setLineHeight(heroLine);
  m_clock->setColor(kLabel1);
  m_clock->setText(m_state.time);
  m_clock->measure(renderer);
  m_clock->setPosition(snap(cx - m_clock->width() * 0.5F), snap(clockY + (heroLine - m_clock->height()) * 0.5F));
  const float dateLine = Tokens::typeTitle3.line * s;
  m_date->setFontSize(Tokens::typeTitle3.size * s);
  m_date->setLineHeight(dateLine);
  m_date->setColor(kLabel1);
  m_date->setText(m_state.date);
  m_date->measure(renderer);
  const float dateY = clockY - 8.0F * s - dateLine;
  m_date->setPosition(snap(cx - m_date->width() * 0.5F), snap(dateY + (dateLine - m_date->height()) * 0.5F));

  m_shakeAmplitude = 8.0F * s;
}

void LockAuthColumn::playRejected() {
  if (m_animations == nullptr) {
    return;
  }
  m_animations->cancelForOwner(this);
  const float baseX = m_capsuleRect.x;
  m_animations->animateSpring(
      1.0F, 0.0F, Style::spring(Tokens::springSnap),
      [this, baseX](float v) {
        // Three swings that shrink as the spring settles.
        const float swing = std::sin((1.0F - v) * 3.0F * 2.0F * std::numbers::pi_v<float>);
        m_capsuleGroup->setPosition(baseX + std::round(m_shakeAmplitude * v * swing), m_capsuleRect.y);
      },
      [this] { m_capsuleGroup->setPosition(m_capsuleRect.x, m_capsuleRect.y); }, this
  );
}

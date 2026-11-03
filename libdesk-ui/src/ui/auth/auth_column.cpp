#include "ui/auth/auth_column.h"

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/animation/motion_service.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
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
  constexpr Color kRegular = rgba(28.0F / 255.0F, 28.0F / 255.0F, 32.0F / 255.0F, 0.40F);
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.56F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  // fill_1 (dark): the capsule inside the Glance sheet.
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 15.0F / 255.0F);
  constexpr Color kAccent = rgba(97.0F / 255.0F, 89.0F / 255.0F, 245.0F / 255.0F, 1.0F);
  // accent_text (dark): #8c85ff.
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
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

void AuthColumn::build(Node& root, AnimationManager& animations) {
  m_animations = &animations;
  m_group = addGroup(root);
  m_group->setZIndex(6);

  m_island = addRect(*m_group);
  m_batteryGlyph = addGlyph(*m_group);
  m_batteryText = addLabel(*m_group, FontWeight::SemiBold, true);
  m_batteryText->setFontFeatures("tnum");
  m_networkGlyph = addGlyph(*m_group);
  m_layoutText = addLabel(*m_group, FontWeight::SemiBold, true);

  m_glanceGroup = addGroup(*m_group);
  m_date = addLabel(*m_glanceGroup, FontWeight::SemiBold, true);
  for (Label*& line : m_clockLines) {
    line = addLabel(*m_glanceGroup, FontWeight::Light, true);
    line->setFontFeatures("tnum");
  }
  m_clockAccent = addLabel(*m_glanceGroup, FontWeight::Light, true);
  m_clockAccent->setFontFeatures("tnum");
  m_glanceHint = addLabel(*m_glanceGroup, FontWeight::Medium, true);

  m_columnGroup = addGroup(*m_group);
  m_sheet = addRect(*m_columnGroup);
  m_card = addRect(*m_columnGroup);
  m_cardGlyph = addGlyph(*m_columnGroup);
  m_cardTitle = addLabel(*m_columnGroup, FontWeight::SemiBold, false);
  m_cardBody = addLabel(*m_columnGroup, FontWeight::Medium, false);

  m_avatar = addRect(*m_columnGroup);
  m_initial = addLabel(*m_columnGroup, FontWeight::SemiBold, false);
  m_name = addLabel(*m_columnGroup, FontWeight::SemiBold, true);

  m_capsuleGroup = addGroup(*m_columnGroup);
  m_capsule = addRect(*m_capsuleGroup);
  m_field = addLabel(*m_capsuleGroup, FontWeight::Medium, false);
  m_go = addRect(*m_capsuleGroup);
  m_goGlyph = addGlyph(*m_capsuleGroup);
  m_trailingGlyph = addGlyph(*m_capsuleGroup);

  m_hint = addLabel(*m_columnGroup, FontWeight::Medium, true);
  m_owner = addLabel(*m_columnGroup, FontWeight::Medium, true);
}

void AuthColumn::setState(State state) { m_state = std::move(state); }

void AuthColumn::layout(Renderer& renderer, float sw, float sh, float scale) {
  layout(renderer, placeAuth({.sw = sw, .sh = sh, .scale = scale, .reasonCard = !m_state.reasonTitle.empty()}), sw, scale);
}

void AuthColumn::layout(Renderer& renderer, const AuthPlacement& place, float sw, float scale) {
  // An empty placeholder means the session is not locked (or blacked out): draw nothing.
  m_group->setVisible(!m_state.placeholder.empty());
  if (m_state.placeholder.empty()) {
    return;
  }
  const float s = std::max(0.1F, scale);
  m_place = place;
  const float cx = place.columnCx;
  const auto snap = [](float v) { return std::round(v); };

  const float capW = place.capsule.w;
  const float capH = place.capsule.h;
  const float capX = place.capsule.x;
  const float capY = place.capsule.y;
  m_capsuleRect = {capX, capY, capW, capH};

  m_capsuleGroup->setPosition(capX, capY);
  m_capsuleGroup->setOpacity(m_state.inputOff ? 0.6F : 1.0F);
  m_capsule->setPosition(0.0F, 0.0F);
  m_capsule->setSize(capW, capH);
  const bool inSheet = !place.sheet.empty();
  m_capsule->setStyle(
      RoundedRectStyle{
          .fill = inSheet ? kFill1 : kThick,
          .border = inSheet ? Color{} : kStroke,
          .radius = capH * 0.5F,
          .borderWidth = inSheet ? 0.0F : 0.5F * s,
      }
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

  // Name (title-3) 16 above the capsule, avatar 64 12 above the name; Minimal shows neither.
  m_name->setVisible(place.account);
  m_avatar->setVisible(place.account);
  m_initial->setVisible(place.account);
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
    // Glance: the sheet grows down to hold the owner message with its 24 bottom padding.
    if (!m_place.sheet.empty()) {
      m_place.sheet.h = snap(hintY + footLine + 24.0F * s + m_owner->height() + 24.0F * s - m_place.sheet.y);
    }
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

  // Status island: h 32, r 16, p 0 12 0 8, g 8, at top 8 / right 8; parts that have nothing to show drop out.
  {
    const Status& st = m_state.status;
    const float islandH = 32.0F * s;
    const float gap = 8.0F * s;
    struct Part {
      Node* node;
      bool show;
    };
    m_batteryGlyph->setVisible(!st.batteryGlyph.empty());
    m_batteryText->setVisible(!st.batteryText.empty());
    m_networkGlyph->setVisible(!st.networkGlyph.empty());
    m_layoutText->setVisible(!st.layoutLabel.empty());
    for (Glyph* glyph : {m_batteryGlyph, m_networkGlyph}) {
      glyph->setGlyph(glyph == m_batteryGlyph ? st.batteryGlyph : st.networkGlyph);
      glyph->setGlyphSize(16.0F * s);
      glyph->setColor(kLabel1);
      glyph->measure(renderer);
    }
    for (Label* label : {m_batteryText, m_layoutText}) {
      label->setFontSize(Tokens::typeHeadline.size * s);
      label->setLineHeight(Tokens::typeHeadline.line * s);
      label->setColor(kLabel1);
      label->setText(label == m_batteryText ? st.batteryText : st.layoutLabel);
      label->measure(renderer);
    }
    const std::array<Part, 4> parts{{{m_batteryGlyph, !st.batteryGlyph.empty()},
                                     {m_batteryText, !st.batteryText.empty()},
                                     {m_networkGlyph, !st.networkGlyph.empty()},
                                     {m_layoutText, !st.layoutLabel.empty()}}};
    float contentW = 0.0F;
    int shown = 0;
    for (const Part& part : parts) {
      if (part.show) {
        contentW += part.node->width();
        ++shown;
      }
    }
    m_island->setVisible(place.chrome && shown > 0);
    for (const Part& part : parts) {
      part.node->setVisible(place.chrome && part.show);
    }
    if (shown > 0) {
      contentW += gap * static_cast<float>(shown - 1);
      const float islandW = snap(8.0F * s + contentW + 12.0F * s);
      const float islandX = snap(sw - 8.0F * s - islandW);
      const float islandY = snap(8.0F * s);
      m_island->setPosition(islandX, islandY);
      m_island->setSize(islandW, islandH);
      m_island->setStyle(RoundedRectStyle{.fill = kRegular, .border = kStroke, .radius = islandH * 0.5F, .borderWidth = 0.5F * s});
      float x = islandX + 8.0F * s;
      for (const Part& part : parts) {
        if (!part.show) {
          continue;
        }
        part.node->setPosition(snap(x), snap(islandY + (islandH - part.node->height()) * 0.5F));
        x += part.node->width() + gap;
      }
    }
  }

  // Clock: hero role at the layout's size (line height = size); the date line 8 above it, or 8 below (Minimal).
  const float heroLine = place.clockSize;
  const float clockY = place.clockY;
  const auto lineX = [&](float width) { return snap(place.clockCentred ? place.clockX - width * 0.5F : place.clockX); };
  m_date->setVisible(place.clock && !m_state.date.empty());
  const std::vector<std::string> lines =
      m_state.clockLines.empty() ? std::vector<std::string>{m_state.time} : m_state.clockLines;
  const auto weight = static_cast<FontWeight>(m_state.clockWeight);
  const auto styleLine = [&](Label* label, const std::string& text, Color color) {
    label->setFontFamily(m_state.clockFamily);
    label->setFontWeight(weight);
    label->setFontSize(place.clockSize);
    label->setLineHeight(heroLine);
    label->setColor(color);
    label->setText(text);
    label->measure(renderer);
  };
  m_clockAccent->setVisible(false);
  for (std::size_t i = 0; i < m_clockLines.size(); ++i) {
    Label* label = m_clockLines[i];
    const bool shown = place.clock && i < lines.size() && i < static_cast<std::size_t>(place.clockLines)
        && !lines[i].empty();
    label->setVisible(shown);
    if (!shown) {
      continue;
    }
    const float y = clockY + heroLine * static_cast<float>(i);
    const std::size_t colon = lines[i].find(':');
    if (m_state.clockTwoTone && colon != std::string::npos) {
      styleLine(label, lines[i].substr(0, colon + 1), kLabel1);
      styleLine(m_clockAccent, lines[i].substr(colon + 1), kAccentText);
      m_clockAccent->setVisible(true);
      const float x = lineX(label->width() + m_clockAccent->width());
      label->setPosition(x, snap(y + (heroLine - label->height()) * 0.5F));
      m_clockAccent->setPosition(x + label->width(), snap(y + (heroLine - m_clockAccent->height()) * 0.5F));
      continue;
    }
    styleLine(label, lines[i], kLabel1);
    label->setPosition(lineX(label->width()), snap(y + (heroLine - label->height()) * 0.5F));
  }
  const float dateLine = Tokens::typeTitle3.line * s;
  m_date->setFontSize(Tokens::typeTitle3.size * s);
  m_date->setLineHeight(dateLine);
  m_date->setColor(place.dateSecondary ? kLabel2 : kLabel1);
  m_date->setText(m_state.date);
  m_date->measure(renderer);
  m_date->setPosition(lineX(m_date->width()), snap(place.date.y + (dateLine - m_date->height()) * 0.5F));

  // Glance hint: footnote label_2, centred, its line 72 above the bottom.
  m_glanceHint->setVisible(inSheet && !m_state.glanceHint.empty());
  if (inSheet && !m_state.glanceHint.empty()) {
    m_glanceHint->setFontSize(Tokens::typeFootnote.size * s);
    m_glanceHint->setLineHeight(footLine);
    m_glanceHint->setColor(kLabel2);
    m_glanceHint->setText(m_state.glanceHint);
    m_glanceHint->measure(renderer);
    m_glanceHint->setPosition(snap(sw * 0.5F - m_glanceHint->width() * 0.5F), place.glanceHintY);
  }

  m_shakeAmplitude = 8.0F * s;
  applySheet();
}

void AuthColumn::setSheetOpen(bool open, bool instant) {
  m_sheetOpen = open;
  const float target = open ? 1.0F : 0.0F;
  if (instant || m_animations == nullptr) {
    if (m_animations != nullptr && m_sheetAnim != 0) {
      m_animations->cancel(m_sheetAnim);
    }
    m_sheetAnim = 0;
    m_sheetT = target;
    applySheet();
    return;
  }
  // Reduced motion: a plain fade (applySheet keeps the full size).
  const bool reduced = MotionService::instance().reduced();
  const auto& spring = reduced ? (open ? Tokens::springFade : Tokens::springFadeOut)
                               : (open ? Tokens::springOpen : Tokens::springClose);
  m_sheetAnim = m_animations->drive(
      m_sheetAnim, m_sheetT, target, Style::spring(spring),
      [this](float t) {
        m_sheetT = t;
        applySheet();
      },
      [this] { m_sheetAnim = 0; }, m_columnGroup
  );
}

// Glance: the sheet Grows from 60 % of its size at the centre while the column fades in; the glance layer recedes
// to 40 % and its hint fades out. Other layouts: no sheet, everything at full opacity.
void AuthColumn::applySheet() {
  const AuthRect& sheet = m_place.sheet;
  if (sheet.empty()) {
    m_sheet->setVisible(false);
    m_columnGroup->setVisible(true);
    m_columnGroup->setOpacity(1.0F);
    m_glanceGroup->setOpacity(1.0F);
    return;
  }
  const float t = std::clamp(m_sheetT, 0.0F, 1.0F);
  const float k = MotionService::instance().reduced() ? 1.0F : 0.6F + 0.4F * m_sheetT;
  const float w = sheet.w * k;
  const float h = sheet.h * k;
  m_sheet->setVisible(t > 0.0F);
  m_sheet->setPosition(std::round(sheet.x + (sheet.w - w) * 0.5F), std::round(sheet.y + (sheet.h - h) * 0.5F));
  m_sheet->setSize(std::round(w), std::round(h));
  const float s = sheet.w / 360.0F;
  m_sheet->setStyle(RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 28.0F * s, .borderWidth = 0.5F * s});
  m_columnGroup->setOpacity(t);
  m_columnGroup->setVisible(t > 0.0F);
  m_glanceGroup->setOpacity(1.0F - 0.6F * t);
  m_glanceHint->setOpacity(1.0F - t);
}

void AuthColumn::playRejected() {
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

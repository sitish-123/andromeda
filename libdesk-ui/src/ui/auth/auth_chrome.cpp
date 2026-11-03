#include "ui/auth/auth_chrome.h"

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
#include <utility>

namespace {

  // Dark scheme over the wallpaper, as in auth_column.cpp (rulebook 9.15).
  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kRegular = rgba(28.0F / 255.0F, 28.0F / 255.0F, 32.0F / 255.0F, 0.40F);
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.56F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 0.12F);
  constexpr Color kAvatarFrom = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  constexpr Color kAvatarTo = rgba(1.0F, 95.0F / 255.0F, 162.0F / 255.0F, 1.0F);

  constexpr float kEdge = 24.0F;
  constexpr float kControl = 40.0F;
  constexpr float kGap = 8.0F;
  constexpr float kMenuWidth = 256.0F;
  constexpr float kMenuPad = 4.0F;
  constexpr float kRowHeight = 36.0F;
  constexpr float kAvatar = 24.0F;

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

  Glyph* addGlyph(Node& parent) { return static_cast<Glyph*>(parent.addChild(std::make_unique<Glyph>())); }

} // namespace

void AuthChrome::build(Node& root) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(7);
  for (Chip* chip : {&m_users, &m_session, &m_power}) {
    chip->bg = addRect(*m_group);
    chip->glyph = addGlyph(*m_group);
    chip->label = addLabel(*m_group, FontWeight::Medium);
  }
  m_users.glyph->setGlyph("users");
  m_users.label->setText("Other users");
  m_session.glyph->setGlyph("chevron-up");
  m_power.glyph->setGlyph("power");
  m_menuBg = addRect(*m_group);
}

void AuthChrome::setState(State state) { m_state = std::move(state); }

void AuthChrome::layoutChip(Renderer& renderer, Chip& chip, float x, float y, bool alignRight, float s) {
  const bool hasLabel = !chip.label->text().empty();
  const float h = kControl * s;
  chip.glyph->setGlyphSize(16.0F * s);
  chip.glyph->setColor(kLabel1);
  chip.glyph->measure(renderer);
  float w = h;
  if (hasLabel) {
    chip.label->setFontSize(Tokens::typeBody.size * s);
    chip.label->setColor(kLabel1);
    chip.label->measure(renderer);
    // Capsule: p 0 / 16, symbol 16, gap 8.
    w = std::round((16.0F + 16.0F + 8.0F + 16.0F) * s + chip.label->width());
  }
  const float left = alignRight ? x - w : x;
  chip.rect = {left, y, w, h};
  chip.bg->setPosition(left, y);
  chip.bg->setSize(w, h);
  chip.bg->setStyle(RoundedRectStyle{.fill = kRegular, .border = kStroke, .radius = h * 0.5F, .borderWidth = 0.5F * s});
  if (hasLabel) {
    // The session chip reads "Name ^": label first, chevron after it; the users chip leads with its symbol.
    const bool glyphFirst = &chip == &m_users;
    const float labelX = left + (glyphFirst ? (16.0F + 16.0F + 8.0F) * s : 16.0F * s);
    const float glyphX = glyphFirst ? left + 16.0F * s : labelX + chip.label->width() + 8.0F * s;
    chip.label->setPosition(std::round(labelX), std::round(y + (h - chip.label->height()) * 0.5F));
    chip.glyph->setPosition(std::round(glyphX), std::round(y + (h - chip.glyph->height()) * 0.5F));
  } else {
    chip.glyph->setPosition(
        std::round(left + (w - chip.glyph->width()) * 0.5F), std::round(y + (h - chip.glyph->height()) * 0.5F)
    );
  }
}

void AuthChrome::layout(Renderer& renderer, float sw, float sh, float scale) {
  const float s = std::max(0.1F, scale);
  const float y = std::round(sh - (kEdge + kControl) * s);
  const float right = std::round(sw - kEdge * s);

  layoutChip(renderer, m_power, right, y, true, s);
  m_session.label->setText(m_state.sessionLabel);
  const bool session = !m_state.sessionLabel.empty();
  for (auto* node : {static_cast<Node*>(m_session.bg), static_cast<Node*>(m_session.glyph),
                     static_cast<Node*>(m_session.label)}) {
    node->setVisible(session);
  }
  if (session) {
    layoutChip(renderer, m_session, m_power.rect.x - kGap * s, y, true, s);
  } else {
    m_session.rect = {};
  }
  for (auto* node :
       {static_cast<Node*>(m_users.bg), static_cast<Node*>(m_users.glyph), static_cast<Node*>(m_users.label)}) {
    node->setVisible(m_state.usersChip);
  }
  if (m_state.usersChip) {
    layoutChip(renderer, m_users, std::round(kEdge * s), y, false, s);
  } else {
    m_users.rect = {};
  }
  layoutMenu(renderer, sw, sh, s);
}

void AuthChrome::layoutMenu(Renderer& renderer, float /*sw*/, float /*sh*/, float s) {
  const bool open = m_state.menu != Menu::None && !m_state.items.empty();
  m_menuBg->setVisible(open);
  while (m_rows.size() < m_state.items.size()) {
    Row row;
    row.bg = addRect(*m_group);
    row.avatar = addRect(*m_group);
    row.initial = addLabel(*m_group, FontWeight::SemiBold);
    row.glyph = addGlyph(*m_group);
    row.label = addLabel(*m_group, FontWeight::Medium);
    m_rows.push_back(row);
  }
  m_rowRects.assign(m_state.items.size(), Rect{});
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    const Row& row = m_rows[i];
    const bool shown = open && i < m_state.items.size();
    for (auto* node : {static_cast<Node*>(row.bg), static_cast<Node*>(row.avatar), static_cast<Node*>(row.initial),
                       static_cast<Node*>(row.glyph), static_cast<Node*>(row.label)}) {
      node->setVisible(shown);
    }
  }
  if (!open) {
    m_menuRect = {};
    return;
  }

  // Power menu (rulebook 9.15): thick glass, r 20, p 4, w 256, 8 above its control and aligned with it.
  const Chip& anchor = m_state.menu == Menu::Power ? m_power : m_state.menu == Menu::Session ? m_session : m_users;
  const float w = kMenuWidth * s;
  const float h = (kMenuPad * 2.0F + kRowHeight * static_cast<float>(m_state.items.size())) * s;
  const float x = m_state.menu == Menu::Users ? anchor.rect.x : anchor.rect.x + anchor.rect.w - w;
  const float y = anchor.rect.y - kGap * s - h;
  m_menuRect = {x, y, w, h};
  m_menuBg->setPosition(x, y);
  m_menuBg->setSize(w, h);
  m_menuBg->setStyle(RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 20.0F * s, .borderWidth = 0.5F * s});

  for (std::size_t i = 0; i < m_state.items.size(); ++i) {
    const Item& item = m_state.items[i];
    const Row& row = m_rows[i];
    const float rx = x + kMenuPad * s;
    const float ry = y + (kMenuPad + kRowHeight * static_cast<float>(i)) * s;
    const float rw = w - kMenuPad * 2.0F * s;
    const float rh = kRowHeight * s;
    m_rowRects[i] = {rx, ry, rw, rh};
    const bool lit = static_cast<int>(i) == m_state.hover || static_cast<int>(i) == m_state.current;
    row.bg->setVisible(lit);
    row.bg->setPosition(rx, ry);
    row.bg->setSize(rw, rh);
    row.bg->setStyle(RoundedRectStyle{.fill = kFill2, .radius = 16.0F * s});

    float textX = rx + 12.0F * s;
    const bool avatar = !item.initial.empty();
    row.avatar->setVisible(avatar);
    row.initial->setVisible(avatar);
    row.glyph->setVisible(!avatar && !item.glyph.empty());
    if (avatar) {
      const float a = kAvatar * s;
      const float ay = std::round(ry + (rh - a) * 0.5F);
      row.avatar->setPosition(textX, ay);
      row.avatar->setSize(a, a);
      row.avatar->setStyle(RoundedRectStyle{
          .fill = kAvatarFrom,
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = GradientDirection::Horizontal,
          .gradientStops = {GradientStop{0.0F, kAvatarFrom}, GradientStop{1.0F, kAvatarTo},
                            GradientStop{1.0F, kAvatarTo}, GradientStop{1.0F, kAvatarTo}},
          .radius = a * 0.5F,
      });
      row.initial->setText(item.initial);
      row.initial->setFontSize(12.0F * s);
      row.initial->setColor(kLabel1);
      row.initial->measure(renderer);
      row.initial->setPosition(
          std::round(textX + (a - row.initial->width()) * 0.5F), std::round(ay + (a - row.initial->height()) * 0.5F)
      );
      textX += a + 12.0F * s;
    } else if (!item.glyph.empty()) {
      row.glyph->setGlyph(item.glyph);
      row.glyph->setGlyphSize(16.0F * s);
      row.glyph->setColor(kLabel1);
      row.glyph->measure(renderer);
      row.glyph->setPosition(std::round(textX), std::round(ry + (rh - row.glyph->height()) * 0.5F));
      textX += 16.0F * s + 12.0F * s;
    }
    row.label->setText(item.label);
    row.label->setFontSize(Tokens::typeBody.size * s);
    row.label->setColor(kLabel1);
    row.label->setMaxWidth(rx + rw - 12.0F * s - textX);
    row.label->measure(renderer);
    row.label->setPosition(std::round(textX), std::round(ry + (rh - row.label->height()) * 0.5F));
  }
}

AuthChrome::Hit AuthChrome::hitTest(float x, float y) const {
  if (m_state.menu != Menu::None && m_menuRect.contains(x, y)) {
    for (std::size_t i = 0; i < m_rowRects.size(); ++i) {
      if (m_rowRects[i].contains(x, y)) {
        return {Target::Item, static_cast<int>(i)};
      }
    }
    return {Target::Menu, -1};
  }
  if (m_power.rect.contains(x, y)) {
    return {Target::Power, -1};
  }
  if (!m_state.sessionLabel.empty() && m_session.rect.contains(x, y)) {
    return {Target::Session, -1};
  }
  if (m_state.usersChip && m_users.rect.contains(x, y)) {
    return {Target::Users, -1};
  }
  return {};
}

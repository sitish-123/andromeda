#include "shell/lockscreen/lock_editor_bar.h"

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
#include <utility>

namespace {

  // Dark scheme over the wallpaper (mock section 2, rulebook 9.15).
  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kLabel3 = rgba(1.0F, 1.0F, 1.0F, 0.46F);
  // The surface's own preview lies behind the bar, out of the compositor blur's reach: dark thick tint 0.92.
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.92F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 0.10F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 0.16F);
  constexpr Color kOn = rgba(1.0F, 1.0F, 1.0F, 0.92F);
  constexpr Color kOnText = rgba(20.0F / 255.0F, 20.0F / 255.0F, 24.0F / 255.0F, 1.0F);
  constexpr Color kAccent = rgba(97.0F / 255.0F, 89.0F / 255.0F, 245.0F / 255.0F, 1.0F);
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  constexpr Color kClear = rgba(0.0F, 0.0F, 0.0F, 0.0F);

  constexpr float kTop = 16.0F;
  constexpr float kBarHeight = 40.0F;
  constexpr float kBarPad = 4.0F;
  constexpr float kChipHeight = 32.0F;
  constexpr float kChipPad = 16.0F;
  constexpr float kPanelTop = 96.0F;
  constexpr float kPanelWidth = 880.0F;
  constexpr float kPanelPad = 16.0F;
  constexpr float kCardHeight = 132.0F;
  constexpr float kGap = 12.0F;

  constexpr std::array<const char*, LockEditorBar::kSegments> kSegmentKeys{
      "lockscreen.editor.slots", "lockscreen.editor.free", "lockscreen.editor.wallpaper", "lockscreen.editor.layout",
      "lockscreen.editor.theme"
  };

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

  void place(RectNode* node, float x, float y, float w, float h, RoundedRectStyle style) {
    node->setPosition(x, y);
    node->setSize(w, h);
    node->setStyle(style);
  }

} // namespace

bool LockEditorBar::segmentEnabled(int index) noexcept {
  const auto segment = static_cast<Segment>(index);
  return segment != Segment::Free && segment != Segment::Wallpaper;
}

std::vector<LockEditorBar::Hit> LockEditorBar::focusOrder(bool panelOpen) {
  std::vector<Hit> order;
  for (int i = 0; i < kSegments; ++i) {
    if (segmentEnabled(i)) {
      order.push_back({Target::Segment, i});
    }
  }
  order.push_back({Target::Done, -1});
  if (panelOpen) {
    for (int i = 0; i < kLayouts; ++i) {
      order.push_back({Target::LayoutCard, i});
    }
    for (int i = 0; i < kThemes; ++i) {
      order.push_back({Target::ThemeChip, i});
    }
  }
  return order;
}

void LockEditorBar::build(Node& root) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(10);
  m_barBg = addRect(*m_group);
  for (std::size_t i = 0; i < m_segments.size(); ++i) {
    m_segments[i].bg = addRect(*m_group);
    m_segments[i].label = addLabel(*m_group, FontWeight::Medium);
  }
  m_done.bg = addRect(*m_group);
  m_done.label = addLabel(*m_group, FontWeight::SemiBold);
  m_doneFocus = addRect(*m_group);
  m_panelBg = addRect(*m_group);
  m_layoutTitle = addLabel(*m_group, FontWeight::SemiBold);
  m_themeTitle = addLabel(*m_group, FontWeight::SemiBold);
  m_themeNote = addLabel(*m_group, FontWeight::Normal);
  for (Card& card : m_cards) {
    card.bg = addRect(*m_group);
    card.clock = addRect(*m_group);
    card.line = addRect(*m_group);
    card.label = addLabel(*m_group, FontWeight::Normal);
  }
  for (Chip& chip : m_themes) {
    chip.bg = addRect(*m_group);
    chip.label = addLabel(*m_group, FontWeight::Medium);
  }
}

void LockEditorBar::setState(const State& state) { m_state = state; }

bool LockEditorBar::focused(Target target, int index) const noexcept {
  return m_state.focus.target == target && m_state.focus.index == index;
}

void LockEditorBar::layoutChip(
    Renderer& renderer, Chip& chip, float x, float y, float s, bool on, bool enabled, bool focusedChip
) {
  chip.label->setFontSize(13.0F * s);
  chip.label->setFontWeight(on ? FontWeight::SemiBold : FontWeight::Medium);
  chip.label->setColor(on ? kOnText : enabled ? kLabel2 : kLabel3);
  chip.label->measure(renderer);
  const float h = kChipHeight * s;
  const float w = std::round(chip.label->width() + kChipPad * 2.0F * s);
  chip.rect = {x, y, w, h};
  const bool hover = m_state.hover == Hit{Target::Segment, static_cast<int>(&chip - m_segments.data())};
  const Color fill = on ? kOn : (hover && enabled) ? kFill1 : kClear;
  place(
      chip.bg, x, y, w, h,
      RoundedRectStyle{
          .fill = fill,
          .border = focusedChip ? kAccentText : kClear,
          .radius = h * 0.5F,
          .borderWidth = focusedChip ? 2.0F * s : 0.0F,
      }
  );
  chip.label->setPosition(std::round(x + kChipPad * s), std::round(y + (h - chip.label->height()) * 0.5F));
}

void LockEditorBar::layout(Renderer& renderer, float sw, float scale) {
  const float s = std::max(0.1F, scale);
  const int mode = static_cast<int>(m_state.mode);
  for (int i = 0; i < kSegments; ++i) {
    std::string text = i18n::tr(kSegmentKeys[static_cast<std::size_t>(i)]);
    if (static_cast<Segment>(i) == Segment::Layout || static_cast<Segment>(i) == Segment::Theme) {
      text += " ▾";
    }
    m_segments[static_cast<std::size_t>(i)].label->setText(text);
  }
  m_done.label->setText(i18n::tr("lockscreen.editor.done"));

  // Measure first: the bar hugs its chips and sits centred.
  float width = kBarPad * s;
  for (int i = 0; i < kSegments; ++i) {
    Chip& chip = m_segments[static_cast<std::size_t>(i)];
    layoutChip(renderer, chip, 0.0F, 0.0F, s, i == mode, segmentEnabled(i), false);
    width += chip.rect.w + kBarPad * s;
  }
  m_done.label->setFontSize(13.0F * s);
  m_done.label->measure(renderer);
  const float doneW = std::round(m_done.label->width() + kChipPad * 2.0F * s);
  width += 8.0F * s - kBarPad * s + doneW + kBarPad * s;

  const float barX = std::round((sw - width) * 0.5F);
  const float barY = std::round(kTop * s);
  const float barH = kBarHeight * s;
  m_barRect = {barX, barY, width, barH};
  place(m_barBg, barX, barY, width, barH,
        RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = barH * 0.5F, .borderWidth = 0.5F * s});

  float x = barX + kBarPad * s;
  const float chipY = barY + kBarPad * s;
  for (int i = 0; i < kSegments; ++i) {
    Chip& chip = m_segments[static_cast<std::size_t>(i)];
    layoutChip(renderer, chip, x, chipY, s, i == mode, segmentEnabled(i), focused(Target::Segment, i));
    x += chip.rect.w + kBarPad * s;
  }
  x += 8.0F * s - kBarPad * s;
  const float chipH = kChipHeight * s;
  m_done.rect = {x, chipY, doneW, chipH};
  place(m_done.bg, x, chipY, doneW, chipH, RoundedRectStyle{.fill = kAccent, .radius = chipH * 0.5F});
  m_done.label->setColor(rgba(1.0F, 1.0F, 1.0F, 1.0F));
  m_done.label->setPosition(std::round(x + kChipPad * s), std::round(chipY + (chipH - m_done.label->height()) * 0.5F));
  const bool doneFocus = focused(Target::Done, -1);
  m_doneFocus->setVisible(doneFocus);
  if (doneFocus) {
    const float g = 3.0F * s;
    place(m_doneFocus, x - g, chipY - g, doneW + g * 2.0F, chipH + g * 2.0F,
          RoundedRectStyle{.fill = kClear, .border = kAccentText, .radius = chipH * 0.5F + g, .borderWidth = 2.0F * s});
  }

  layoutPanel(renderer, sw, s);
}

void LockEditorBar::layoutPanel(Renderer& renderer, float sw, float s) {
  const bool open = m_state.panelOpen;
  for (Node* node : {static_cast<Node*>(m_panelBg), static_cast<Node*>(m_layoutTitle),
                     static_cast<Node*>(m_themeTitle), static_cast<Node*>(m_themeNote)}) {
    node->setVisible(open);
  }
  for (Card& card : m_cards) {
    for (Node* node : {static_cast<Node*>(card.bg), static_cast<Node*>(card.clock), static_cast<Node*>(card.line),
                       static_cast<Node*>(card.label)}) {
      node->setVisible(open);
    }
  }
  for (Chip& chip : m_themes) {
    chip.bg->setVisible(open);
    chip.label->setVisible(open);
  }
  if (!open) {
    m_panelRect = {};
    return;
  }

  const float w = std::min(kPanelWidth * s, sw - 32.0F * s);
  const float x = std::round((sw - w) * 0.5F);
  const float y = std::round(kPanelTop * s);
  const float pad = kPanelPad * s;
  float cy = y + pad;

  m_layoutTitle->setText(i18n::tr("lockscreen.editor.layout"));
  m_layoutTitle->setFontSize(Tokens::typeTitle3.size * s);
  m_layoutTitle->setColor(kLabel1);
  m_layoutTitle->measure(renderer);
  m_layoutTitle->setPosition(x + pad, cy);
  cy += m_layoutTitle->height() + kGap * s;

  const float cardW = std::floor((w - pad * 2.0F - kGap * s * static_cast<float>(kLayouts - 1)) / kLayouts);
  const float cardH = kCardHeight * s;
  for (int i = 0; i < kLayouts; ++i) {
    Card& card = m_cards[static_cast<std::size_t>(i)];
    const float cx = x + pad + static_cast<float>(i) * (cardW + kGap * s);
    card.rect = {cx, cy, cardW, cardH};
    const bool selected = i == m_state.layout;
    const bool ring = selected || focused(Target::LayoutCard, i);
    const bool hover = m_state.hover == Hit{Target::LayoutCard, i};
    place(card.bg, cx, cy, cardW, cardH,
          RoundedRectStyle{
              .fill = hover ? kFill2 : kFill1,
              .border = ring ? kAccentText : kClear,
              .radius = 16.0F * s,
              .borderWidth = ring ? (focused(Target::LayoutCard, i) && !selected ? 1.5F : 2.0F) * s : 0.0F,
          });
    // Thumbnail: where the clock and the date sit in this layout (mock section 2 Layout panel).
    float clockW = 64.0F * s;
    float clockH = 16.0F * s;
    float clockX = cx + (cardW - clockW) * 0.5F;
    float clockY = cy + 24.0F * s;
    if (i == 1) {
      clockW = 56.0F * s;
      clockH = 14.0F * s;
      clockX = cx + 16.0F * s;
      clockY = cy + 20.0F * s;
    } else if (i == 2) {
      clockW = 72.0F * s;
      clockH = 22.0F * s;
      clockX = cx + 16.0F * s;
      clockY = cy + 40.0F * s;
    } else if (i == 3) {
      clockW = 80.0F * s;
      clockH = 22.0F * s;
      clockX = cx + (cardW - clockW) * 0.5F;
      clockY = cy + 40.0F * s;
    }
    place(card.clock, std::round(clockX), std::round(clockY), clockW, clockH,
          RoundedRectStyle{.fill = kLabel2, .radius = 4.0F * s});
    const float lineW = i == 3 ? 0.0F : 96.0F * s;
    card.line->setVisible(lineW > 0.0F);
    const float lineX = i == 0 ? cx + (cardW - lineW) * 0.5F : clockX;
    place(card.line, std::round(lineX), std::round(clockY + clockH + 8.0F * s), lineW, 10.0F * s,
          RoundedRectStyle{.fill = kFill2, .radius = 5.0F * s});
    card.label->setText(i18n::tr(std::string("lockscreen.editor.layouts.") + kLayoutNames[static_cast<std::size_t>(i)]));
    card.label->setFontSize(Tokens::typeFootnote.size * s);
    card.label->setColor(kLabel2);
    card.label->setMaxWidth(cardW - 24.0F * s);
    card.label->measure(renderer);
    card.label->setPosition(std::round(cx + 12.0F * s), std::round(cy + cardH - 8.0F * s - card.label->height()));
  }
  cy += cardH + kPanelPad * s;

  m_themeTitle->setText(i18n::tr("lockscreen.editor.theme"));
  m_themeTitle->setFontSize(Tokens::typeTitle3.size * s);
  m_themeTitle->setColor(kLabel1);
  m_themeTitle->measure(renderer);
  m_themeTitle->setPosition(x + pad, cy);
  cy += m_themeTitle->height() + kGap * s;

  float tx = x + pad;
  for (int i = 0; i < kThemes; ++i) {
    Chip& chip = m_themes[static_cast<std::size_t>(i)];
    chip.label->setText(i18n::tr(std::string("lockscreen.editor.themes.") + kThemeNames[static_cast<std::size_t>(i)]));
    const bool on = i == m_state.theme;
    layoutChip(renderer, chip, tx, cy, s, on, true, focused(Target::ThemeChip, i));
    if (!on) {
      chip.bg->setStyle(RoundedRectStyle{
          .fill = m_state.hover == Hit{Target::ThemeChip, i} ? kFill2 : kFill1,
          .border = focused(Target::ThemeChip, i) ? kAccentText : kClear,
          .radius = chip.rect.h * 0.5F,
          .borderWidth = focused(Target::ThemeChip, i) ? 2.0F * s : 0.0F,
      });
    }
    tx += chip.rect.w + 8.0F * s;
  }
  m_themeNote->setText(i18n::tr(m_state.theme == 0 ? "lockscreen.editor.theme-note" : "lockscreen.editor.theme-note-ryoku"));
  m_themeNote->setFontSize(Tokens::typeFootnote.size * s);
  m_themeNote->setColor(kLabel2);
  m_themeNote->setMaxWidth(x + w - pad - tx - 8.0F * s);
  m_themeNote->measure(renderer);
  m_themeNote->setPosition(
      std::round(x + w - pad - m_themeNote->width()), std::round(cy + (kChipHeight * s - m_themeNote->height()) * 0.5F)
  );
  cy += kChipHeight * s + pad;

  m_panelRect = {x, y, w, cy - y};
  place(m_panelBg, x, y, w, cy - y,
        RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 28.0F * s, .borderWidth = 0.5F * s});
}

LockEditorBar::Hit LockEditorBar::hitTest(float x, float y) const {
  if (m_state.panelOpen && m_panelRect.contains(x, y)) {
    for (int i = 0; i < kLayouts; ++i) {
      if (m_cards[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::LayoutCard, i};
      }
    }
    for (int i = 0; i < kThemes; ++i) {
      if (m_themes[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::ThemeChip, i};
      }
    }
    return {Target::Panel, -1};
  }
  if (!m_barRect.contains(x, y)) {
    return {};
  }
  for (int i = 0; i < kSegments; ++i) {
    if (segmentEnabled(i) && m_segments[static_cast<std::size_t>(i)].rect.contains(x, y)) {
      return {Target::Segment, i};
    }
  }
  if (m_done.rect.contains(x, y)) {
    return {Target::Done, -1};
  }
  return {Target::Bar, -1};
}

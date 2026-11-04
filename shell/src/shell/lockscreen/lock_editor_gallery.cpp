#include "shell/lockscreen/lock_editor_gallery.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "shell/lockscreen/lock_surface.h"
#include "ui/auth/clock_style.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <string>

namespace {

  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  // Dark thick tint 0.92: the preview behind it is the surface's own content, out of the compositor blur's reach.
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.92F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 0.10F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 0.16F);
  constexpr Color kOn = rgba(1.0F, 1.0F, 1.0F, 0.92F);
  constexpr Color kOnText = rgba(20.0F / 255.0F, 20.0F / 255.0F, 24.0F / 255.0F, 1.0F);
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  // Mock section 2: a free slot has a 1.5 px dashed white 55 % edge; the picked slot a solid accent ring.
  constexpr Color kSlotEdge = rgba(1.0F, 1.0F, 1.0F, 0.55F);
  constexpr Color kClear = rgba(0.0F, 0.0F, 0.0F, 0.0F);

  constexpr float kPanelWidth = 720.0F;
  constexpr float kPad = 16.0F;
  constexpr float kColumns = 8.0F;
  constexpr float kTileHeight = 72.0F;
  constexpr float kGap = 8.0F;
  constexpr float kChipHeight = 28.0F;
  constexpr std::array<const char*, LockEditorGallery::kSizes> kSizeLabels{"", "S", "M", "L", "XL"};

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

  void place(RectNode* node, float x, float y, float w, float h, const RoundedRectStyle& style) {
    node->setPosition(x, y);
    node->setSize(w, h);
    node->setStyle(style);
  }

  // The sample time every tile shows.
  std::string sampleText(const ClockStyle& style) {
    std::tm time{};
    time.tm_hour = 10;
    time.tm_min = 42;
    if (style.layout == ClockLayout::Analog || style.layout == ClockLayout::Flip) {
      return "10:42";
    }
    const std::vector<std::string> lines = clockLines(style, time, {.h24 = true, .seconds = false});
    std::string text;
    for (const std::string& line : lines) {
      text += text.empty() ? line : " " + line;
    }
    return text;
  }

} // namespace

std::vector<LockEditorGallery::Hit> LockEditorGallery::focusOrder(const State& state, int styleCount) {
  std::vector<Hit> order;
  if (!state.slotsMode) {
    return order;
  }
  order.push_back({Target::ClockSlot, -1});
  if (state.open) {
    for (int i = 0; i < styleCount; ++i) {
      order.push_back({Target::Style, i});
    }
    for (int i = 0; i < kSizes; ++i) {
      order.push_back({Target::Size, i});
    }
    order.push_back({Target::Hour24, -1});
    order.push_back({Target::Seconds, -1});
  }
  return order;
}

void LockEditorGallery::build(Node& root) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(9);
  m_slot = addRect(*m_group);
  m_panelBg = addRect(*m_group);
  m_title = addLabel(*m_group, FontWeight::SemiBold);
  // The editor builds a new scene on each open; tiles of the last scene point into freed nodes.
  m_tiles.clear();
  for (std::size_t i = 0; i < clockStyles().size(); ++i) {
    Tile tile;
    tile.bg = addRect(*m_group);
    tile.sample = addLabel(*m_group, FontWeight::Medium);
    tile.name = addLabel(*m_group, FontWeight::Normal);
    m_tiles.push_back(tile);
  }
  for (Chip* chip : {&m_sizes[0], &m_sizes[1], &m_sizes[2], &m_sizes[3], &m_sizes[4], &m_hour24, &m_seconds}) {
    chip->bg = addRect(*m_group);
    chip->label = addLabel(*m_group, FontWeight::Medium);
  }
}

void LockEditorGallery::layoutChip(
    Renderer& renderer, Chip& chip, float x, float y, float s, bool on, bool focused, bool hover
) {
  chip.label->setFontSize(12.0F * s);
  chip.label->setFontWeight(on ? FontWeight::SemiBold : FontWeight::Medium);
  chip.label->setColor(on ? kOnText : kLabel2);
  chip.label->measure(renderer);
  const float h = kChipHeight * s;
  const float w = std::round(chip.label->width() + 24.0F * s);
  chip.rect = {x, y, w, h};
  place(chip.bg, x, y, w, h,
        RoundedRectStyle{
            .fill = on ? kOn : hover ? kFill2 : kFill1,
            .border = focused ? kAccentText : kClear,
            .radius = h * 0.5F,
            .borderWidth = focused ? 2.0F * s : 0.0F,
        });
  chip.label->setPosition(std::round(x + 12.0F * s), std::round(y + (h - chip.label->height()) * 0.5F));
}

void LockEditorGallery::layout(Renderer& renderer, const State& state, Rect slot, float sw, float sh, float scale) {
  m_state = state;
  const float s = std::max(0.1F, scale);
  const bool slotFocus = state.focus == Hit{Target::ClockSlot, -1};
  const bool slotOn = state.open || slotFocus;
  m_slot->setVisible(state.slotsMode);
  m_slotRect = state.slotsMode ? slot : Rect{};
  if (state.slotsMode) {
    place(m_slot, slot.x, slot.y, slot.w, slot.h,
          RoundedRectStyle{
              .fill = state.hover == Hit{Target::ClockSlot, -1} ? kFill1 : kClear,
              .border = slotOn ? kAccentText : kSlotEdge,
              .radius = 16.0F * s,
              .borderWidth = (slotOn ? 2.0F : 1.5F) * s,
              .borderDash = slotOn ? 0.0F : 6.0F * s,
              .borderGap = 4.0F * s,
          });
  }

  const bool open = state.slotsMode && state.open;
  for (Node* node : {static_cast<Node*>(m_panelBg), static_cast<Node*>(m_title)}) {
    node->setVisible(open);
  }
  for (Tile& tile : m_tiles) {
    tile.bg->setVisible(open);
    tile.sample->setVisible(open);
    tile.name->setVisible(open);
  }
  for (Chip* chip : {&m_sizes[0], &m_sizes[1], &m_sizes[2], &m_sizes[3], &m_sizes[4], &m_hour24, &m_seconds}) {
    chip->bg->setVisible(open);
    chip->label->setVisible(open);
  }
  if (!open) {
    m_panelRect = {};
    return;
  }

  const float w = std::min(kPanelWidth * s, sw - 32.0F * s);
  const float pad = kPad * s;
  const auto rows = static_cast<float>((m_tiles.size() + 7) / 8);
  const float tileW = std::floor((w - pad * 2.0F - kGap * s * (kColumns - 1.0F)) / kColumns);
  const float tileH = kTileHeight * s;
  const float h = pad + kChipHeight * s + 12.0F * s + rows * tileH + (rows - 1.0F) * kGap * s + pad;
  const float x = std::round((sw - w) * 0.5F);
  const float y = std::round(sh - 24.0F * s - h);
  m_panelRect = {x, y, w, h};
  place(m_panelBg, x, y, w, h,
        RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 28.0F * s, .borderWidth = 0.5F * s});

  m_title->setText(i18n::tr("lockscreen.editor.clock-style"));
  m_title->setFontSize(Tokens::typeTitle3.size * s);
  m_title->setColor(kLabel1);
  m_title->measure(renderer);
  m_title->setPosition(x + pad, std::round(y + pad + (kChipHeight * s - m_title->height()) * 0.5F));

  // Options right-aligned in the header row: Auto S M L XL · 24-hour · Seconds.
  m_seconds.label->setText(i18n::tr("lockscreen.editor.seconds"));
  m_hour24.label->setText(i18n::tr("lockscreen.editor.hour24"));
  for (int i = 0; i < kSizes; ++i) {
    m_sizes[static_cast<std::size_t>(i)].label->setText(
        i == 0 ? i18n::tr("settings.schema.lockscreen.clock-size.auto") : kSizeLabels[static_cast<std::size_t>(i)]
    );
  }
  const float chipY = y + pad;
  float right = x + w - pad;
  const auto rightChip = [&](Chip& chip, bool on, Hit hit) {
    layoutChip(renderer, chip, 0.0F, chipY, s, on, state.focus == hit, state.hover == hit);
    right -= chip.rect.w;
    layoutChip(renderer, chip, right, chipY, s, on, state.focus == hit, state.hover == hit);
    right -= 6.0F * s;
  };
  rightChip(m_seconds, state.seconds, {Target::Seconds, -1});
  rightChip(m_hour24, state.hour24, {Target::Hour24, -1});
  right -= 10.0F * s;
  for (int i = kSizes - 1; i >= 0; --i) {
    rightChip(m_sizes[static_cast<std::size_t>(i)], i == state.size, {Target::Size, i});
  }

  const float tilesY = chipY + kChipHeight * s + 12.0F * s;
  const auto styles = clockStyles();
  for (std::size_t i = 0; i < m_tiles.size(); ++i) {
    Tile& tile = m_tiles[i];
    const ClockStyle& style = styles[i];
    const auto column = static_cast<float>(i % 8);
    const auto row = static_cast<float>(i / 8);
    const float tx = x + pad + column * (tileW + kGap * s);
    const float ty = tilesY + row * (tileH + kGap * s);
    tile.rect = {tx, ty, tileW, tileH};
    const Hit hit{Target::Style, static_cast<int>(i)};
    const bool on = static_cast<int>(i) == state.style;
    const bool ring = on || state.focus == hit;
    place(tile.bg, tx, ty, tileW, tileH,
          RoundedRectStyle{
              .fill = on || state.hover == hit ? kFill2 : kFill1,
              .border = ring ? kAccentText : kClear,
              .radius = 16.0F * s,
              .borderWidth = ring ? 1.5F * s : 0.0F,
          });
    tile.sample->setText(sampleText(style));
    tile.sample->setFontFamily(lockClockFontFamily(style));
    tile.sample->setFontWeight(static_cast<FontWeight>(std::clamp(style.weight, 100, 900)));
    // One line that fits the tile: long samples (Words) shrink instead of wrapping.
    tile.sample->setMaxLines(1);
    tile.sample->setFontSize(18.0F * s);
    tile.sample->setColor(kLabel1);
    tile.sample->measure(renderer);
    const float room = tileW - 12.0F * s;
    if (tile.sample->width() > room) {
      tile.sample->setFontSize(std::max(10.0F * s, std::floor(18.0F * s * room / tile.sample->width())));
      tile.sample->measure(renderer);
    }
    tile.name->setText(i18n::tr("settings.schema.lockscreen.clock-style." + std::string(style.id)));
    tile.name->setFontSize(Tokens::typeFootnote.size * s);
    tile.name->setColor(kLabel2);
    tile.name->setMaxWidth(tileW - 8.0F * s);
    tile.name->measure(renderer);
    const float stack = tile.sample->height() + 4.0F * s + tile.name->height();
    const float top = ty + (tileH - stack) * 0.5F;
    tile.sample->setPosition(std::round(tx + (tileW - tile.sample->width()) * 0.5F), std::round(top));
    tile.name->setPosition(
        std::round(tx + (tileW - tile.name->width()) * 0.5F), std::round(top + tile.sample->height() + 4.0F * s)
    );
  }
}

LockEditorGallery::Hit LockEditorGallery::hitTest(float x, float y) const {
  if (m_state.slotsMode && m_state.open && m_panelRect.contains(x, y)) {
    for (std::size_t i = 0; i < m_tiles.size(); ++i) {
      if (m_tiles[i].rect.contains(x, y)) {
        return {Target::Style, static_cast<int>(i)};
      }
    }
    for (int i = 0; i < kSizes; ++i) {
      if (m_sizes[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::Size, i};
      }
    }
    if (m_hour24.rect.contains(x, y)) {
      return {Target::Hour24, -1};
    }
    if (m_seconds.rect.contains(x, y)) {
      return {Target::Seconds, -1};
    }
    return {Target::Panel, -1};
  }
  if (m_slotRect.contains(x, y)) {
    return {Target::ClockSlot, -1};
  }
  return {};
}

#include "shell/lockscreen/lock_editor_gallery.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/auth/auth_clock.h"
#include "ui/auth/clock_style.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace {
  constexpr float kSampleFontSize = 24.0F;
  constexpr float kSampleMinFontSize = 12.0F;

  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kLabel3 = rgba(1.0F, 1.0F, 1.0F, 0.46F);
  // Dark thick tint 0.92: the preview behind it is the surface's own content, out of the compositor blur's reach.
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.62F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 0.10F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 0.16F);
  constexpr Color kOn = rgba(1.0F, 1.0F, 1.0F, 0.92F);
  constexpr Color kOnText = rgba(20.0F / 255.0F, 20.0F / 255.0F, 24.0F / 255.0F, 1.0F);
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  constexpr std::uint32_t kAccentTextInk = 0x8C85FFFFU;
  // Mock section 2: a free slot has a 1.5 px dashed white 55 % edge; the picked slot a solid accent ring.
  constexpr Color kSlotEdge = rgba(1.0F, 1.0F, 1.0F, 0.55F);
  constexpr Color kClear = rgba(0.0F, 0.0F, 0.0F, 0.0F);

  constexpr float kPanelWidth = 1120.0F;
  float optionsWidth() noexcept { return Tokens::spaceP64 * 5.0F; }
  constexpr float kPad = 16.0F;
  constexpr int kColumns = 6;
  constexpr float kTileHeight = 68.0F;
  constexpr const float& kGap = Tokens::spaceP8;
  constexpr float kChipHeight = 28.0F;
  constexpr float kRow = 32.0F;
  constexpr const float& kRowGap = Tokens::spaceP4;
  float labelWidth() noexcept { return Tokens::spaceP24 * 3.0F; }
  constexpr const float& kValueWidth = Tokens::spaceP40;
  constexpr float kTrack = 4.0F;
  constexpr float kKnob = 16.0F;
  constexpr float kSwatch = 24.0F;
  constexpr std::array<const char*, LockEditorGallery::kSizes> kSizeLabels{"", "S", "M", "L", "XL", ""};
  constexpr std::array<const char*, LockEditorGallery::kFilters> kFilterKeys{
      "filter-all", "filter-digital", "filter-serif", "filter-stacked", "filter-analog", "filter-animated",
      "filter-matrix"
  };
  constexpr std::array<const char*, LockEditorGallery::kAxes> kAxisKeys{"axis-weight", "axis-width", "axis-round",
                                                                         "axis-slant"};

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
    if (style.layout == ClockLayout::Analog || style.layout == ClockLayout::Flip || style.layout == ClockLayout::Orbit) {
      return "10:42";
    }
    if (style.layout == ClockLayout::WordGrid) {
      return "TWENTY TO ELEVEN";
    }
    if (style.layout == ClockLayout::Poster) {
      return "Sat 10:42";
    }
    const std::vector<std::string> lines = clockLines(style, time, {.h24 = true, .seconds = false});
    std::string text;
    for (const std::string& line : lines) {
      text += text.empty() ? line : " " + line;
    }
    return text;
  }

  // A tag's value in the style's own axis string ("wdth=75,ROND=100"); fallback when absent.
  float styleAxis(const ClockStyle& style, std::string_view tag, float fallback) {
    const std::string_view axes = style.variations;
    const std::string key = std::string(tag) + "=";
    const std::size_t at = axes.find(key);
    if (at == std::string_view::npos) {
      return fallback;
    }
    float value = fallback;
    const char* begin = axes.data() + at + key.size();
    std::from_chars(begin, axes.data() + axes.size(), value);
    return value;
  }

  bool animated(const ClockStyle& style) {
    return style.live || style.breatheWidth || style.morph || style.caret || style.layout == ClockLayout::Flip;
  }

  bool isSerif(const ClockStyle& style) {
    return style.file.starts_with("Fraunces") || style.file.starts_with("InstrumentSerif");
  }

  std::string axisText(int axis, float value) {
    const int v = static_cast<int>(std::lround(value));
    switch (axis) {
    case 1:
      return std::format("{} %", v);
    case 3:
      return std::format("{}°", v);
    default:
      return std::to_string(v);
    }
  }

} // namespace

LockEditorGallery::AxisRange LockEditorGallery::axisRange(const ClockStyle& style, int axis) {
  const ClockFontAxes axes = clockFontAxes(style);
  switch (axis) {
  case 0:
    // A live style's weight follows the seconds.
    return {axes.weight() && !style.live, axes.weightMin, axes.weightMax, static_cast<float>(style.weight)};
  case 1:
    return {axes.width(), axes.widthMin, axes.widthMax, styleAxis(style, "wdth", axes.widthMax)};
  case 2:
    return {axes.round(), 0.0F, axes.roundMax, styleAxis(style, axes.roundTag, 0.0F)};
  case 3:
    return {axes.slant(), 0.0F, -axes.slantMin, -styleAxis(style, "slnt", 0.0F)};
  default:
    return {};
  }
}

float LockEditorGallery::axisValue(const ClockStyle& style, int axis, int raw) {
  const AxisRange range = axisRange(style, axis);
  const bool set = axis == 2 ? raw >= 0 : raw > 0;
  return std::clamp(set ? static_cast<float>(raw) : range.base, range.min, std::max(range.min, range.max));
}

int LockEditorGallery::axisRaw(const ClockStyle& style, int axis, float value) {
  const AxisRange range = axisRange(style, axis);
  const int v = static_cast<int>(std::lround(value));
  if (v == static_cast<int>(std::lround(range.base))) {
    return axis == 2 ? -1 : 0;
  }
  // Weight 0 and width 0 mean "the style's own"; the smallest real value stays a real override.
  return axis == 2 || axis == 3 ? v : std::max(1, v);
}

bool LockEditorGallery::inFilter(const ClockStyle& style, int filter) {
  switch (filter) {
  case 1:
    return style.layout == ClockLayout::Line && !animated(style) && !isSerif(style) && !style.file.starts_with("Doto");
  case 2:
    return isSerif(style);
  case 3:
    return style.layout == ClockLayout::Stacked || style.layout == ClockLayout::Words
        || style.layout == ClockLayout::Poster;
  case 4:
    return style.layout == ClockLayout::Analog || style.layout == ClockLayout::Orbit;
  case 5:
    return animated(style);
  case 6:
    return style.file.starts_with("Doto") || style.layout == ClockLayout::WordGrid;
  default:
    return true;
  }
}

std::vector<LockEditorGallery::Hit> LockEditorGallery::focusOrder(const State& state, int styleCount) {
  std::vector<Hit> order;
  if (!state.slotsMode) {
    return order;
  }
  order.push_back({Target::ClockSlot, -1});
  if (!state.open) {
    return order;
  }
  for (int i = 0; i < kFilters; ++i) {
    order.push_back({Target::Filter, i});
  }
  const auto styles = clockStyles();
  for (int i = 0; i < styleCount && i < static_cast<int>(styles.size()); ++i) {
    if (inFilter(styles[static_cast<std::size_t>(i)], state.filter)) {
      order.push_back({Target::Style, i});
    }
  }
  const ClockStyle& style = styles[static_cast<std::size_t>(std::clamp(state.style, 0, styleCount - 1))];
  for (int i = 0; i < kAxes; ++i) {
    if (axisRange(style, i).available) {
      order.push_back({Target::Axis, i});
    }
  }
  for (int i = 0; i < kMaterials; ++i) {
    order.push_back({Target::Material, i});
  }
  for (int i = 0; i < kColors; ++i) {
    order.push_back({Target::Color, i});
  }
  for (int i = 0; i < kSizes; ++i) {
    order.push_back({Target::Size, i});
  }
  order.push_back({Target::Hour24, -1});
  order.push_back({Target::Seconds, -1});
  order.push_back({Target::Motion, -1});
  return order;
}

void LockEditorGallery::build(Node& root, AnimationManager& animations) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(9);
  m_slot = addRect(*m_group);
  m_panelBg = addRect(*m_group);
  m_title = addLabel(*m_group, FontWeight::SemiBold);
  for (Chip& chip : m_filters) {
    chip.bg = addRect(*m_group);
    chip.label = addLabel(*m_group, FontWeight::Medium);
  }
  // The editor builds a new scene on each open; tiles of the last scene point into freed nodes.
  m_tiles.clear();
  for (std::size_t i = 0; i < clockStyles().size(); ++i) {
    Tile tile;
    tile.bg = addRect(*m_group);
    tile.sample = addLabel(*m_group, FontWeight::Medium);
    tile.name = addLabel(*m_group, FontWeight::Normal);
    m_tiles.push_back(tile);
  }
  m_tileDial.build(*m_group);
  m_tileOrbit.build(*m_group);
  m_tileFlip.build(*m_group, animations);
  m_tileMinutes = addLabel(*m_group, FontWeight::Medium);
  for (Bar& bar : m_bars) {
    bar.label = addLabel(*m_group, FontWeight::Normal);
    bar.track = addRect(*m_group);
    bar.fill = addRect(*m_group);
    bar.knob = addRect(*m_group);
    bar.value = addLabel(*m_group, FontWeight::Medium);
  }
  m_materialLabel = addLabel(*m_group, FontWeight::Normal);
  for (Chip& chip : m_materials) {
    chip.bg = addRect(*m_group);
    chip.label = addLabel(*m_group, FontWeight::Medium);
  }
  m_colorLabel = addLabel(*m_group, FontWeight::Normal);
  for (Swatch& swatch : m_swatches) {
    swatch.ring = addRect(*m_group);
    swatch.dot = addRect(*m_group);
  }
  m_sizeLabel = addLabel(*m_group, FontWeight::Normal);
  for (Chip& chip : m_sizes) {
    chip.bg = addRect(*m_group);
    chip.label = addLabel(*m_group, FontWeight::Medium);
  }
  for (Chip* chip : {&m_hour24, &m_seconds, &m_motion}) {
    chip->bg = addRect(*m_group);
    chip->label = addLabel(*m_group, FontWeight::Medium);
  }
}

void LockEditorGallery::layoutChip(
    Renderer& renderer, Chip& chip, float x, float y, float s, bool on, bool focused, bool hover
) {
  chip.label->setFontSize(Tokens::typeFootnote.size * s);
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

void LockEditorGallery::layoutBar(Renderer& renderer, Bar& bar, int axis, float x, float y, float w, float s) {
  const ClockStyle& style = clockStyles()[static_cast<std::size_t>(m_state.style)];
  const AxisRange range = axisRange(style, axis);
  const float value = axisValue(style, axis, m_state.axes[static_cast<std::size_t>(axis)]);
  const float fraction = range.max > range.min ? (value - range.min) / (range.max - range.min) : 0.0F;
  const Hit hit{Target::Axis, axis};
  const bool focused = m_state.focus == hit;
  const float rowH = kRow * s;
  bar.rect = {x, y, w, rowH};
  bar.label->setText(i18n::tr(std::string("lockscreen.editor.") + kAxisKeys[static_cast<std::size_t>(axis)]));
  bar.label->setFontSize(Tokens::typeFootnote.size * s);
  bar.label->setColor(kLabel2);
  bar.label->measure(renderer);
  bar.label->setPosition(x, std::round(y + (rowH - bar.label->height()) * 0.5F));
  bar.value->setText(axisText(axis, value));
  bar.value->setFontSize(Tokens::typeFootnote.size * s);
  bar.value->setColor(kLabel1);
  bar.value->measure(renderer);
  bar.value->setPosition(std::round(x + w - bar.value->width()), std::round(y + (rowH - bar.value->height()) * 0.5F));
  const float trackX = x + labelWidth() * s;
  const float trackW = std::max(1.0F, w - (labelWidth() + kValueWidth + 8.0F) * s);
  const float trackH = kTrack * s;
  const float trackY = std::round(y + (rowH - trackH) * 0.5F);
  bar.trackRect = {trackX, y, trackW, rowH};
  place(bar.track, trackX, trackY, trackW, trackH, RoundedRectStyle{.fill = kFill2, .radius = trackH * 0.5F});
  const float cx = trackX + trackW * std::clamp(fraction, 0.0F, 1.0F);
  place(bar.fill, trackX, trackY, std::max(trackH, cx - trackX), trackH,
        RoundedRectStyle{.fill = kOn, .radius = trackH * 0.5F});
  const float knob = kKnob * s;
  place(bar.knob, std::round(cx - knob * 0.5F), std::round(y + (rowH - knob) * 0.5F), knob, knob,
        RoundedRectStyle{
            .fill = kOn,
            .border = focused ? kAccentText : m_state.hover == hit ? kLabel2 : kClear,
            .radius = knob * 0.5F,
            .borderWidth = focused || m_state.hover == hit ? 2.0F * s : 0.0F,
        });
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
              .radius = Tokens::radiusR16 * s,
              .borderWidth = (slotOn ? 2.0F : 1.5F) * s,
              .borderDash = slotOn ? 0.0F : 6.0F * s,
              .borderGap = 4.0F * s,
          });
  }

  const bool open = state.slotsMode && state.open;
  const auto styles = clockStyles();
  const ClockStyle& current = styles[static_cast<std::size_t>(std::clamp<int>(state.style, 0, static_cast<int>(styles.size()) - 1))];
  const auto show = [](std::initializer_list<Node*> nodes, bool visible) {
    for (Node* node : nodes) {
      node->setVisible(visible);
    }
  };
  show({m_panelBg, m_title, m_materialLabel, m_colorLabel, m_sizeLabel, m_hour24.bg, m_hour24.label, m_seconds.bg,
        m_seconds.label, m_motion.bg, m_motion.label},
       open);
  for (Chip& chip : m_materials) {
    show({chip.bg, chip.label}, open);
  }
  for (Chip& chip : m_filters) {
    show({chip.bg, chip.label}, open);
  }
  m_tileDial.setVisible(false);
  m_tileOrbit.setVisible(false);
  m_tileFlip.setVisible(false);
  m_tileMinutes->setVisible(false);
  for (std::size_t i = 0; i < m_tiles.size(); ++i) {
    show({m_tiles[i].bg, m_tiles[i].sample, m_tiles[i].name}, open && inFilter(styles[i], state.filter));
    if (!open || !inFilter(styles[i], state.filter)) {
      m_tiles[i].rect = {};
    }
  }
  for (int i = 0; i < kAxes; ++i) {
    Bar& bar = m_bars[static_cast<std::size_t>(i)];
    const bool on = open && axisRange(current, i).available;
    show({bar.label, bar.track, bar.fill, bar.knob, bar.value}, on);
    if (!on) {
      bar.rect = {};
      bar.trackRect = {};
    }
  }
  for (Swatch& swatch : m_swatches) {
    show({swatch.ring, swatch.dot}, open);
  }
  for (Chip& chip : m_sizes) {
    show({chip.bg, chip.label}, open);
  }
  if (!open) {
    m_panelRect = {};
    return;
  }

  const float w = std::min(kPanelWidth * s, sw - 32.0F * s);
  const float pad = kPad * s;
  const float optionsW = std::min(optionsWidth() * s, w * 0.4F);
  const float leftW = w - pad * 3.0F - optionsW;
  int shown = 0;
  for (const ClockStyle& style : styles) {
    shown += inFilter(style, state.filter) ? 1 : 0;
  }
  const auto rows = static_cast<float>((shown + kColumns - 1) / kColumns);
  const float tileW = std::floor((leftW - kGap * s * static_cast<float>(kColumns - 1)) / static_cast<float>(kColumns));
  const float tileH = kTileHeight * s;
  int axisRows = 0;
  for (int i = 0; i < kAxes; ++i) {
    axisRows += axisRange(current, i).available ? 1 : 0;
  }
  const float tilesH = std::max(1.0F, rows) * tileH + std::max(0.0F, rows - 1.0F) * kGap * s;
  // Options: axis bars, material, colour, size label + chips, format chips.
  const float optionsH = static_cast<float>(axisRows + 3) * (kRow + kRowGap) * s + (kChipHeight + 8.0F) * s * 2.0F;
  // The tallest filter (All) sets the height, so the sheet keeps its size while filtering.
  const float allRows = static_cast<float>((static_cast<int>(styles.size()) + kColumns - 1) / kColumns);
  const float bodyH = std::max({tilesH, optionsH, allRows * tileH + (allRows - 1.0F) * kGap * s});
  const float h = pad + kChipHeight * s + 12.0F * s + bodyH + pad;
  const float x = std::round((sw - w) * 0.5F);
  const float y = std::round(sh - 24.0F * s - h);
  m_panelRect = {x, y, w, h};
  place(m_panelBg, x, y, w, h,
        RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = Tokens::radiusR28 * s, .borderWidth = 0.5F * s});

  // Header: title, then the filter chips.
  const float chipY = y + pad;
  m_title->setText(i18n::tr("lockscreen.editor.clock-style"));
  m_title->setFontSize(Tokens::typeTitle3.size * s);
  m_title->setColor(kLabel1);
  m_title->measure(renderer);
  m_title->setPosition(x + pad, std::round(chipY + (kChipHeight * s - m_title->height()) * 0.5F));
  float fx = x + pad + m_title->width() + 16.0F * s;
  for (int i = 0; i < kFilters; ++i) {
    Chip& chip = m_filters[static_cast<std::size_t>(i)];
    const Hit hit{Target::Filter, i};
    chip.label->setText(i18n::tr(std::string("lockscreen.editor.") + kFilterKeys[static_cast<std::size_t>(i)]));
    layoutChip(renderer, chip, fx, chipY, s, i == state.filter, state.focus == hit, state.hover == hit);
    fx += chip.rect.w + 6.0F * s;
  }

  // Tiles: every shown style drawn in its own font, weight, axes, tracking, outline and gradient.
  const float tilesY = chipY + kChipHeight * s + 12.0F * s;
  int slotIndex = 0;
  for (std::size_t i = 0; i < m_tiles.size(); ++i) {
    const ClockStyle& style = styles[i];
    if (!inFilter(style, state.filter)) {
      continue;
    }
    Tile& tile = m_tiles[i];
    const auto column = static_cast<float>(slotIndex % kColumns);
    const auto row = static_cast<float>(slotIndex / kColumns);
    ++slotIndex;
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
              .radius = Tokens::radiusR16 * s,
              .borderWidth = ring ? 1.5F * s : 0.0F,
          });
    tile.sample->setText(sampleText(style));
    tile.sample->setFontFamily(clockFontFamily(style));
    tile.sample->setFontWeight(static_cast<FontWeight>(std::clamp(clockWeightAt(style, 30), 100, 900)));
    tile.sample->setVariations(std::string(style.variations));
    tile.sample->setTracking(style.tracking);
    tile.sample->setStroke(style.stroke);
    tile.sample->setGradient(
        style.gradient ? state.wallpaperInk : 0U, style.gradient ? state.gradientTo : 0U
    );
    // One line that fits the tile: long samples (Words) shrink instead of wrapping.
    tile.sample->setMaxLines(1);
    tile.sample->setFontSize(kSampleFontSize * s);
    tile.sample->setColor(kLabel1);
    tile.sample->measure(renderer);
    const float room = tileW - 16.0F * s;
    if (tile.sample->width() > room) {
      tile.sample->setFontSize(std::max(kSampleMinFontSize * s, std::floor(kSampleFontSize * s * room / tile.sample->width())));
      tile.sample->measure(renderer);
    }
    tile.name->setText(i18n::tr("settings.schema.lockscreen.clock-style." + std::string(style.id)));
    tile.name->setFontSize(Tokens::typeFootnote.size * s);
    tile.name->setColor(on ? kLabel1 : kLabel3);
    tile.name->setMaxWidth(tileW - 8.0F * s);
    tile.name->measure(renderer);
    // Analog and Flip draw their own clock in the sample's place; Two-tone splits off the accent minutes.
    const float art = 36.0F * s;
    const bool dial = style.layout == ClockLayout::Analog || style.layout == ClockLayout::Orbit;
    const bool flip = style.layout == ClockLayout::Flip;
    tile.sample->setVisible(!dial && !flip);
    const float sampleH = dial || flip ? art : tile.sample->height();
    const float stack = sampleH + 4.0F * s + tile.name->height();
    const float top = ty + (tileH - stack) * 0.5F;
    if (dial) {
      if (style.layout == ClockLayout::Orbit) {
        m_tileOrbit.setVisible(true);
        m_tileOrbit.layoutOrbit(std::round(tx + (tileW - art) * 0.5F), std::round(top), art, 10, 42, kLabel1, kAccentText);
      } else {
        m_tileDial.setVisible(true);
        m_tileDial.layout(std::round(tx + (tileW - art) * 0.5F), std::round(top), art, 10, 42, kLabel1);
      }
    } else if (flip) {
      const float clockSize = art * 64.0F / 72.0F;
      m_tileFlip.setVisible(true);
      m_tileFlip.layout(renderer, std::round(tx + (tileW - FlipClock::widthFor(clockSize)) * 0.5F), std::round(top),
                        clockSize, "10", "42", kLabel1);
    } else if (style.twoTone) {
      tile.sample->setText("10:");
      tile.sample->setColor(kLabel1);
      tile.sample->measure(renderer);
      m_tileMinutes->setVisible(true);
      m_tileMinutes->setText("42");
      m_tileMinutes->setFontFamily(clockFontFamily(style));
      m_tileMinutes->setFontWeight(static_cast<FontWeight>(std::clamp(style.weight, 100, 900)));
      m_tileMinutes->setTracking(style.tracking);
      m_tileMinutes->setFontSize(tile.sample->fontSize());
      m_tileMinutes->setColor(kAccentText);
      m_tileMinutes->measure(renderer);
      const float left = std::round(tx + (tileW - tile.sample->width() - m_tileMinutes->width()) * 0.5F);
      tile.sample->setPosition(left, std::round(top));
      m_tileMinutes->setPosition(left + tile.sample->width(), std::round(top));
    }
    if (!style.twoTone) {
      tile.sample->setPosition(std::round(tx + (tileW - tile.sample->width()) * 0.5F), std::round(top));
    }
    tile.name->setPosition(
        std::round(tx + (tileW - tile.name->width()) * 0.5F), std::round(top + sampleH + 4.0F * s)
    );
  }

  // Options column.
  const float ox = x + w - pad - optionsW;
  float oy = tilesY;
  for (int i = 0; i < kAxes; ++i) {
    if (axisRange(current, i).available) {
      layoutBar(renderer, m_bars[static_cast<std::size_t>(i)], i, ox, oy, optionsW, s);
      oy += (kRow + kRowGap) * s;
    }
  }
  const float rowH = kRow * s;
  m_materialLabel->setText(i18n::tr("lockscreen.editor.material"));
  m_materialLabel->setFontSize(Tokens::typeFootnote.size * s);
  m_materialLabel->setColor(kLabel2);
  m_materialLabel->measure(renderer);
  m_materialLabel->setPosition(ox, std::round(oy + (rowH - m_materialLabel->height()) * 0.5F));
  float mx = ox + labelWidth() * s;
  for (int i = 0; i < kMaterials; ++i) {
    Chip& chip = m_materials[static_cast<std::size_t>(i)];
    const Hit hit{Target::Material, i};
    chip.label->setText(i18n::tr(std::string("lockscreen.editor.material-") + kMaterialNames[static_cast<std::size_t>(i)]));
    layoutChip(renderer, chip, mx, std::round(oy + (rowH - kChipHeight * s) * 0.5F), s, i == state.material,
               state.focus == hit, state.hover == hit);
    mx += chip.rect.w + 6.0F * s;
  }
  oy += (kRow + kRowGap) * s;
  m_colorLabel->setText(i18n::tr("lockscreen.editor.colour"));
  m_colorLabel->setFontSize(Tokens::typeFootnote.size * s);
  m_colorLabel->setColor(kLabel2);
  m_colorLabel->measure(renderer);
  m_colorLabel->setPosition(ox, std::round(oy + (rowH - m_colorLabel->height()) * 0.5F));
  float sx = ox + labelWidth() * s;
  for (int i = 0; i < kColors; ++i) {
    Swatch& swatch = m_swatches[static_cast<std::size_t>(i)];
    const Hit hit{Target::Color, i};
    const bool on = i == state.color;
    const float ring = rowH;
    swatch.rect = {sx, oy, ring, ring};
    place(swatch.ring, sx, oy, ring, ring,
          RoundedRectStyle{
              .fill = state.hover == hit ? kFill1 : kClear,
              .border = state.focus == hit ? kAccentText : on ? kOn : kClear,
              .radius = ring * 0.5F,
              .borderWidth = on || state.focus == hit ? 2.0F * s : 0.0F,
          });
    const float dot = kSwatch * s;
    const float dx = std::round(sx + (ring - dot) * 0.5F);
    const float dy = std::round(oy + (ring - dot) * 0.5F);
    const Color wallpaper = state.wallpaperInk != 0 ? rgbaHex(state.wallpaperInk) : kLabel2;
    RoundedRectStyle dotStyle{.fill = i == 0 ? kLabel1 : i == 1 ? kAccentText : wallpaper, .radius = dot * 0.5F};
    if (i == 3) {
      const Color to = state.gradientTo != 0 ? rgbaHex(state.gradientTo) : rgbaHex(kAccentTextInk);
      dotStyle.fillMode = FillMode::LinearGradient;
      dotStyle.gradientDirection = GradientDirection::Horizontal;
      dotStyle.gradientStops = {GradientStop{0.0F, wallpaper}, GradientStop{1.0F, to}, GradientStop{1.0F, to},
                                GradientStop{1.0F, to}};
    }
    place(swatch.dot, dx, dy, dot, dot, dotStyle);
    sx += ring + 8.0F * s;
  }
  oy += (kRow + kRowGap) * s;

  m_sizeLabel->setText(i18n::tr("settings.schema.lockscreen.clock-size.label"));
  m_sizeLabel->setFontSize(Tokens::typeFootnote.size * s);
  m_sizeLabel->setColor(kLabel2);
  m_sizeLabel->measure(renderer);
  m_sizeLabel->setPosition(ox, std::round(oy + (rowH - m_sizeLabel->height()) * 0.5F));
  oy += rowH;
  float cx = ox;
  for (int i = 0; i < kSizes; ++i) {
    Chip& chip = m_sizes[static_cast<std::size_t>(i)];
    const Hit hit{Target::Size, i};
    chip.label->setText(
        i == 0                ? i18n::tr("settings.schema.lockscreen.clock-size.auto")
            : i == kSizes - 1 ? i18n::tr("lockscreen.editor.size-fill")
                              : std::string(kSizeLabels[static_cast<std::size_t>(i)])
    );
    layoutChip(renderer, chip, cx, oy, s, i == state.size, state.focus == hit, state.hover == hit);
    cx += chip.rect.w + 6.0F * s;
  }
  oy += (kChipHeight + 8.0F) * s;
  m_hour24.label->setText(i18n::tr("lockscreen.editor.hour24"));
  m_seconds.label->setText(i18n::tr("lockscreen.editor.seconds"));
  m_motion.label->setText(i18n::tr("lockscreen.editor.motion"));
  layoutChip(renderer, m_hour24, ox, oy, s, state.hour24, state.focus == Hit{Target::Hour24, -1},
             state.hover == Hit{Target::Hour24, -1});
  layoutChip(renderer, m_seconds, ox + m_hour24.rect.w + 6.0F * s, oy, s, state.seconds,
             state.focus == Hit{Target::Seconds, -1}, state.hover == Hit{Target::Seconds, -1});
  layoutChip(renderer, m_motion, m_seconds.rect.x + m_seconds.rect.w + 6.0F * s, oy, s, state.motion,
             state.focus == Hit{Target::Motion, -1}, state.hover == Hit{Target::Motion, -1});
}

float LockEditorGallery::axisValueAt(const State& state, int axis, float x) const {
  const auto styles = clockStyles();
  const ClockStyle& style = styles[static_cast<std::size_t>(std::clamp<int>(state.style, 0, static_cast<int>(styles.size()) - 1))];
  const AxisRange range = axisRange(style, axis);
  const Rect& track = m_bars[static_cast<std::size_t>(std::clamp(axis, 0, kAxes - 1))].trackRect;
  const float fraction = track.w > 0.0F ? std::clamp((x - track.x) / track.w, 0.0F, 1.0F) : 0.0F;
  return range.min + fraction * (range.max - range.min);
}

LockEditorGallery::Hit LockEditorGallery::hitTest(float x, float y) const {
  if (m_state.slotsMode && m_state.open && m_panelRect.contains(x, y)) {
    for (int i = 0; i < kFilters; ++i) {
      if (m_filters[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::Filter, i};
      }
    }
    for (std::size_t i = 0; i < m_tiles.size(); ++i) {
      if (m_tiles[i].rect.contains(x, y)) {
        return {Target::Style, static_cast<int>(i)};
      }
    }
    for (int i = 0; i < kAxes; ++i) {
      if (m_bars[static_cast<std::size_t>(i)].trackRect.contains(x, y)) {
        return {Target::Axis, i};
      }
    }
    for (int i = 0; i < kMaterials; ++i) {
      if (m_materials[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::Material, i};
      }
    }
    for (int i = 0; i < kColors; ++i) {
      if (m_swatches[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::Color, i};
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
    if (m_motion.rect.contains(x, y)) {
      return {Target::Motion, -1};
    }
    return {Target::Panel, -1};
  }
  if (m_slotRect.contains(x, y)) {
    return {Target::ClockSlot, -1};
  }
  return {};
}

#include "shell/lockscreen/lock_editor_wallpaper.h"

#include "core/files/directory_scanner.h"
#include "i18n/i18n.h"
#include "shell/lockscreen/lock_screen.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>

namespace {

  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  // Dark thick tint 0.92, as the clock gallery: the preview behind it is the surface's own content.
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.92F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 0.10F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 0.16F);
  constexpr Color kOn = rgba(1.0F, 1.0F, 1.0F, 0.92F);
  constexpr Color kOnText = rgba(20.0F / 255.0F, 20.0F / 255.0F, 24.0F / 255.0F, 1.0F);
  constexpr Color kAccent = rgba(81.0F / 255.0F, 71.0F / 255.0F, 229.0F / 255.0F, 1.0F);
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  constexpr Color kBadge = rgba(0.0F, 0.0F, 0.0F, 0.40F);
  constexpr Color kClear = rgba(0.0F, 0.0F, 0.0F, 0.0F);

  // Mock section 3: panel 880, tiles 80 high r 12 in 6 columns, gap 8, tab and chip height 24.
  constexpr float kPanelWidth = 880.0F;
  constexpr float kPad = 16.0F;
  constexpr float kGap = 8.0F;
  constexpr float kTileHeight = 80.0F;
  constexpr float kTileRadius = 12.0F;
  constexpr float kTabHeight = 24.0F;
  constexpr float kSliderRow = 28.0F;
  constexpr float kTrackHeight = 6.0F;
  constexpr int kThumbPx = 256;
  constexpr std::array<const char*, LockEditorWallpaper::kTabs> kTabKeys{
      "lockscreen.editor.wallpaper-photos", "lockscreen.editor.wallpaper-live", "lockscreen.editor.wallpaper-colours"
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

  void place(RectNode* node, float x, float y, float w, float h, const RoundedRectStyle& style) {
    node->setPosition(x, y);
    node->setSize(w, h);
    node->setStyle(style);
  }

  // A capsule chip of height h: white when on, an accent ring when focused; an unfilled chip shows a fill on hover.
  void layoutChip(Renderer& renderer, RectNode* bg, Label* label, float x, float y, float h, float s, bool on,
                  bool focused, bool hover, bool filled, float& width) {
    label->setFontSize(12.0F * s);
    label->setFontWeight(on ? FontWeight::SemiBold : FontWeight::Medium);
    label->setColor(on ? kOnText : kLabel2);
    label->measure(renderer);
    width = std::round(label->width() + 20.0F * s);
    place(bg, x, y, width, h,
          RoundedRectStyle{
              .fill = on ? kOn : hover ? kFill2 : filled ? kFill1 : kClear,
              .border = focused ? kAccentText : kClear,
              .radius = h * 0.5F,
              .borderWidth = focused ? 2.0F * s : 0.0F,
          });
    label->setPosition(std::round(x + 10.0F * s), std::round(y + (h - label->height()) * 0.5F));
  }

} // namespace

std::vector<LockEditorWallpaper::Item> LockEditorWallpaper::items(int tab, const std::string& directory) {
  std::vector<Item> list;
  if (tab == 2) {
    // Rulebook night blue, indigo, teal, graphite, slate, rose, sand, black.
    constexpr std::array<const char*, 8> kColours{"#10182b", "#5147e5", "#1f6f78", "#2b2b30",
                                                  "#4a5568", "#c06c84", "#d8c3a5", "#000000"};
    for (const char* hex : kColours) {
      Color colour;
      (void)tryParseHexColor(hex, colour);
      list.push_back({.kind = Item::Kind::Colour, .value = std::string("color:") + hex, .name = hex, .colour = colour});
    }
    return list;
  }
  if (tab == 0) {
    list.push_back({.kind = Item::Kind::Desktop, .value = "", .name = i18n::tr("lockscreen.editor.wallpaper-desktop")});
  }
  std::error_code error;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (!entry.is_regular_file(error)) {
      continue;
    }
    const std::string path = entry.path().string();
    const bool live = !LockScreen::liveWallpaperType(path).empty();
    if (tab == 0 ? DirectoryScanner::isImagePath(entry.path()) : live) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  for (const auto& file : files) {
    const bool video = LockScreen::liveWallpaperType(file.string()) == "video";
    list.push_back({
        .kind = tab == 0 ? Item::Kind::Image : video ? Item::Kind::Video : Item::Kind::Shader,
        .value = file.string(),
        .name = file.stem().string(),
    });
  }
  return list;
}

int LockEditorWallpaper::rowFor(int index, int scrollRow) {
  const int row = index / kColumns;
  return std::clamp(scrollRow, row - kRows + 1, row);
}

std::vector<LockEditorWallpaper::Hit> LockEditorWallpaper::focusOrder(const State& state, int itemCount) {
  std::vector<Hit> order;
  if (!state.open) {
    return order;
  }
  for (int i = 0; i < kTabs; ++i) {
    order.push_back({Target::Tab, i});
  }
  order.push_back({Target::Scope, -1});
  for (int i = 0; i < itemCount; ++i) {
    order.push_back({Target::Tile, i});
  }
  order.push_back({Target::Blur, -1});
  order.push_back({Target::Dim, -1});
  return order;
}

void LockEditorWallpaper::build(Node& root) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(9);
  m_panelBg = addRect(*m_group);
  for (Chip& tab : m_tabs) {
    tab.bg = addRect(*m_group);
    tab.label = addLabel(*m_group, FontWeight::Medium);
  }
  m_scopeKey = addLabel(*m_group, FontWeight::Normal);
  m_scope.bg = addRect(*m_group);
  m_scope.label = addLabel(*m_group, FontWeight::Medium);
  for (Tile& tile : m_tiles) {
    tile.bg = addRect(*m_group);
    auto thumb = std::make_unique<Image>();
    thumb->setHitTestVisible(false);
    thumb->setFit(ImageFit::Cover);
    thumb->setAsyncReadyCallback([this] {
      if (m_onThumbnail) {
        m_onThumbnail();
      }
    });
    tile.thumb = static_cast<Image*>(m_group->addChild(std::move(thumb)));
    tile.name = addLabel(*m_group, FontWeight::Medium);
    tile.name->setEllipsize(TextEllipsize::Middle);
    tile.name->setMaxLines(1);
    tile.badge = addRect(*m_group);
    tile.badgeText = addLabel(*m_group, FontWeight::Medium);
    tile.ring = addRect(*m_group);
    tile.source.clear();
  }
  m_empty = addLabel(*m_group, FontWeight::Normal);
  for (Slider* slider : {&m_blur, &m_dim}) {
    slider->label = addLabel(*m_group, FontWeight::Medium);
    slider->track = addRect(*m_group);
    slider->fill = addRect(*m_group);
    slider->value = addLabel(*m_group, FontWeight::Normal);
  }
  m_foot = addLabel(*m_group, FontWeight::Normal);
}

void LockEditorWallpaper::layoutTile(
    Renderer& renderer, Tile& tile, const Item* item, float x, float y, float w, float h, float s, bool picked,
    bool focused, bool hover
) {
  const bool visible = item != nullptr;
  for (Node* node : std::initializer_list<Node*>{tile.bg, tile.thumb, tile.name, tile.badge, tile.badgeText, tile.ring}) {
    node->setVisible(visible);
  }
  tile.rect = visible ? Rect{x, y, w, h} : Rect{};
  if (!visible) {
    return;
  }
  const float radius = kTileRadius * s;
  const bool live = item->kind == Item::Kind::Video || item->kind == Item::Kind::Shader;
  // A live item shows its still once the editor found one (Item::still); until then the tile shows the name.
  const std::string& still = live ? item->still : item->kind == Item::Kind::Image ? item->value : item->still;
  const bool image = !still.empty();
  place(tile.bg, x, y, w, h,
        RoundedRectStyle{.fill = item->kind == Item::Kind::Colour ? item->colour : hover ? kFill2 : kFill1, .radius = radius});

  tile.thumb->setVisible(image);
  if (image) {
    if (tile.source != still) {
      tile.source = still;
      tile.thumb->clear(renderer);
      (void)tile.thumb->setSourceFile(renderer, still, kThumbPx);
    }
    tile.thumb->setRadius(radius);
    tile.thumb->setPosition(x, y);
    tile.thumb->setFrameSize(w, h);
    tile.thumb->setSize(w, h);
    tile.thumb->layout(renderer);
  } else if (!tile.source.empty()) {
    tile.source.clear();
    tile.thumb->clear(renderer);
  }

  // A picture names nothing; "Same as desktop" and a live item without its still show their name.
  const bool named = !image && item->kind != Item::Kind::Colour;
  tile.name->setVisible(named);
  if (named) {
    tile.name->setText(item->name);
    tile.name->setFontSize(12.0F * s);
    tile.name->setColor(kLabel1);
    tile.name->setMaxWidth(w - 16.0F * s);
    tile.name->measure(renderer);
    tile.name->setPosition(std::round(x + 8.0F * s), std::round(y + 8.0F * s));
  }
  const bool badged = item->kind == Item::Kind::Video || item->kind == Item::Kind::Shader;
  tile.badge->setVisible(badged);
  tile.badgeText->setVisible(badged);
  if (badged) {
    tile.badgeText->setText(
        item->kind == Item::Kind::Video ? "▶ " + i18n::tr("lockscreen.editor.wallpaper-live")
                                        : "✦ " + i18n::tr("lockscreen.editor.wallpaper-shader")
    );
    tile.badgeText->setFontSize(11.0F * s);
    tile.badgeText->setColor(kLabel1);
    tile.badgeText->measure(renderer);
    const float bh = 20.0F * s;
    const float bw = std::round(tile.badgeText->width() + 12.0F * s);
    const float bx = x + 6.0F * s;
    const float by = y + h - 6.0F * s - bh;
    place(tile.badge, bx, by, bw, bh, RoundedRectStyle{.fill = kBadge, .radius = bh * 0.5F});
    tile.badgeText->setPosition(std::round(bx + 6.0F * s), std::round(by + (bh - tile.badgeText->height()) * 0.5F));
  }
  // Mock: the current wallpaper has an inset 2 px accent ring; the keyboard focus the accent-text ring.
  const bool ringed = picked || focused;
  tile.ring->setVisible(ringed);
  if (ringed) {
    place(tile.ring, x, y, w, h,
          RoundedRectStyle{
              .fill = kClear, .border = focused ? kAccentText : kAccent, .radius = radius, .borderWidth = 2.0F * s
          });
  }
}

void LockEditorWallpaper::layoutSlider(
    Renderer& renderer, Slider& slider, const char* label, float fraction, const std::string& value, float x, float y,
    float w, float s, bool focused
) {
  const float rowH = kSliderRow * s;
  slider.rect = {x, y, w, rowH};
  slider.label->setText(i18n::tr(label));
  slider.label->setFontSize(13.0F * s);
  slider.label->setColor(kLabel2);
  slider.label->measure(renderer);
  slider.label->setPosition(x, std::round(y + (rowH - slider.label->height()) * 0.5F));
  slider.value->setText(value);
  slider.value->setFontSize(12.0F * s);
  slider.value->setColor(kLabel2);
  slider.value->measure(renderer);
  const float valueW = 40.0F * s;
  slider.value->setPosition(std::round(x + w - valueW), std::round(y + (rowH - slider.value->height()) * 0.5F));
  const float labelW = 96.0F * s + 12.0F * s;
  const float trackX = x + labelW;
  const float trackW = std::max(1.0F, w - labelW - valueW - 8.0F * s);
  const float trackH = kTrackHeight * s;
  const float trackY = std::round(y + (rowH - trackH) * 0.5F);
  slider.trackRect = {trackX, y, trackW, rowH};
  place(slider.track, trackX, trackY, trackW, trackH,
        RoundedRectStyle{
            .fill = kFill2,
            .border = focused ? kAccentText : kClear,
            .radius = trackH * 0.5F,
            .borderWidth = focused ? 2.0F * s : 0.0F,
        });
  const float fillW = std::max(trackH, trackW * std::clamp(fraction, 0.0F, 1.0F));
  place(slider.fill, trackX, trackY, fillW, trackH, RoundedRectStyle{.fill = kOn, .radius = trackH * 0.5F});
}

void LockEditorWallpaper::layout(
    Renderer& renderer, const State& state, const std::vector<Item>& items, float sw, float sh, float s
) {
  m_group->setVisible(state.open);
  if (!state.open) {
    m_panelRect = {};
    for (Tile& tile : m_tiles) {
      tile.rect = {};
    }
    return;
  }
  const float pad = kPad * s;
  const float gap = kGap * s;
  const float width = std::min(kPanelWidth * s, sw - 2.0F * pad);
  const float tileW = std::floor((width - 2.0F * pad - gap * (kColumns - 1)) / kColumns);
  const float tileH = kTileHeight * s;
  const float tabH = kTabHeight * s;
  const float gridH = tileH * kRows + gap * (kRows - 1);
  const float height = pad + tabH + gap * 1.5F + gridH + 12.0F * s + kSliderRow * s + 8.0F * s + 16.0F * s + pad;
  const float x = std::round((sw - width) * 0.5F);
  const float y = std::round(sh - height - 24.0F * s);
  m_panelRect = {x, y, width, height};
  place(m_panelBg, x, y, width, height,
        RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 28.0F * s, .borderWidth = 1.0F});

  float cx = x + pad;
  for (int i = 0; i < kTabs; ++i) {
    Chip& tab = m_tabs[static_cast<std::size_t>(i)];
    tab.label->setText(i18n::tr(kTabKeys[static_cast<std::size_t>(i)]));
    float w = 0.0F;
    layoutChip(renderer, tab.bg, tab.label, cx, y + pad, tabH, s, state.tab == i, state.focus == Hit{Target::Tab, i},
               state.hover == Hit{Target::Tab, i}, false, w);
    tab.rect = {cx, y + pad, w, tabH};
    cx += w + gap;
  }
  m_scope.label->setText(
      i18n::tr(state.allDisplays ? "lockscreen.editor.wallpaper-all-displays" : "lockscreen.editor.wallpaper-this-display")
      + " ▾"
  );
  float scopeW = 0.0F;
  m_scope.label->measure(renderer);
  const float scopeX = x + width - pad - std::round(m_scope.label->width() + 20.0F * s);
  layoutChip(renderer, m_scope.bg, m_scope.label, scopeX, y + pad, tabH, s, false,
             state.focus.target == Target::Scope, state.hover.target == Target::Scope, true, scopeW);
  m_scope.rect = {scopeX, y + pad, scopeW, tabH};
  m_scopeKey->setText(i18n::tr("lockscreen.editor.wallpaper-applies-to"));
  m_scopeKey->setFontSize(12.0F * s);
  m_scopeKey->setColor(kLabel2);
  m_scopeKey->measure(renderer);
  m_scopeKey->setPosition(
      std::round(scopeX - 8.0F * s - m_scopeKey->width()), std::round(y + pad + (tabH - m_scopeKey->height()) * 0.5F)
  );

  const float gridY = y + pad + tabH + gap * 1.5F;
  m_firstItem = state.scrollRow * kColumns;
  for (int i = 0; i < kColumns * kRows; ++i) {
    const int index = m_firstItem + i;
    const Item* item = index < static_cast<int>(items.size()) ? &items[static_cast<std::size_t>(index)] : nullptr;
    const float tx = x + pad + static_cast<float>(i % kColumns) * (tileW + gap);
    const float ty = gridY + static_cast<float>(i / kColumns) * (tileH + gap);
    layoutTile(renderer, m_tiles[static_cast<std::size_t>(i)], item, tx, ty, tileW, tileH, s,
               item != nullptr && item->value == state.picked, state.focus == Hit{Target::Tile, index},
               state.hover == Hit{Target::Tile, index});
  }
  m_empty->setVisible(items.empty());
  if (items.empty()) {
    m_empty->setText(state.emptyText);
    m_empty->setFontSize(13.0F * s);
    m_empty->setColor(kLabel2);
    m_empty->setMaxWidth(width - 2.0F * pad);
    m_empty->measure(renderer);
    m_empty->setPosition(std::round(x + (width - m_empty->width()) * 0.5F),
                         std::round(gridY + (gridH - m_empty->height()) * 0.5F));
  }

  const float sliderY = gridY + gridH + 12.0F * s;
  const float sliderW = (width - 2.0F * pad - 24.0F * s) * 0.5F;
  layoutSlider(renderer, m_blur, "lockscreen.editor.wallpaper-blur", state.blur,
               std::format("{}", static_cast<int>(std::lround(state.blur * 100.0F))), x + pad, sliderY, sliderW, s,
               state.focus.target == Target::Blur);
  layoutSlider(renderer, m_dim, "lockscreen.editor.wallpaper-dim", state.dim / kMaxDim,
               std::format("{} %", static_cast<int>(std::lround(state.dim * 100.0F))), x + pad + sliderW + 24.0F * s,
               sliderY, sliderW, s, state.focus.target == Target::Dim);

  m_foot->setText(i18n::tr("lockscreen.editor.wallpaper-foot"));
  m_foot->setFontSize(12.0F * s);
  m_foot->setColor(kLabel2);
  m_foot->setMaxWidth(width - 2.0F * pad);
  m_foot->setEllipsize(TextEllipsize::End);
  m_foot->setMaxLines(1);
  m_foot->measure(renderer);
  m_foot->setPosition(x + pad, std::round(sliderY + kSliderRow * s + 8.0F * s));
}

LockEditorWallpaper::Hit LockEditorWallpaper::hitTest(float x, float y) const {
  if (!m_panelRect.contains(x, y)) {
    return {};
  }
  for (int i = 0; i < kTabs; ++i) {
    if (m_tabs[static_cast<std::size_t>(i)].rect.contains(x, y)) {
      return {Target::Tab, i};
    }
  }
  if (m_scope.rect.contains(x, y)) {
    return {Target::Scope, -1};
  }
  for (std::size_t i = 0; i < m_tiles.size(); ++i) {
    if (m_tiles[i].rect.contains(x, y)) {
      return {Target::Tile, m_firstItem + static_cast<int>(i)};
    }
  }
  if (m_blur.trackRect.contains(x, y)) {
    return {Target::Blur, -1};
  }
  if (m_dim.trackRect.contains(x, y)) {
    return {Target::Dim, -1};
  }
  return {Target::Panel, -1};
}

float LockEditorWallpaper::sliderFraction(Target target, float x) const {
  const Rect& track = target == Target::Blur ? m_blur.trackRect : m_dim.trackRect;
  return track.w > 0.0F ? std::clamp((x - track.x) / track.w, 0.0F, 1.0F) : 0.0F;
}

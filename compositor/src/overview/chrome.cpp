#include "overview/chrome.h"

#include "config/config.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "wlr.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <format>

namespace andromeda::ov {

  namespace {

    constexpr int kNameHeight = 22;
    constexpr int kNameGap = 6;
    constexpr int kToolHeight = 32;
    constexpr int kChipHeight = 24;
    constexpr int kFieldHeight = 32;
    constexpr int kCapsuleHeight = 26;
    constexpr int kCapsuleIcon = 16; // app icon in a title capsule (prototype .lab)
    constexpr int kIconGap = 6;      // icon to title
    constexpr int kCapsuleGap = 6;
    constexpr int kCapsuleMaxWidth = 260;
    constexpr int kCapsuleMinCard = 60; // a card narrower than this shows no title
    constexpr int kCloseMinCard = 80;   // nor, narrower than this, a close button
    constexpr int kPlusRadius = 12;
    constexpr int kCloseSize = 26;
    constexpr int kCloseInset = 4; // the button centres this far inside the card's top-right corner
    constexpr int kMiniRadius = 3;
    constexpr std::array<float, 4> kCloseRed{0.898F, 0.282F, 0.302F, 1.0F};
    constexpr int kMenuWidth = 280;
    constexpr int kMenuPad = 6;
    constexpr int kMenuHeadHeight = 26;
    constexpr int kMenuRowHeight = 48;
    constexpr int kMenuGap = 6; // between the chip and the menu
    constexpr int kMenuRadius = 14;
    constexpr int kMenuArtWidth = 44;
    constexpr int kMenuArtHeight = 28;
    constexpr int kWindowMenuWidth = 240;
    constexpr int kTabHeight = 24;
    constexpr int kTabPad = 4;
    constexpr int kTabBottom = 24; // tab bar above the usable bottom, where the dock sits
    constexpr int kWindowMenuRowHeight = 32;

    // One block of a row's drawing, in its 44 x 28 box (prototype MODE_SVG, cut to the box).
    struct ArtBlock {
      int x, y, w, h;
      float alpha;
    };
    struct MenuRow {
      std::string_view name;
      std::string_view description;
      std::array<ArtBlock, 4> art;
      size_t blocks;
    };
    constexpr std::array<MenuRow, kMenuRows> kMenu{{
        {"Free", "Windows stay where you put them. Snap to edges and layouts.",
         {{{3, 3, 22, 14, 1.0F}, {17, 10, 24, 15, 0.7F}}}, 2},
        {"Scrolling", "Columns on an endless strip. New windows open to the right.",
         {{{0, 3, 8, 22, 0.35F}, {11, 3, 14, 22, 1.0F}, {28, 3, 14, 22, 1.0F}}}, 3},
        {"Split", "Each new window splits the focused one in half.",
         {{{3, 3, 18, 22, 1.0F}, {23, 3, 18, 10, 1.0F}, {23, 15, 8, 10, 1.0F}, {33, 15, 8, 10, 1.0F}}}, 4},
        {"Main + Stack", "One large window, the rest stacked beside it.",
         {{{3, 3, 23, 22, 1.0F}, {28, 3, 13, 6, 1.0F}, {28, 11, 13, 6, 1.0F}, {28, 19, 13, 6, 1.0F}}}, 4},
    }};

    bool boxHolds(const wlr_box& box, double lx, double ly) {
      return lx >= box.x && lx < box.x + box.width && ly >= box.y && ly < box.y + box.height;
    }

    std::array<float, 4> withAlpha(std::array<float, 4> color, float alpha) {
      color[3] = alpha;
      return color;
    }

    // Chrome fades in over the last fifth of the open, after the cards have mostly arrived.
    float lateFade(double progress) {
      const double t = std::clamp((progress - 0.8) / 0.2, 0.0, 1.0);
      return static_cast<float>(t * t * (3.0 - 2.0 * t));
    }

    std::string span(const std::array<float, 4>& color, std::string_view weight, std::string_view text) {
      return std::format("<span foreground='{}' weight='{}'>{}</span>", rgbaHex(color), weight, escapeMarkup(text));
    }

    void destroyNode(wlr_scene_tree*& tree) {
      if (tree != nullptr) {
        wlr_scene_node_destroy(&tree->node);
        tree = nullptr;
      }
    }

  } // namespace

  Chrome::Chrome(wlr_scene_tree* parent) : m_tree(wlr_scene_tree_create(parent)) {}

  void Chrome::raise() {
    if (m_tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_tree->node);
    }
  }

  const void* Chrome::closeAt(double lx, double ly) const {
    if (m_closeKey == nullptr) {
      return nullptr;
    }
    const double r = kCloseSize / 2.0;
    const double dx = lx - (m_closeBox.x + r);
    const double dy = ly - (m_closeBox.y + r);
    return dx * dx + dy * dy <= r * r ? m_closeKey : nullptr;
  }

  std::optional<size_t> Chrome::chipAt(double lx, double ly) const {
    for (const auto& [workspace, box] : m_chips) {
      if (boxHolds(box, lx, ly)) {
        return workspace;
      }
    }
    return std::nullopt;
  }

  int Chrome::menuRowAt(double lx, double ly) const {
    if (!m_menuBox.has_value()) {
      return -1;
    }
    for (int row = 0; row < kMenuRows; ++row) {
      if (boxHolds(m_menuRowBoxes[static_cast<size_t>(row)], lx, ly)) {
        return row;
      }
    }
    return -1;
  }

  bool Chrome::titleAt(double lx, double ly) const { return m_titleBox.has_value() && boxHolds(*m_titleBox, lx, ly); }

  bool Chrome::menuContains(double lx, double ly) const { return m_menuBox.has_value() && boxHolds(*m_menuBox, lx, ly); }

  std::optional<size_t> Chrome::tabAt(double lx, double ly) const {
    for (const auto& [index, box] : m_tabBoxes) {
      if (boxHolds(box, lx, ly)) {
        return index;
      }
    }
    // The + target past the thumbs adds a workspace like the + tab.
    if (m_plus.alpha > 0.0F && boxHolds(m_plus.box, lx, ly)) {
      return std::numeric_limits<size_t>::max();
    }
    return std::nullopt;
  }

  int Chrome::windowMenuRowAt(double lx, double ly) const {
    if (!m_windowMenuBox.has_value()) {
      return -1;
    }
    for (size_t row = 0; row < m_windowMenuRows.size(); ++row) {
      if (boxHolds(m_windowMenuRows[row], lx, ly)) {
        return static_cast<int>(row);
      }
    }
    return -1;
  }

  bool Chrome::windowMenuContains(double lx, double ly) const {
    return m_windowMenuBox.has_value() && boxHolds(*m_windowMenuBox, lx, ly);
  }

  void Chrome::setLabel(Label& label, const std::string& markup, const LabelStyle& style, double scale) {
    label.used = true;
    std::string key = std::format(
        "{}|{}|{}|{}|{}|{}|{}|{}", markup, style.font, scale, style.maxWidth, style.height, style.wrap,
        static_cast<const void*>(style.icon), style.iconSize
    );
    if (label.tree != nullptr && key == label.key) {
      return;
    }
    if (label.tree == nullptr) {
      label.tree = wlr_scene_tree_create(m_tree);
      if (label.tree == nullptr) {
        return;
      }
      wlr_scene_node_set_enabled(&label.tree->node, false);
      label.alpha = -1.0F;
    }
    if (label.text != nullptr) {
      wlr_scene_node_destroy(&label.text->node);
      label.text = nullptr;
    }
    const int lead = style.icon != nullptr ? style.iconSize + kIconGap : 0;
    const int textMax = std::max(1, style.maxWidth - 2 * style.padX - lead);
    TextBufferResult rendered = renderTextBuffer({
        .markup = markup,
        .font = std::string(style.font),
        .maxWidth = textMax,
        .padding = 0,
        .scale = scale,
        .ellipsize = !style.wrap,
    });
    label.key = std::move(key);
    if (rendered.buffer == nullptr) {
      label.textWidth = label.textHeight = 0;
      return;
    }
    label.textWidth = rendered.logicalWidth;
    label.textHeight = rendered.logicalHeight;
    const bool capsule = style.height > 0;
    label.height = capsule ? style.height : rendered.logicalHeight;
    label.width = capsule ? std::max(lead + label.textWidth + 2 * style.padX, style.height) : label.textWidth;
    label.color = style.fill;
    if (capsule) {
      if (label.fill == nullptr) {
        label.fill = wlr_scene_rect_create(label.tree, 1, 1, std::array<float, 4>{}.data());
      }
      if (label.fill != nullptr) {
        wlr_scene_rect_set_size(label.fill, label.width, label.height);
        wlr_scene_rect_set_clipped_region(label.fill, clipped_region_get_default());
        wlr_scene_rect_set_corner_radii(label.fill, corner_radii_all(label.height / 2));
      }
    }
    label.text = wlr_scene_buffer_create(label.tree, rendered.buffer);
    wlr_buffer_drop(rendered.buffer);
    if (label.text == nullptr) {
      return;
    }
    label.text->point_accepts_input = [](wlr_scene_buffer*, double*, double*) { return false; };
    wlr_scene_buffer_set_dest_size(label.text, label.textWidth, label.textHeight);
    const int left = (label.width - lead - label.textWidth) / 2;
    wlr_scene_node_set_position(&label.text->node, left + lead, (label.height - label.textHeight) / 2);
    if (style.icon != nullptr) {
      if (label.icon == nullptr) {
        label.icon = wlr_scene_buffer_create(label.tree, style.icon);
      } else {
        wlr_scene_buffer_set_buffer(label.icon, style.icon);
      }
      if (label.icon != nullptr) {
        label.icon->point_accepts_input = [](wlr_scene_buffer*, double*, double*) { return false; };
        wlr_scene_buffer_set_dest_size(label.icon, style.iconSize, style.iconSize);
        wlr_scene_node_set_position(&label.icon->node, left, (label.height - style.iconSize) / 2);
      }
    } else if (label.icon != nullptr) {
      wlr_scene_node_destroy(&label.icon->node);
      label.icon = nullptr;
    }
    label.alpha = -1.0F; // the new buffer starts opaque
  }

  void Chrome::placeLabel(Label& label, int x, int y, float alpha) {
    if (label.tree == nullptr) {
      return;
    }
    const bool on = alpha > 0.004F && label.text != nullptr;
    wlr_scene_node_set_enabled(&label.tree->node, on);
    if (!on) {
      return;
    }
    wlr_scene_node_set_position(&label.tree->node, x, y);
    if (alpha == label.alpha) {
      return;
    }
    label.alpha = alpha;
    wlr_scene_buffer_set_opacity(label.text, alpha);
    if (label.icon != nullptr) {
      wlr_scene_buffer_set_opacity(label.icon, alpha);
    }
    if (label.fill != nullptr) {
      std::array<float, 4> fill{};
      premultiplied(fill.data(), label.color, alpha);
      wlr_scene_rect_set_color(label.fill, fill.data());
    }
  }

  void Chrome::ensureFrame(Frame& frame) {
    if (frame.tree != nullptr) {
      return;
    }
    frame.tree = wlr_scene_tree_create(m_tree);
    if (frame.tree == nullptr) {
      return;
    }
    const std::array<float, 4> clear{};
    frame.fill = wlr_scene_rect_create(frame.tree, 1, 1, clear.data());
    frame.ring = wlr_scene_border_create(frame.tree, clear.data(), clear.data());
    frame.box = {};
    frame.alpha = -1.0F;
  }

  void Chrome::placeFrame(
      Frame& frame, const wlr_box& box, int radius, int ringWidth, const std::array<float, 4>& fill,
      const std::array<float, 4>& ring, float alpha
  ) {
    ensureFrame(frame);
    if (frame.tree == nullptr) {
      return;
    }
    const bool on = alpha > 0.004F && box.width > 0 && box.height > 0;
    wlr_scene_node_set_enabled(&frame.tree->node, on);
    if (!on) {
      return;
    }
    wlr_scene_node_set_position(&frame.tree->node, box.x, box.y);
    if (box.width != frame.box.width || box.height != frame.box.height) {
      frame.box = box;
      if (frame.fill != nullptr) {
        wlr_scene_rect_set_size(frame.fill, box.width, box.height);
        wlr_scene_rect_set_clipped_region(frame.fill, clipped_region_get_default());
        wlr_scene_rect_set_corner_radii(frame.fill, corner_radii_all(radius));
      }
      if (frame.ring != nullptr) {
        applyBorderGeometry(frame.ring, makeBorderRing(box.width, box.height, radius, ringWidth, 0), ringWidth, 0);
      }
      frame.alpha = -1.0F;
    }
    frame.box.x = box.x;
    frame.box.y = box.y;
    if (alpha == frame.alpha) {
      return;
    }
    frame.alpha = alpha;
    std::array<float, 4> fillColor{};
    std::array<float, 4> ringColor{};
    premultiplied(fillColor.data(), fill, alpha);
    premultiplied(ringColor.data(), ring, alpha);
    if (frame.fill != nullptr) {
      wlr_scene_node_set_enabled(&frame.fill->node, fill[3] > 0.0F);
      wlr_scene_rect_set_color(frame.fill, fillColor.data());
    }
    if (frame.ring != nullptr) {
      wlr_scene_border_set_colors(frame.ring, ringColor.data(), ringColor.data());
    }
  }

  void Chrome::update(const ChromeFrame& frame) {
    if (m_tree == nullptr || frame.placement == nullptr) {
      return;
    }
    const Placement& placement = *frame.placement;
    const auto& colors = config().colors;
    const std::array<float, 4> thick = withAlpha(colors.background, 0.92F);
    const std::array<float, 4> thin = withAlpha(colors.background, 0.62F);
    const std::array<float, 4>& l1 = colors.textPrimary;
    const std::array<float, 4> l2 = withAlpha(colors.textPrimary, 0.62F);
    const std::array<float, 4> l3 = withAlpha(colors.textPrimary, 0.38F);
    const double scale = frame.scale;
    const float shown = static_cast<float>(std::clamp(frame.progress, 0.0, 1.0));
    const float late = lateFade(frame.progress);
    const int ox = frame.output.x;
    const int oy = frame.output.y;
    const auto px = [](double value) { return static_cast<int>(std::lround(value)); };

    m_chips.clear();
    for (Label& label : m_names) {
      label.used = false;
    }
    m_names.resize(std::max(m_names.size(), frame.workspaces.size()));
    for (size_t index = 0; index < frame.workspaces.size(); ++index) {
      const ChromeWorkspace& workspace = frame.workspaces[index];
      Label& label = m_names[index];
      if (!workspace.shown) {
        continue;
      }
      std::string markup = span(l1, "600", workspace.name);
      if (workspace.kind != CardKind::Thumb && !workspace.arrangement.empty()) {
        markup += span(l2, "500", std::format("  ·  {}  ▾", workspace.arrangement));
      }
      setLabel(
          label, markup, {.font = "sans 9", .padX = 8, .height = kNameHeight, .maxWidth = 280, .fill = thin}, scale
      );
      const wlr_box& card = workspace.card;
      const bool above = workspace.kind == CardKind::Frame || frame.style == Style::TaskView;
      const int y = above ? card.y - kNameHeight - kNameGap : card.y + card.height + kNameGap;
      placeLabel(label, card.x, y, shown);
      if (workspace.kind != CardKind::Thumb && !workspace.arrangement.empty() && shown > 0.5F) {
        m_chips.emplace_back(index, wlr_box{card.x, y, label.width, label.height});
      }
    }
    for (Label& label : m_names) {
      if (!label.used && label.tree != nullptr) {
        wlr_scene_node_set_enabled(&label.tree->node, false);
      }
    }

    // Strip tabs: a thick capsule in the dock's place (the dock steps aside while open), current tab on fill 2.
    m_tabBoxes.clear();
    const size_t tabCount = frame.style == Style::Strip ? frame.workspaces.size() + 1 : 0;
    m_tabs.resize(std::max(m_tabs.size(), tabCount));
    int tabsWidth = kTabPad;
    for (size_t index = 0; index < tabCount; ++index) {
      const bool plus = index == frame.workspaces.size();
      const std::string_view text = plus ? std::string_view("+") : frame.workspaces[index].name;
      setLabel(
          m_tabs[index], span(index == frame.current ? l1 : l2, "600", text),
          {.font = "sans 9", .padX = 12, .height = kTabHeight, .maxWidth = 160}, scale
      );
      tabsWidth += m_tabs[index].width + kTabPad;
    }
    if (tabCount > 0) {
      const int barHeight = kTabHeight + 2 * kTabPad;
      const wlr_box bar{
          ox + (frame.output.width - tabsWidth) / 2, frame.usable.y + frame.usable.height - kTabBottom - barHeight,
          tabsWidth, barHeight
      };
      placeFrame(m_tabBar, bar, barHeight / 2, 1, thick, withAlpha(l1, 0.1F), late);
      if (m_tabBar.tree != nullptr) {
        wlr_scene_node_raise_to_top(&m_tabBar.tree->node);
      }
      int x = bar.x + kTabPad;
      for (size_t index = 0; index < tabCount; ++index) {
        Label& tab = m_tabs[index];
        const wlr_box box{x, bar.y + kTabPad, tab.width, kTabHeight};
        if (index == frame.current) {
          placeFrame(m_tabCurrent, box, kTabHeight / 2, 1, withAlpha(l1, 0.14F), {}, late);
          if (m_tabCurrent.tree != nullptr) {
            wlr_scene_node_raise_to_top(&m_tabCurrent.tree->node);
          }
        }
        placeLabel(tab, x, box.y + (kTabHeight - tab.height) / 2, late);
        // Frames are created lazily and may land above the text; the text stays on top.
        if (tab.tree != nullptr) {
          wlr_scene_node_raise_to_top(&tab.tree->node);
        }
        if (late > 0.5F) {
          m_tabBoxes.emplace_back(index, box);
        }
        x += tab.width + kTabPad;
      }
    } else {
      placeFrame(m_tabBar, {}, 0, 0, {}, {}, 0.0F);
      placeFrame(m_tabCurrent, {}, 0, 0, {}, {}, 0.0F);
    }
    for (size_t index = tabCount; index < m_tabs.size(); ++index) {
      placeLabel(m_tabs[index], 0, 0, 0.0F);
    }

    // Title row: the chosen workspace's name and its arrangement chip.
    const ChromeWorkspace* current =
        frame.current < frame.workspaces.size() ? &frame.workspaces[frame.current] : nullptr;
    if (placement.title.has_value() && current != nullptr) {
      if (frame.renaming.has_value()) {
        // Prototype .nm[contenteditable]: the name on a thick field with an accent ring; the caret ends the text.
        setLabel(
            m_title, span(l1, "bold", *frame.renaming) + span(colors.accentPrimary, "normal", "▏"),
            {.font = "sans 13.5", .padX = 6, .height = kToolHeight, .maxWidth = 420, .fill = thick}, scale
        );
      } else {
        setLabel(m_title, span(l1, "bold", current->name), {.font = "sans 13.5", .maxWidth = 420}, scale);
      }
      setLabel(
          m_chip, span(l1, "600", std::format("{}  ▾", current->arrangement)),
          {.font = "sans 9", .padX = 10, .height = kChipHeight, .maxWidth = 200, .fill = thick}, scale
      );
      const int x = ox + px(placement.title->x);
      const int y = oy + px(placement.title->y);
      placeLabel(m_title, x + 4, y + (kToolHeight - m_title.height) / 2, shown);
      m_titleBox = wlr_box{x + 4, y + (kToolHeight - m_title.height) / 2, m_title.width, m_title.height};
      placeFrame(m_titleRing, *m_titleBox, 6, 2, {}, colors.accentPrimary, frame.renaming.has_value() ? shown : 0.0F);
      const wlr_box chip{x + 4 + m_title.width + 10, y + (kToolHeight - kChipHeight) / 2, m_chip.width, kChipHeight};
      placeLabel(m_chip, chip.x, chip.y, shown);
      if (shown > 0.5F) {
        m_chips.emplace_back(frame.current, chip);
      }
    } else {
      placeLabel(m_title, 0, 0, 0.0F);
      placeLabel(m_chip, 0, 0, 0.0F);
      placeFrame(m_titleRing, {}, 0, 0, {}, {}, 0.0F);
      m_titleBox.reset();
    }

    if (placement.filter.has_value()) {
      const std::string markup = frame.filter.empty() ? span(l3, "normal", "⌕  Type to search windows")
                                                      : span(l1, "normal", std::format("⌕  {}", frame.filter));
      setLabel(
          m_filter, markup,
          {.font = "sans 9.75",
           .padX = 14,
           .height = kFieldHeight,
           .maxWidth = static_cast<int>(kFilterWidth),
           .fill = thick},
          scale
      );
      placeLabel(m_filter, ox + px(placement.filter->x), oy + px(placement.filter->y), shown);
    } else {
      placeLabel(m_filter, 0, 0, 0.0F);
    }

    const int usableBottom = frame.usable.y + frame.usable.height;

    // The shown workspace's windows live in the main area; its thumb shows them as see-through blocks (prototype
    // .mini i), so the desktop shows behind them and a maximized window does not turn the thumb solid.
    size_t minis = 0;
    if (current != nullptr && current->shown && current->kind == CardKind::Thumb && frame.output.width > 0) {
      const double z = static_cast<double>(current->card.width) / frame.output.width;
      const std::array<float, 4> fill = withAlpha(colors.textPrimary, 0.14F);
      const std::array<float, 4> line = withAlpha(colors.textPrimary, 0.3F);
      for (const Rect& r : placement.currentDesk) {
        if (minis == m_minis.size()) {
          m_minis.emplace_back();
        }
        const wlr_box box{
            current->card.x + px(r.x * z), current->card.y + px(r.y * z), std::max(1, px(r.w * z)),
            std::max(1, px(r.h * z))
        };
        placeFrame(m_minis[minis++], box, kMiniRadius, 1, fill, line, shown);
      }
    }
    for (; minis < m_minis.size(); ++minis) {
      placeFrame(m_minis[minis], {}, kMiniRadius, 1, {}, {}, 0.0F);
    }

    // The shown workspace's card carries an accent ring just outside it (prototype .card.cur: 3 px on thumbs, 2 px on
    // full-size rows and tiles).
    if (current != nullptr && current->shown && current->card.width > 0) {
      const int width = current->kind == CardKind::Thumb ? 3 : 2;
      const wlr_box& card = current->card;
      const wlr_box box{card.x - width, card.y - width, card.width + 2 * width, card.height + 2 * width};
      placeFrame(m_current, box, kPlusRadius + width, width, {}, colors.accentPrimary, shown);
    } else {
      placeFrame(m_current, {}, kPlusRadius, 2, {}, {}, 0.0F);
    }

    if (placement.plus.has_value()) {
      const Rect& plus = *placement.plus;
      const wlr_box box{ox + px(plus.x), oy + px(plus.y), px(plus.w), px(plus.h)};
      placeFrame(m_plus, box, kPlusRadius, 2, {0.5F, 0.5F, 0.5F, 0.08F}, l3, shown);
      setLabel(m_plusMark, span(l2, "normal", "+"), {.font = "sans 21"}, scale);
      placeLabel(
          m_plusMark, box.x + (box.width - m_plusMark.width) / 2, box.y + (box.height - m_plusMark.height) / 2, shown
      );
    } else {
      placeFrame(m_plus, {}, kPlusRadius, 2, {}, {}, 0.0F);
      placeLabel(m_plusMark, 0, 0, 0.0F);
    }


    // Window title capsules, centred under their cards, in card stacking order.
    const int capsuleFloor = usableBottom - kCapsuleGap;
    for (auto& [key, label] : m_titles) {
      label.used = false;
    }
    for (const ChromeWindow& window : frame.windows) {
      if (window.key == nullptr) {
        continue;
      }
      Label& label = m_titles[window.key];
      setLabel(
          label, span(l1, "600", window.title.empty() ? std::string_view("Untitled") : window.title),
          {.font = "sans 9",
           .padX = 10,
           .height = kCapsuleHeight,
           .maxWidth = kCapsuleMaxWidth,
           .fill = thick,
           .icon = window.icon,
           .iconSize = kCapsuleIcon},
          scale
      );
      const int centre = window.box.x + window.box.width / 2;
      // An off-screen Strip column sits beside its card; clamped in, its capsule would cover a neighbour's.
      const bool inCard = !window.within.has_value()
          || (centre >= window.within->x && centre < window.within->x + window.within->width);
      const bool wanted = (frame.titlesAlways || window.selected || window.key == frame.hovered)
          && inCard
          && !window.dragged
          && window.box.width > kCapsuleMinCard
          && window.opacity > 0.45F;
      int x = window.box.x + (window.box.width - label.width) / 2;
      if (window.within.has_value()) {
        const wlr_box& card = *window.within;
        x = std::clamp(x, card.x, std::max(card.x, card.x + card.width - label.width));
      }
      // A card reaching down to the usable bottom, or to the bottom of its workspace card, carries its capsule over its own
      // bottom edge instead.
      const int floor =
          window.within.has_value() ? window.within->y + window.within->height - kCapsuleGap : capsuleFloor;
      const int y = std::min(window.box.y + window.box.height + kCapsuleGap, floor - label.height);
      placeLabel(label, x, y, wanted ? late : 0.0F);
      if (label.tree != nullptr) {
        wlr_scene_node_raise_to_top(&label.tree->node);
      }
    }
    // The hovered card's close button, centred just inside its top-right corner, above every capsule.
    const ChromeWindow* closeFor = nullptr;
    for (const ChromeWindow& window : frame.windows) {
      if (window.key != nullptr && window.key == frame.hovered) {
        closeFor = &window;
      }
    }
    m_closeKey = nullptr;
    if (closeFor != nullptr
        && !closeFor->dragged
        && closeFor->box.width > kCloseMinCard
        && closeFor->opacity > 0.45F
        && late > 0.5F) {
      const LabelStyle style{.font = "sans 9.75", .height = kCloseSize, .maxWidth = kCloseSize, .fill = thick};
      setLabel(m_close, span(l1, "bold", "✕"), style, scale);
      LabelStyle hot = style;
      hot.fill = kCloseRed;
      setLabel(m_closeHot, span({1.0F, 1.0F, 1.0F, 1.0F}, "bold", "✕"), hot, scale);
      m_closeBox = {
          closeFor->box.x + closeFor->box.width - kCloseInset - kCloseSize / 2,
          closeFor->box.y + kCloseInset - kCloseSize / 2,
          kCloseSize,
          kCloseSize,
      };
      m_closeKey = closeFor->key;
      Label& on = frame.closeHovered ? m_closeHot : m_close;
      Label& off = frame.closeHovered ? m_close : m_closeHot;
      placeLabel(off, 0, 0, 0.0F);
      placeLabel(on, m_closeBox.x, m_closeBox.y, late);
      if (on.tree != nullptr) {
        wlr_scene_node_raise_to_top(&on.tree->node);
      }
    } else {
      placeLabel(m_close, 0, 0, 0.0F);
      placeLabel(m_closeHot, 0, 0, 0.0F);
    }

    // The arrangement menu, under its chip (above it when the output ends first), over everything else.
    std::optional<wlr_box> anchor;
    if (frame.menu.has_value()) {
      for (const auto& [workspace, box] : m_chips) {
        if (workspace == frame.menu->workspace) {
          anchor = box;
        }
      }
    }
    const auto raise = [](wlr_scene_tree* tree) {
      if (tree != nullptr) {
        wlr_scene_node_raise_to_top(&tree->node);
      }
    };
    m_menuBox.reset();
    if (anchor.has_value() && frame.workspaces.size() > frame.menu->workspace) {
      const int height = 2 * kMenuPad + kMenuHeadHeight + kMenuRows * kMenuRowHeight;
      const int right = frame.output.x + frame.output.width - kMenuPad - kMenuWidth;
      const int x = std::max(frame.output.x + kMenuPad, std::min(anchor->x, right));
      int y = anchor->y + anchor->height + kMenuGap;
      if (y + height > frame.output.y + frame.output.height - kMenuPad) {
        y = anchor->y - kMenuGap - height;
      }
      m_menuBox = wlr_box{x, y, kMenuWidth, height};
      // No backdrop blur under this panel, so it is near solid: the cards' edges must not show through.
      placeFrame(m_menu, *m_menuBox, kMenuRadius, 1, withAlpha(colors.background, 0.97F), withAlpha(l1, 0.1F), 1.0F);
      raise(m_menu.tree);
      const std::string_view name = frame.workspaces[frame.menu->workspace].name;
      setLabel(m_menuHead, span(l2, "600", std::format("Arrange “{}”", name)), {.font = "sans 8.25", .maxWidth = 240}, scale);
      placeLabel(m_menuHead, x + kMenuPad + 10, y + kMenuPad + (kMenuHeadHeight - m_menuHead.height) / 2, 1.0F);
      raise(m_menuHead.tree);
      const int textX = kMenuPad + 10 + kMenuArtWidth + 10;
      for (size_t row = 0; row < kMenu.size(); ++row) {
        const MenuRow& item = kMenu[row];
        const wlr_box box{
            x + kMenuPad, y + kMenuPad + kMenuHeadHeight + static_cast<int>(row) * kMenuRowHeight,
            kMenuWidth - 2 * kMenuPad, kMenuRowHeight
        };
        m_menuRowBoxes[row] = box;
        if (static_cast<int>(row) == frame.menu->hovered) {
          placeFrame(m_menuHover, box, 8, 1, withAlpha(l1, 0.08F), {}, 1.0F);
          raise(m_menuHover.tree);
        }
        const int artX = box.x + 10;
        const int artY = box.y + (kMenuRowHeight - kMenuArtHeight) / 2;
        for (size_t block = 0; block < m_menuArt[row].size(); ++block) {
          Frame& art = m_menuArt[row][block];
          if (block >= item.blocks) {
            placeFrame(art, {}, 0, 0, {}, {}, 0.0F);
            continue;
          }
          const ArtBlock& b = item.art[block];
          placeFrame(art, {artX + b.x, artY + b.y, b.w, b.h}, 2, 1, colors.accentPrimary, {}, b.alpha);
          raise(art.tree);
        }
        std::string markup = span(l1, "600", item.name);
        if (static_cast<int>(row) == frame.menu->current) {
          markup += span(colors.accentPrimary, "bold", "  ✓");
        }
        markup += std::format(
            "\n<span size='small' foreground='{}'>{}</span>", rgbaHex(l2), escapeMarkup(item.description)
        );
        Label& label = m_menuLabels[row];
        setLabel(label, markup, {.font = "sans 9", .maxWidth = kMenuWidth - textX - kMenuPad - 10, .wrap = true}, scale);
        placeLabel(label, x + textX, box.y + (kMenuRowHeight - label.height) / 2, 1.0F);
        raise(label.tree);
      }
    }
    if (!m_menuBox.has_value() || frame.menu->hovered < 0) {
      placeFrame(m_menuHover, {}, 0, 0, {}, {}, 0.0F);
    }
    if (!m_menuBox.has_value()) {
      placeFrame(m_menu, {}, 0, 0, {}, {}, 0.0F);
      placeLabel(m_menuHead, 0, 0, 0.0F);
      for (size_t row = 0; row < kMenu.size(); ++row) {
        placeLabel(m_menuLabels[row], 0, 0, 0.0F);
        for (Frame& art : m_menuArt[row]) {
          placeFrame(art, {}, 0, 0, {}, {}, 0.0F);
        }
      }
    }

    // Window menu: plain text rows at the pointer, kept on the output; same panel look as the arrangement menu.
    m_windowMenuBox.reset();
    const size_t windowRows = frame.windowMenu.has_value() ? frame.windowMenu->rows.size() : 0;
    if (m_windowMenuLabels.size() < windowRows) {
      m_windowMenuLabels.resize(windowRows);
      m_windowMenuRows.resize(windowRows);
    }
    if (windowRows > 0) {
      const ChromeWindowMenu& wm = *frame.windowMenu;
      const int height = 2 * kMenuPad + static_cast<int>(windowRows) * kWindowMenuRowHeight;
      const int x = std::clamp(
          wm.x, frame.output.x + kMenuPad, frame.output.x + frame.output.width - kMenuPad - kWindowMenuWidth
      );
      const int y =
          std::clamp(wm.y, frame.output.y + kMenuPad, frame.output.y + frame.output.height - kMenuPad - height);
      m_windowMenuBox = wlr_box{x, y, kWindowMenuWidth, height};
      placeFrame(
          m_windowMenu, *m_windowMenuBox, kMenuRadius, 1, withAlpha(colors.background, 0.97F), withAlpha(l1, 0.1F), 1.0F
      );
      raise(m_windowMenu.tree);
      for (size_t row = 0; row < windowRows; ++row) {
        const wlr_box box{
            x + kMenuPad, y + kMenuPad + static_cast<int>(row) * kWindowMenuRowHeight, kWindowMenuWidth - 2 * kMenuPad,
            kWindowMenuRowHeight
        };
        m_windowMenuRows[row] = box;
        if (static_cast<int>(row) == wm.hovered) {
          placeFrame(m_windowMenuHover, box, 8, 1, withAlpha(l1, 0.08F), {}, 1.0F);
          raise(m_windowMenuHover.tree);
        }
        const bool danger = wm.dangerFrom >= 0 && static_cast<int>(row) >= wm.dangerFrom;
        Label& label = m_windowMenuLabels[row];
        setLabel(
            label, span(danger ? colors.error : l1, "normal", wm.rows[row]),
            {.font = "sans 9.75", .maxWidth = kWindowMenuWidth - 2 * kMenuPad - 24}, scale
        );
        placeLabel(label, box.x + 12, box.y + (kWindowMenuRowHeight - label.height) / 2, 1.0F);
        raise(label.tree);
      }
    }
    if (!m_windowMenuBox.has_value() || frame.windowMenu->hovered < 0) {
      placeFrame(m_windowMenuHover, {}, 0, 0, {}, {}, 0.0F);
    }
    if (!m_windowMenuBox.has_value()) {
      placeFrame(m_windowMenu, {}, 0, 0, {}, {}, 0.0F);
    }
    for (size_t row = windowRows; row < m_windowMenuLabels.size(); ++row) {
      placeLabel(m_windowMenuLabels[row], 0, 0, 0.0F);
    }

    for (auto it = m_titles.begin(); it != m_titles.end();) {
      if (it->second.used) {
        ++it;
        continue;
      }
      destroyNode(it->second.tree);
      it = m_titles.erase(it);
    }
  }

} // namespace andromeda::ov

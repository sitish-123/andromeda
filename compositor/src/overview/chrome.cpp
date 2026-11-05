#include "overview/chrome.h"

#include "config/config.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace andromeda::ov {

  namespace {

    constexpr int kNameHeight = 22;
    constexpr int kNameGap = 6;
    constexpr int kToolHeight = 32;
    constexpr int kChipHeight = 24;
    constexpr int kFieldHeight = 32;
    constexpr int kHintHeight = 24;
    constexpr int kHintBottom = 10;
    constexpr int kCapsuleHeight = 26;
    constexpr int kCapsuleGap = 6;
    constexpr int kCapsuleMaxWidth = 260;
    constexpr int kCapsuleMinCard = 60; // a card narrower than this shows no title
    constexpr int kCloseMinCard = 80;   // nor, narrower than this, a close button
    constexpr int kPlusRadius = 12;
    constexpr int kViewportRadius = 14;
    constexpr int kViewportTagHeight = 18;
    constexpr int kCloseSize = 26;
    constexpr int kCloseInset = 4; // the button centres this far inside the card's top-right corner
    constexpr std::array<float, 4> kCloseRed{0.898F, 0.282F, 0.302F, 1.0F};

    std::array<float, 4> withAlpha(std::array<float, 4> color, float alpha) {
      color[3] = alpha;
      return color;
    }

    // Chrome fades in over the last fifth of the open, after the cards have mostly arrived.
    float lateFade(double progress) {
      const double t = std::clamp((progress - 0.8) / 0.2, 0.0, 1.0);
      return static_cast<float>(t * t * (3.0 - 2.0 * t));
    }

    std::string_view hintText(Style style) {
      switch (style) {
      case Style::Spaces:
        return "Click a thumbnail to look at that workspace · drag windows onto thumbnails or each other · Esc closes";
      case Style::TaskView:
        return "Click a window to open it · drag it onto a desktop below · Esc closes";
      case Style::Strip:
        return "Wheel changes row · Shift+wheel pans the strip · Esc closes";
      case Style::Grid:
        return "Click a tile to go there · drag windows between tiles · Esc closes";
      }
      return "";
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

  void Chrome::setLabel(Label& label, const std::string& markup, const LabelStyle& style, double scale) {
    label.used = true;
    std::string key = std::format("{}|{}|{}|{}|{}", markup, style.font, scale, style.maxWidth, style.height);
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
    const int textMax = std::max(1, style.maxWidth - 2 * style.padX);
    TextBufferResult rendered = renderTextBuffer({
        .markup = markup,
        .font = std::string(style.font),
        .maxWidth = textMax,
        .padding = 0,
        .scale = scale,
        .ellipsize = true,
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
    label.width = capsule ? std::max(label.textWidth + 2 * style.padX, style.height) : label.textWidth;
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
    wlr_scene_node_set_position(
        &label.text->node, (label.width - label.textWidth) / 2, (label.height - label.textHeight) / 2
    );
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
        markup += span(l2, "500", std::format("  ·  {}", workspace.arrangement));
      }
      setLabel(
          label, markup, {.font = "sans 9", .padX = 8, .height = kNameHeight, .maxWidth = 280, .fill = thin}, scale
      );
      const wlr_box& card = workspace.card;
      const bool above = workspace.kind == CardKind::Frame || frame.style == Style::TaskView;
      const int y = above ? card.y - kNameHeight - kNameGap : card.y + card.height + kNameGap;
      placeLabel(label, card.x, y, shown);
    }
    for (Label& label : m_names) {
      if (!label.used && label.tree != nullptr) {
        wlr_scene_node_set_enabled(&label.tree->node, false);
      }
    }

    // Title row: the chosen workspace's name and its arrangement chip.
    const ChromeWorkspace* current =
        frame.current < frame.workspaces.size() ? &frame.workspaces[frame.current] : nullptr;
    if (placement.title.has_value() && current != nullptr) {
      setLabel(m_title, span(l1, "bold", current->name), {.font = "sans 13.5", .maxWidth = 420}, scale);
      setLabel(
          m_chip, span(l1, "600", std::format("{}  ▾", current->arrangement)),
          {.font = "sans 9", .padX = 10, .height = kChipHeight, .maxWidth = 200, .fill = thick}, scale
      );
      const int x = ox + px(placement.title->x);
      const int y = oy + px(placement.title->y);
      placeLabel(m_title, x + 4, y + (kToolHeight - m_title.height) / 2, shown);
      placeLabel(m_chip, x + 4 + m_title.width + 10, y + (kToolHeight - kChipHeight) / 2, shown);
    } else {
      placeLabel(m_title, 0, 0, 0.0F);
      placeLabel(m_chip, 0, 0, 0.0F);
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

    setLabel(
        m_hint, span(withAlpha({1.0F, 1.0F, 1.0F, 1.0F}, 0.75F), "normal", hintText(frame.style)),
        {.font = "sans 9",
         .padX = 12,
         .height = kHintHeight,
         .maxWidth = std::max(200, frame.output.width - 2 * static_cast<int>(kMargin)),
         .fill = {0.0F, 0.0F, 0.0F, 0.35F}},
        scale
    );
    const int usableBottom = frame.usable.y + frame.usable.height;
    placeLabel(
        m_hint, frame.output.x + (frame.output.width - m_hint.width) / 2, usableBottom - kHintBottom - m_hint.height,
        frame.hintHidden ? 0.0F
        : frame.style == Style::Spaces ? late
                                       : shown
    );

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

    if (placement.viewport.has_value()) {
      // The On screen frame grows from the output's work area as the overview opens.
      const Rect usable{
          static_cast<double>(frame.usable.x - ox), static_cast<double>(frame.usable.y - oy),
          static_cast<double>(frame.usable.width), static_cast<double>(frame.usable.height)
      };
      const Rect v = lerpRect(usable, *placement.viewport, frame.progress);
      const wlr_box box{ox + px(v.x), oy + px(v.y), px(v.w), px(v.h)};
      placeFrame(m_viewport, box, kViewportRadius, 2, {}, {1.0F, 1.0F, 1.0F, 0.85F}, shown);
      setLabel(
          m_viewportTag, span({1.0F, 1.0F, 1.0F, 1.0F}, "bold", "ON SCREEN"),
          {.font = "sans 8.25", .padX = 8, .height = kViewportTagHeight, .fill = colors.accentPrimary}, scale
      );
      placeLabel(m_viewportTag, box.x, box.y - kViewportTagHeight - 6, late);
    } else {
      placeFrame(m_viewport, {}, kViewportRadius, 2, {}, {}, 0.0F);
      placeLabel(m_viewportTag, 0, 0, 0.0F);
    }

    // Window title capsules, centred under their cards, in card stacking order.
    const int capsuleFloor = m_hint.tree != nullptr && m_hint.tree->node.enabled ? m_hint.tree->node.y - kCapsuleGap
                                                                                 : usableBottom - kCapsuleGap;
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
          {.font = "sans 9", .padX = 10, .height = kCapsuleHeight, .maxWidth = kCapsuleMaxWidth, .fill = thick}, scale
      );
      const bool wanted = (frame.titlesAlways || window.selected || window.key == frame.hovered)
          && !window.dragged
          && window.box.width > kCapsuleMinCard
          && window.opacity > 0.45F;
      int x = window.box.x + (window.box.width - label.width) / 2;
      if (window.within.has_value()) {
        const wlr_box& card = *window.within;
        x = std::clamp(x, card.x, std::max(card.x, card.x + card.width - label.width));
      }
      // A card reaching down to the hint, or to the bottom of its workspace card, carries its capsule over its own
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

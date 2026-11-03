#include "shell/lockscreen/lock_editor_slots.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "shell/lockscreen/lock_cards.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  // Same values as the clock gallery (lock_editor_gallery.cpp).
  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.92F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 0.10F);
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  constexpr Color kSlotEdge = rgba(1.0F, 1.0F, 1.0F, 0.55F);
  constexpr Color kClear = rgba(0.0F, 0.0F, 0.0F, 0.0F);
  constexpr float kPad = 16.0F;
  constexpr float kChipHeight = 28.0F;
  constexpr float kMinPanelWidth = 360.0F;

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

  void place(RectNode* node, const LockEditorSlots::Rect& r, const RoundedRectStyle& style) {
    node->setPosition(r.x, r.y);
    node->setSize(r.w, r.h);
    node->setStyle(style);
  }

  RoundedRectStyle outline(bool on, bool hover, float s) {
    return RoundedRectStyle{
        .fill = hover ? kFill1 : kClear,
        .border = on ? kAccentText : kSlotEdge,
        .radius = 16.0F * s,
        .borderWidth = (on ? 2.0F : 1.5F) * s,
    };
  }

  std::string kindLabel(std::string_view kind) { return i18n::tr("lockscreen.widgets." + std::string(kind)); }

} // namespace

std::vector<LockEditorSlots::Hit> LockEditorSlots::focusOrder(const State& state) {
  std::vector<Hit> order;
  if (!state.active) {
    return order;
  }
  for (int i = 0; i < state.places; ++i) {
    order.push_back({Target::Place, i});
  }
  if (state.bottom) {
    order.push_back({Target::BottomSlot, -1});
  }
  if (state.panel == Panel::Widget) {
    for (int i = 0; i < kKinds; ++i) {
      order.push_back({Target::Kind, i});
    }
    if (state.editPlace >= 0 && state.editPlace < static_cast<int>(state.widgets.size())) {
      order.push_back({Target::Remove, -1});
    }
  } else if (state.panel == Panel::Bottom) {
    for (int i = 0; i < static_cast<int>(kBottomNames.size()); ++i) {
      order.push_back({Target::BottomKind, i});
    }
  }
  return order;
}

void LockEditorSlots::build(Node& root) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(9);
  for (Place& p : m_places) {
    p.outline = addRect(*m_group);
    p.plus = addLabel(*m_group, FontWeight::Normal);
  }
  m_bottom = addRect(*m_group);
  m_panelBg = addRect(*m_group);
  m_title = addLabel(*m_group, FontWeight::SemiBold);
  const auto addChip = [this](LockEditorGallery::Chip& chip) {
    chip.bg = addRect(*m_group);
    chip.label = addLabel(*m_group, FontWeight::Medium);
  };
  std::ranges::for_each(m_kinds, addChip);
  addChip(m_remove);
  std::ranges::for_each(m_bottomKinds, addChip);
}

void LockEditorSlots::layout(
    Renderer& renderer, const State& state, const std::vector<Rect>& placeRects, Rect bottomRect, float sw, float sh,
    float scale
) {
  m_state = state;
  const float s = std::max(0.1F, scale);
  const int placed = static_cast<int>(state.widgets.size());
  for (int i = 0; i < kPlaces; ++i) {
    Place& p = m_places[static_cast<std::size_t>(i)];
    const bool shown = state.active && i < state.places && i < static_cast<int>(placeRects.size());
    p.outline->setVisible(shown);
    p.plus->setVisible(shown && i >= placed);
    p.rect = shown ? placeRects[static_cast<std::size_t>(i)] : Rect{};
    if (!shown) {
      continue;
    }
    const Hit hit{Target::Place, i};
    const bool on = state.focus == hit || (state.panel == Panel::Widget && state.editPlace == i);
    place(p.outline, p.rect, outline(on, state.hover == hit, s));
    if (i >= placed) {
      p.plus->setText("+");
      p.plus->setFontSize(Tokens::typeTitle3.size * s);
      p.plus->setColor(kLabel2);
      p.plus->measure(renderer);
      p.plus->setPosition(std::round(p.rect.x + (p.rect.w - p.plus->width()) * 0.5F),
                          std::round(p.rect.y + (p.rect.h - p.plus->height()) * 0.5F));
    }
  }

  const bool bottom = state.active && state.bottom && bottomRect.w > 0.0F;
  m_bottom->setVisible(bottom);
  m_bottomRect = bottom ? bottomRect : Rect{};
  if (bottom) {
    const Hit hit{Target::BottomSlot, -1};
    place(m_bottom, m_bottomRect, outline(state.focus == hit || state.panel == Panel::Bottom, state.hover == hit, s));
  }

  const bool widgetPanel = state.active && state.panel == Panel::Widget;
  const bool bottomPanel = state.active && state.panel == Panel::Bottom;
  const bool canRemove = widgetPanel && state.editPlace >= 0 && state.editPlace < placed;
  m_panelBg->setVisible(widgetPanel || bottomPanel);
  m_title->setVisible(widgetPanel || bottomPanel);
  for (auto& chip : m_kinds) {
    chip.bg->setVisible(widgetPanel);
    chip.label->setVisible(widgetPanel);
    chip.rect = {};
  }
  m_remove.bg->setVisible(canRemove);
  m_remove.label->setVisible(canRemove);
  m_remove.rect = {};
  for (auto& chip : m_bottomKinds) {
    chip.bg->setVisible(bottomPanel);
    chip.label->setVisible(bottomPanel);
    chip.rect = {};
  }
  if (!widgetPanel && !bottomPanel) {
    m_panelRect = {};
    return;
  }

  // One row: title left, chips right (as the clock gallery's header row), thick glass 24 above the bottom.
  struct Item {
    LockEditorGallery::Chip* chip;
    bool on;
    Hit hit;
  };
  std::vector<Item> items;
  if (widgetPanel) {
    const std::string current = canRemove ? state.widgets[static_cast<std::size_t>(state.editPlace)] : std::string{};
    for (int i = 0; i < kKinds; ++i) {
      auto& chip = m_kinds[static_cast<std::size_t>(i)];
      chip.label->setText(kindLabel(LockCards::kWidgetKinds[static_cast<std::size_t>(i)]));
      items.push_back({&chip, current == LockCards::kWidgetKinds[static_cast<std::size_t>(i)], {Target::Kind, i}});
    }
    if (canRemove) {
      m_remove.label->setText(i18n::tr("lockscreen.editor.remove"));
      items.push_back({&m_remove, false, {Target::Remove, -1}});
    }
    m_title->setText(i18n::tr("lockscreen.editor.widgets"));
  } else {
    for (std::size_t i = 0; i < kBottomNames.size(); ++i) {
      auto& chip = m_bottomKinds[i];
      chip.label->setText(i18n::tr(std::string("lockscreen.editor.bottom.") + kBottomNames[i]));
      items.push_back({&chip, static_cast<int>(i) == state.bottomSlot, {Target::BottomKind, static_cast<int>(i)}});
    }
    m_title->setText(i18n::tr("lockscreen.editor.bottom-slot"));
  }
  m_title->setFontSize(Tokens::typeTitle3.size * s);
  m_title->setColor(kLabel1);
  m_title->measure(renderer);

  const float pad = kPad * s;
  const float gap = 6.0F * s;
  float chipsW = 0.0F;
  for (const Item& item : items) {
    LockEditorGallery::layoutChip(renderer, *item.chip, 0.0F, 0.0F, s, item.on, false, false);
    chipsW += item.chip->rect.w + gap;
  }
  chipsW -= gap;
  const float w = std::min(sw - 32.0F * s, std::max(kMinPanelWidth * s, pad + m_title->width() + 24.0F * s + chipsW + pad));
  const float h = pad + kChipHeight * s + pad;
  const float x = std::round((sw - w) * 0.5F);
  const float y = std::round(sh - 24.0F * s - h);
  m_panelRect = {x, y, w, h};
  place(m_panelBg, m_panelRect, RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 28.0F * s, .borderWidth = 0.5F * s});
  m_title->setPosition(x + pad, std::round(y + pad + (kChipHeight * s - m_title->height()) * 0.5F));
  float cx = x + w - pad - chipsW;
  for (const Item& item : items) {
    LockEditorGallery::layoutChip(
        renderer, *item.chip, std::round(cx), y + pad, s, item.on, state.focus == item.hit, state.hover == item.hit
    );
    cx += item.chip->rect.w + gap;
  }
}

LockEditorSlots::Hit LockEditorSlots::hitTest(float x, float y) const {
  if (!m_state.active) {
    return {};
  }
  if (m_panelRect.contains(x, y)) {
    for (int i = 0; i < kKinds; ++i) {
      if (m_kinds[static_cast<std::size_t>(i)].rect.contains(x, y)) {
        return {Target::Kind, i};
      }
    }
    if (m_remove.rect.contains(x, y)) {
      return {Target::Remove, -1};
    }
    for (std::size_t i = 0; i < m_bottomKinds.size(); ++i) {
      if (m_bottomKinds[i].rect.contains(x, y)) {
        return {Target::BottomKind, static_cast<int>(i)};
      }
    }
    return {Target::Panel, -1};
  }
  for (int i = 0; i < kPlaces; ++i) {
    if (m_places[static_cast<std::size_t>(i)].rect.contains(x, y)) {
      return {Target::Place, i};
    }
  }
  if (m_bottomRect.contains(x, y)) {
    return {Target::BottomSlot, -1};
  }
  return {};
}

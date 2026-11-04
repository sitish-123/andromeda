#include "shell/lockscreen/lock_editor_free.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "shell/desktop/desktop_widget.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  // Lock materials (rulebook 9.15): widgets on regular glass, editor chrome on thick; editor rings in accent text.
  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kRegular = rgba(28.0F / 255.0F, 28.0F / 255.0F, 32.0F / 255.0F, 0.40F);
  constexpr Color kThick = rgba(32.0F / 255.0F, 32.0F / 255.0F, 36.0F / 255.0F, 0.92F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kAccentText = rgba(140.0F / 255.0F, 133.0F / 255.0F, 1.0F, 1.0F);
  // Mock section 2: zone bands are edges only; Top and Middle dashed, Auth solid.
  constexpr Color kZoneEdge = rgba(1.0F, 1.0F, 1.0F, 0.30F);
  constexpr Color kNoFill = rgba(0.0F, 0.0F, 0.0F, 0.0F);
  constexpr float kPad = 16.0F;
  constexpr float kChipHeight = 28.0F;
  constexpr float kHandle = 12.0F;

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

  void place(RectNode* node, const AuthRect& r, const RoundedRectStyle& style) {
    node->setVisible(true);
    node->setPosition(r.x, r.y);
    node->setSize(r.w, r.h);
    node->setStyle(style);
  }

  bool inside(const AuthRect& r, float x, float y) { return x >= r.x && x < r.right() && y >= r.y && y < r.bottom(); }

  std::string typeLabel(const std::string& type) {
    std::string key = type;
    std::ranges::replace(key, '_', '-');
    return i18n::tr("desktop-widgets.editor.types." + key);
  }

} // namespace

LockEditorFree::LockEditorFree() = default;

LockEditorFree::~LockEditorFree() { clear(); }

void LockEditorFree::setFactory(
    const DesktopWidgetFactory* factory, AnimationManager* animations, std::function<void()> refresh
) {
  m_factory = factory;
  m_animations = animations;
  m_refresh = std::move(refresh);
}

void LockEditorFree::clear() {
  for (Live& live : m_live) {
    if (live.widget != nullptr) {
      live.widget->setUpdateCallback(nullptr);
      live.widget->setLayoutCallback(nullptr);
      live.widget->setRedrawCallback(nullptr);
    }
  }
  m_live.clear();
}

LockEditorFree::Live* LockEditorFree::liveFor(const Item& item, float scale) {
  const auto it = std::ranges::find_if(m_live, [&](const Live& live) { return live.id == item.id && live.type == item.type; });
  if (it != m_live.end()) {
    return it->widget != nullptr ? &*it : nullptr;
  }
  Live live{.id = item.id, .type = item.type};
  if (m_factory != nullptr) {
    live.widget = m_factory->create(item.type, item.settings, desktop_widgets::widgetContentScale(scale));
  }
  if (live.widget != nullptr) {
    live.widget->create();
    live.widget->setAnimationManager(m_animations);
    const auto refresh = [this] {
      if (m_refresh) {
        m_refresh();
      }
    };
    live.widget->setUpdateCallback(refresh);
    live.widget->setLayoutCallback(refresh);
    live.widget->setRedrawCallback(refresh);
    auto node = std::make_unique<Node>();
    node->setZIndex(1);
    live.node = m_group->addChild(std::move(node));
    live.node->addChild(live.widget->releaseRoot());
  }
  m_live.push_back(std::move(live));
  return m_live.back().widget != nullptr ? &m_live.back() : nullptr;
}

std::vector<LockEditorFree::Hit> LockEditorFree::focusOrder(const State& state) {
  std::vector<Hit> order;
  if (!state.active) {
    return order;
  }
  for (int i = 0; i < static_cast<int>(state.items.size()); ++i) {
    order.push_back({Target::Item, i});
  }
  for (int i = 0; i < static_cast<int>(kTypes.size()); ++i) {
    order.push_back({Target::Add, i});
  }
  return order;
}

void LockEditorFree::build(Node& root) {
  m_group = root.addChild(std::make_unique<Node>());
  m_group->setZIndex(9);
  // The editor builds a new scene on each open; nodes of the last scene are freed.
  m_boxes.clear();
  clear();
  m_drag = {};
  for (std::size_t i = 0; i < m_zones.size(); ++i) {
    m_zones[i] = addRect(*m_group);
    m_zoneLabels[i] = addLabel(*m_group, FontWeight::Medium);
  }
  m_toolbarBg = addRect(*m_group);
  m_toolbarBg->setZIndex(3);
  const auto addChip = [this](LockEditorGallery::Chip& chip) {
    chip.bg = addRect(*m_group);
    chip.label = addLabel(*m_group, FontWeight::Medium);
    chip.bg->setZIndex(3);
    chip.label->setZIndex(3);
  };
  std::ranges::for_each(m_add, addChip);
}

void LockEditorFree::layout(
    Renderer& renderer, const State& state, const lock_free::Zones& zones, float sw, float sh, float scale
) {
  m_state = state;
  const float s = std::max(0.1F, scale);
  m_scale = s;
  m_group->setVisible(state.active);
  if (!state.active) {
    for (Box& box : m_boxes) {
      box.rect = {};
    }
    m_toolbarRect = {};
    return;
  }

  // Zone bands with "Snap to zones"; the auth area always shows, as it always rejects drops.
  const std::array<AuthRect, 3> bands{zones.top, zones.middle, zones.auth};
  constexpr std::array<const char*, 3> kZoneKeys{
      "lockscreen.editor.zone-top", "lockscreen.editor.zone-middle", "lockscreen.editor.zone-auth"
  };
  for (std::size_t i = 0; i < bands.size(); ++i) {
    const bool auth = i == 2;
    const bool shown = (auth || state.snapZones) && !bands[i].empty();
    m_zones[i]->setVisible(shown);
    m_zoneLabels[i]->setVisible(shown);
    if (!shown) {
      continue;
    }
    place(m_zones[i], bands[i],
          RoundedRectStyle{
              .fill = kNoFill,
              .border = kZoneEdge,
              .radius = 16.0F * s,
              .borderWidth = 1.0F * s,
              .borderDash = auth ? 0.0F : 6.0F * s,
              .borderGap = 4.0F * s,
          });
    m_zoneLabels[i]->setText(i18n::tr(kZoneKeys[i]));
    m_zoneLabels[i]->setFontSize(Tokens::typeFootnote.size * s);
    m_zoneLabels[i]->setColor(kLabel2);
    m_zoneLabels[i]->measure(renderer);
    m_zoneLabels[i]->setPosition(std::round(bands[i].x + 12.0F * s), std::round(bands[i].y + 8.0F * s));
  }

  // Layers: box glass (0), live widget (1), ring, name and handle (2), toolbar (3).
  while (m_boxes.size() < state.items.size()) {
    Box box;
    box.bg = addRect(*m_group);
    box.ring = addRect(*m_group);
    box.name = addLabel(*m_group, FontWeight::Medium);
    box.handle = addRect(*m_group);
    box.ring->setZIndex(2);
    box.name->setZIndex(2);
    box.handle->setZIndex(2);
    m_boxes.push_back(box);
  }
  // Drop the live widgets of removed items.
  std::erase_if(m_live, [&](Live& live) {
    const bool kept = std::ranges::any_of(state.items, [&](const Item& item) {
      return item.id == live.id && item.type == live.type;
    });
    if (!kept && live.node != nullptr) {
      live.widget->setUpdateCallback(nullptr);
      live.widget->setLayoutCallback(nullptr);
      live.widget->setRedrawCallback(nullptr);
      (void)m_group->removeChild(live.node);
    }
    return !kept;
  });
  for (std::size_t i = 0; i < m_boxes.size(); ++i) {
    Box& box = m_boxes[i];
    const bool shown = i < state.items.size();
    box.bg->setVisible(shown);
    box.ring->setVisible(shown);
    box.name->setVisible(shown);
    box.handle->setVisible(shown && static_cast<int>(i) == state.selected);
    if (!shown) {
      box.rect = {};
      continue;
    }
    const Item& item = state.items[i];
    box.rect = item.box;
    const Hit hit{Target::Item, static_cast<int>(i)};
    const bool ring = static_cast<int>(i) == state.selected || state.focus == hit;
    place(box.bg, box.rect, RoundedRectStyle{.fill = kRegular, .radius = 16.0F * s});
    place(box.ring, box.rect,
          RoundedRectStyle{
              .fill = kNoFill,
              .border = ring ? kAccentText : kStroke,
              .radius = 16.0F * s,
              .borderWidth = (ring ? 2.0F : 0.5F) * s,
          });
    if (static_cast<int>(i) == state.selected) {
      // Mock section 2: the resize handle is a white dot with an accent ring, centred on the bottom right corner.
      const float d = kHandle * s;
      place(box.handle, {box.rect.right() - d * 0.5F, box.rect.bottom() - d * 0.5F, d, d},
            RoundedRectStyle{.fill = kLabel1, .border = kAccentText, .radius = d * 0.5F, .borderWidth = 1.5F * s});
    }
    if (Live* live = liveFor(item, s); live != nullptr) {
      // The widget fits its box as on the lock (setBox), centred in it.
      live->widget->setContentScale(desktop_widgets::widgetContentScale(s));
      live->widget->setBox(box.rect.w, box.rect.h);
      live->widget->update(renderer);
      live->widget->layout(renderer);
      const float iw = std::max(1.0F, live->widget->intrinsicWidth());
      const float ih = std::max(1.0F, live->widget->intrinsicHeight());
      live->node->setFrameSize(iw, ih);
      live->node->setPosition(
          std::round(box.rect.x + (box.rect.w - iw) * 0.5F), std::round(box.rect.y + (box.rect.h - ih) * 0.5F)
      );
      box.name->setVisible(false);
      continue;
    }
    box.name->setText(typeLabel(item.type));
    box.name->setFontSize(Tokens::typeFootnote.size * s);
    box.name->setColor(kLabel1);
    box.name->setMaxWidth(std::max(0.0F, box.rect.w - 16.0F * s));
    box.name->measure(renderer);
    box.name->setPosition(std::round(box.rect.x + (box.rect.w - box.name->width()) * 0.5F),
                          std::round(box.rect.y + (box.rect.h - box.name->height()) * 0.5F));
  }

  // Toolbar: the add chips; thick glass 24 above the bottom.
  float chipsW = 0.0F;
  const float gap = 6.0F * s;
  for (std::size_t i = 0; i < m_add.size(); ++i) {
    m_add[i].label->setText("+ " + typeLabel(kTypes[i]));
    LockEditorGallery::layoutChip(renderer, m_add[i], 0.0F, 0.0F, s, false, false, false);
    chipsW += m_add[i].rect.w + gap;
  }
  chipsW -= gap;
  const float pad = kPad * s;
  const float w = std::min(sw - 32.0F * s, chipsW + pad * 2.0F);
  const float h = pad * 2.0F + kChipHeight * s;
  const float x = std::round((sw - w) * 0.5F);
  const float y = std::round(sh - 24.0F * s - h);
  m_toolbarRect = {x, y, w, h};
  place(m_toolbarBg, {x, y, w, h}, RoundedRectStyle{.fill = kThick, .border = kStroke, .radius = 28.0F * s, .borderWidth = 0.5F * s});
  float cx = x + pad;
  for (std::size_t i = 0; i < m_add.size(); ++i) {
    const Hit hit{Target::Add, static_cast<int>(i)};
    LockEditorGallery::layoutChip(renderer, m_add[i], std::round(cx), y + pad, s, false, state.focus == hit, state.hover == hit);
    cx += m_add[i].rect.w + gap;
  }
}

LockEditorFree::Hit LockEditorFree::hitTest(float x, float y) const {
  if (!m_state.active) {
    return {};
  }
  if (m_toolbarRect.contains(x, y)) {
    for (std::size_t i = 0; i < m_add.size(); ++i) {
      if (m_add[i].rect.contains(x, y)) {
        return {Target::Add, static_cast<int>(i)};
      }
    }
    return {Target::Toolbar, -1};
  }
  // Top-most box first: later items draw over earlier ones.
  for (int i = static_cast<int>(m_state.items.size()) - 1; i >= 0; --i) {
    const AuthRect& r = m_boxes[static_cast<std::size_t>(i)].rect;
    if (!inside(r, x, y)) {
      continue;
    }
    const float s = m_scale;
    const bool corner = i == m_state.selected && x >= r.right() - (kHandle + 8.0F) * s && y >= r.bottom() - (kHandle + 8.0F) * s;
    return {corner ? Target::Resize : Target::Item, i};
  }
  return {};
}

void LockEditorFree::begin(const State& state, int index, bool resize, float x, float y) {
  if (index < 0 || index >= static_cast<int>(state.items.size())) {
    return;
  }
  m_drag.index = index;
  m_drag.resize = resize;
  m_drag.startX = x;
  m_drag.startY = y;
  m_drag.start = state.items[static_cast<std::size_t>(index)].box;
}

bool LockEditorFree::moved(float x, float y) const noexcept {
  return std::abs(x - m_drag.startX) > 2.0F || std::abs(y - m_drag.startY) > 2.0F;
}

AuthRect LockEditorFree::drag(float x, float y, float scale) const {
  AuthRect box = m_drag.start;
  const float dx = x - m_drag.startX;
  const float dy = y - m_drag.startY;
  if (m_drag.resize) {
    const float min = lock_free::kMinSize * std::max(0.1F, scale);
    box.w = std::max(min, box.w + dx);
    box.h = std::max(min, box.h + dy);
  } else {
    box.x += dx;
    box.y += dy;
  }
  return box;
}

std::optional<AuthRect> LockEditorFree::end(
    float x, float y, const State& state, const lock_free::Zones& zones, float sw, float sh, float scale
) {
  if (!dragging()) {
    return std::nullopt;
  }
  const AuthRect box = drag(x, y, scale);
  const lock_free::Zone zone = lock_free::zoneOf(m_drag.start, zones);
  m_drag = {};
  return lock_free::place(box, zones, state.snapZones, zone, true, sw, sh, scale);
}

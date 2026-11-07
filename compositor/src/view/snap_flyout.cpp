#include "view/snap_flyout.h"

#include "config/config.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>

#include <xkbcommon/xkbcommon-keysyms.h>


namespace andromeda {
  namespace {
    // Rulebook 4 grid; prototype SNAP_LAYOUTS: 3 x 2 minis of a 16:10 screen.
    constexpr int kPad = 12;
    constexpr int kMiniW = 72;
    constexpr int kMiniH = 44;
    constexpr int kGap = 8;
    constexpr int kZoneGap = 4;
    constexpr int kColumns = 3;
    constexpr int kSheetRadius = 16;
    constexpr int kZoneRadius = 4;
    constexpr int kSheetW = 2 * kPad + kColumns * kMiniW + (kColumns - 1) * kGap;
    constexpr int kSheetH = 2 * kPad + 2 * kMiniH + kGap;

    std::array<float, 4> premultiplied(const std::array<float, 4>& color, float alpha) {
      const float a = color[3] * alpha;
      return {color[0] * a, color[1] * a, color[2] * a, a};
    }

    bool holds(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    wlr_scene_rect* roundRect(wlr_scene_tree* parent, int radius) {
      wlr_scene_rect* rect = wlr_scene_rect_create(parent, 1, 1, std::array<float, 4>{}.data());
      if (rect != nullptr) {
        wlr_scene_rect_set_corner_radius(rect, radius);
      }
      return rect;
    }
  } // namespace

  SnapFlyout::SnapFlyout(wlr_scene_tree* parent) : m_tree(wlr_scene_tree_create(parent)) {
    if (m_tree == nullptr) {
      return;
    }
    wlr_scene_node_set_enabled(&m_tree->node, false);
    m_sheet = roundRect(m_tree, kSheetRadius);
    for (size_t layout = 0; layout < kSnapLayouts; ++layout) {
      for (size_t zone = 0; zone < snapLayoutZones(layout); ++zone) {
        m_zoneRects.push_back(roundRect(m_tree, kZoneRadius));
      }
    }
  }

  // The scene owns the tree and frees it with the rest of the chrome at teardown.
  SnapFlyout::~SnapFlyout() = default;

  void SnapFlyout::open(View& view, int x, int y, const wlr_box& bounds, bool keyboard) {
    if (m_tree == nullptr) {
      return;
    }
    m_view = Ref<View>(&view);
    const int left = std::clamp(x - kSheetW / 2, bounds.x, std::max(bounds.x, bounds.x + bounds.width - kSheetW));
    const int top = std::clamp(y, bounds.y, std::max(bounds.y, bounds.y + bounds.height - kSheetH));
    m_box = {left, top, kSheetW, kSheetH};
    for (size_t layout = 0; layout < kSnapLayouts; ++layout) {
      const int column = static_cast<int>(layout) % kColumns;
      const int row = static_cast<int>(layout) / kColumns;
      Mini& mini = m_minis[layout];
      mini.box = {left + kPad + column * (kMiniW + kGap), top + kPad + row * (kMiniH + kGap), kMiniW, kMiniH};
      mini.zones.clear();
      for (size_t zone = 0; zone < snapLayoutZones(layout); ++zone) {
        const SnapZone id{.layout = static_cast<uint8_t>(layout), .zone = static_cast<uint8_t>(zone)};
        mini.zones.emplace_back(id, snapZoneBox(mini.box, id, kZoneGap));
      }
    }
    m_open = true;
    m_lit = keyboard ? std::optional<SnapZone>(SnapZone{}) : std::nullopt;
    wlr_scene_node_raise_to_top(&m_tree->node);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    draw();
  }

  void SnapFlyout::close() {
    m_open = false;
    m_lit.reset();
    m_view = Ref<View>();
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
  }

  void SnapFlyout::draw() {
    const auto& colors = config().colors;
    if (m_sheet != nullptr) {
      wlr_scene_node_set_position(&m_sheet->node, m_box.x, m_box.y);
      wlr_scene_rect_set_size(m_sheet, m_box.width, m_box.height);
      wlr_scene_rect_set_color(m_sheet, premultiplied(colors.background, 0.92F).data());
    }
    size_t index = 0;
    for (const Mini& mini : m_minis) {
      for (const auto& [zone, box] : mini.zones) {
        wlr_scene_rect* rect = index < m_zoneRects.size() ? m_zoneRects[index] : nullptr;
        ++index;
        if (rect == nullptr) {
          continue;
        }
        wlr_scene_node_set_position(&rect->node, box.x, box.y);
        wlr_scene_rect_set_size(rect, box.width, box.height);
        const bool lit = m_lit == zone;
        const auto color = lit ? premultiplied(colors.accentPrimary, 1.0F) : premultiplied(colors.textPrimary, 0.24F);
        wlr_scene_rect_set_color(rect, color.data());
      }
    }
  }

  void SnapFlyout::setLit(std::optional<SnapZone> zone) {
    if (zone == m_lit) {
      return;
    }
    m_lit = zone;
    draw();
  }

  bool SnapFlyout::contains(double lx, double ly) const { return active() && holds(m_box, lx, ly); }

  std::optional<SnapZone> SnapFlyout::zoneAt(double lx, double ly) const {
    if (!contains(lx, ly)) {
      return std::nullopt;
    }
    for (const Mini& mini : m_minis) {
      for (const auto& [zone, box] : mini.zones) {
        if (holds(box, lx, ly)) {
          return zone;
        }
      }
    }
    return std::nullopt;
  }

  std::optional<SnapZone> SnapFlyout::hover(double lx, double ly) {
    if (!active()) {
      return std::nullopt;
    }
    const std::optional<SnapZone> zone = zoneAt(lx, ly);
    setLit(zone);
    return zone;
  }

  void SnapFlyout::snap(SnapZone zone) {
    View* view = m_view.get();
    close();
    if (view != nullptr && view->snapTo(SnapSpot{zone}) && view->snapSpot() == std::optional<SnapSpot>(zone)
        && m_onSnapped) {
      m_onSnapped(*view, zone);
    }
  }

  bool SnapFlyout::press(double lx, double ly) {
    if (!active()) {
      return false;
    }
    if (const std::optional<SnapZone> zone = zoneAt(lx, ly)) {
      snap(*zone);
      return true;
    }
    if (contains(lx, ly)) {
      return true;
    }
    close();
    return false;
  }

  bool SnapFlyout::key(uint32_t keysym) {
    if (!active()) {
      return false;
    }
    // Every zone in reading order: Left/Right step through them, Up/Down jump a row of layouts.
    std::vector<SnapZone> order;
    for (const Mini& mini : m_minis) {
      for (const auto& [zone, box] : mini.zones) {
        order.push_back(zone);
      }
    }
    const auto at = std::ranges::find(order, m_lit.value_or(SnapZone{}));
    const auto index = static_cast<long>(at == order.end() ? 0 : at - order.begin());
    const auto count = static_cast<long>(order.size());
    const auto step = [&](long by) { setLit(order[static_cast<size_t>(((index + by) % count + count) % count)]); };
    const auto row = [&](int by) {
      const SnapZone current = m_lit.value_or(SnapZone{});
      const int layout = (static_cast<int>(current.layout) + by * kColumns + static_cast<int>(kSnapLayouts))
          % static_cast<int>(kSnapLayouts);
      const auto zones = static_cast<uint8_t>(snapLayoutZones(static_cast<size_t>(layout)));
      setLit(SnapZone{.layout = static_cast<uint8_t>(layout), .zone = std::min<uint8_t>(current.zone, zones - 1)});
    };
    switch (keysym) {
    case XKB_KEY_Left:
      step(-1);
      return true;
    case XKB_KEY_Right:
    case XKB_KEY_Tab:
      step(1);
      return true;
    case XKB_KEY_Up:
      row(-1);
      return true;
    case XKB_KEY_Down:
      row(1);
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
      if (m_lit.has_value()) {
        snap(*m_lit);
      }
      return true;
    case XKB_KEY_Escape:
      close();
      return true;
    default:
      close();
      return false;
    }
  }
} // namespace andromeda

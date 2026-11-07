#include "view/snap_flyout.h"

#include "config/config.h"
#include "design_tokens.h"
#include "scene/glass_panel.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>

#include <xkbcommon/xkbcommon-keysyms.h>


namespace andromeda {
  namespace {
    // Board desktop-layouts.html section 3 (radius set B): popover r 12, p 8, holding prototype SNAP_LAYOUTS as 3 x 2
    // minis of a 16:10 screen, 64 x 40, zones r 4 and 4 apart, layouts 8 apart.
    constexpr int kPad = 8;
    constexpr int kMiniW = 64;
    constexpr int kMiniH = 40;
    constexpr int kGap = 8;
    constexpr int kZoneGap = 4;
    constexpr int kColumns = 3;
    constexpr int kSheetRadius = 12;
    constexpr int kZoneRadius = 4;
    constexpr int kSheetW = 2 * kPad + kColumns * kMiniW + (kColumns - 1) * kGap;
    constexpr int kSheetH = 2 * kPad + 2 * kMiniH + kGap;

    std::array<float, 4> token(std::uint32_t value) {
      const auto channel = [value](int shift) { return static_cast<float>((value >> shift) & 0xFFU) / 255.0F; };
      const float a = channel(0);
      return {channel(24) * a, channel(16) * a, channel(8) * a, a};
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

  SnapFlyout::SnapFlyout(Server& server, wlr_scene_tree* parent)
      : m_server(server), m_tree(wlr_scene_tree_create(parent)) {
    m_server.registerAnimatable(this);
    if (m_tree == nullptr) {
      return;
    }
    wlr_scene_node_set_enabled(&m_tree->node, false);
    m_panel = std::make_unique<GlassPanel>(m_tree, glass::Kind::Menu);
    for (size_t layout = 0; layout < kSnapLayouts; ++layout) {
      for (size_t zone = 0; zone < snapLayoutZones(layout); ++zone) {
        m_zoneRects.push_back(roundRect(m_tree, kZoneRadius));
      }
    }
    m_zoneFill.resize(m_zoneRects.size());
  }

  // The scene owns the tree and frees it with the rest of the chrome at teardown.
  SnapFlyout::~SnapFlyout() { m_server.unregisterAnimatable(this); }

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
    m_output = wlr_output_layout_output_at(m_server.outputLayout(), m_box.x + m_box.width / 2.0, m_box.y);
    m_open = true;
    m_lit = keyboard ? std::optional<SnapZone>(SnapZone{}) : std::nullopt;
    // P10 Grow: scale 0.92 -> 1 on open with opacity on fade; reduced motion keeps only the fade (7.8).
    const auto& anim = config().animation;
    const bool reduced = config().appearance.reduceMotion;
    m_scale.snap(anim.enabled && !reduced ? 0.92 : 1.0);
    m_opacity.snap(anim.enabled ? 0.0 : 1.0);
    if (anim.enabled) {
      m_scale.settleSpring(1.0, {.damping = Tokens::springOpen.damping, .stiffness = Tokens::springOpen.stiffness});
      m_opacity.settleSpring(1.0, {.damping = Tokens::springFade.damping, .stiffness = Tokens::springFade.stiffness});
    }
    retargetZones(false);
    wlr_scene_node_raise_to_top(&m_tree->node);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    draw();
    scheduleFrame();
  }

  void SnapFlyout::close(bool animate) {
    const bool wasOpen = m_open;
    m_open = false;
    m_lit.reset();
    m_view = Ref<View>();
    if (m_tree == nullptr) {
      return;
    }
    // P10 Return: opacity on fade_out, scale 1 -> 0.96 toward the anchor on close; the tree hides once it is clear.
    const auto& anim = config().animation;
    if (!wasOpen || !anim.enabled || !animate) {
      m_opacity.snap(0.0);
      m_scale.snap(1.0);
      wlr_scene_node_set_enabled(&m_tree->node, false);
      return;
    }
    if (!config().appearance.reduceMotion) {
      m_scale.settleSpring(0.96, {.damping = Tokens::springClose.damping, .stiffness = Tokens::springClose.stiffness});
    }
    m_opacity.settleSpring(
        0.0, {.damping = Tokens::springFadeOut.damping, .stiffness = Tokens::springFadeOut.stiffness}
    );
    scheduleFrame();
  }

  void SnapFlyout::scheduleFrame() {
    // Every output: the sheet's own output can be unknown or gone, and a fade must still finish.
    for (const auto& output : m_server.outputs()) {
      wlr_output_schedule_frame(output->wlr());
    }
  }

  void SnapFlyout::retargetZones(bool animate) {
    const bool light = config().appearance.scheme == ColorScheme::Light;
    const auto& scheme = light ? Tokens::schemeLight : Tokens::schemeDark;
    const bool springs = animate && config().animation.enabled;
    size_t index = 0;
    for (const Mini& mini : m_minis) {
      for (const auto& entry : mini.zones) {
        const SnapZone zone = entry.first;
        if (index >= m_zoneFill.size()) {
          return;
        }
        // The lit zone in accent, its layout's other zones fill_3, every other zone fill_2.
        const bool lit = m_lit == zone;
        const bool sameLayout = m_lit.has_value() && m_lit->layout == zone.layout;
        const auto color = lit ? token(scheme.accent) : sameLayout ? token(scheme.fill3) : token(scheme.fill2);
        auto& fill = m_zoneFill[index++];
        // P3: a fill rises on fade_out (quick) and falls back on fade (slower), so a quick pass does not flicker.
        const bool rising = lit || sameLayout;
        const auto& spring = rising ? Tokens::springFadeOut : Tokens::springFade;
        for (size_t channel = 0; channel < fill.size(); ++channel) {
          if (springs) {
            fill[channel].settleSpring(color[channel], {.damping = spring.damping, .stiffness = spring.stiffness});
          } else {
            fill[channel].snap(color[channel]);
          }
        }
      }
    }
  }

  bool SnapFlyout::tickAnimations(double nowMsec) {
    bool running = m_scale.tick(nowMsec);
    running = m_opacity.tick(nowMsec) || running;
    for (auto& fill : m_zoneFill) {
      for (auto& channel : fill) {
        running = channel.tick(nowMsec) || running;
      }
    }
    if (m_tree != nullptr && !m_open && !m_opacity.animating()) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    } else {
      draw();
    }
    return running;
  }

  bool SnapFlyout::hasActiveAnimations() const {
    return m_scale.animating() || m_opacity.animating()
        || std::ranges::any_of(m_zoneFill, [](const auto& fill) {
             return std::ranges::any_of(fill, [](const AnimatedValue& channel) { return channel.animating(); });
           });
  }

  bool SnapFlyout::animatesOn(const Output* output) const { return output != nullptr && hasActiveAnimations(); }

  void SnapFlyout::draw() {
    // Scaled about the anchor, the sheet's top centre under the button that opened it.
    const double scale = m_scale.current();
    const auto opacity = static_cast<float>(std::clamp(m_opacity.current(), 0.0, 1.0));
    const double anchorX = m_box.x + m_box.width / 2.0;
    const double anchorY = m_box.y;
    const auto grown = [&](const wlr_box& box) {
      const double x = anchorX + (box.x - anchorX) * scale;
      const double y = anchorY + (box.y - anchorY) * scale;
      return wlr_box{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                     std::max(1, static_cast<int>(std::lround(box.width * scale))),
                     std::max(1, static_cast<int>(std::lround(box.height * scale)))};
    };
    if (m_panel != nullptr) {
      m_panel->place(grown(m_box), kSheetRadius, opacity);
    }
    size_t index = 0;
    for (const Mini& mini : m_minis) {
      for (const auto& [zone, box] : mini.zones) {
        wlr_scene_rect* rect = index < m_zoneRects.size() ? m_zoneRects[index] : nullptr;
        ++index;
        if (rect == nullptr) {
          continue;
        }
        const wlr_box placed = grown(box);
        wlr_scene_node_set_position(&rect->node, placed.x, placed.y);
        wlr_scene_rect_set_size(rect, placed.width, placed.height);
        wlr_scene_rect_set_corner_radius(rect, kZoneRadius);
        const auto& fill = m_zoneFill[index - 1];
        std::array<float, 4> color{};
        for (size_t channel = 0; channel < color.size(); ++channel) {
          color[channel] = static_cast<float>(fill[channel].current()) * opacity;
        }
        wlr_scene_rect_set_color(rect, color.data());
      }
    }
  }

  void SnapFlyout::setLit(std::optional<SnapZone> zone) {
    if (zone == m_lit) {
      return;
    }
    m_lit = zone;
    retargetZones(true);
    draw();
    scheduleFrame();
  }

  bool SnapFlyout::contains(double lx, double ly) const { return active() && holds(m_box, lx, ly); }

  bool SnapFlyout::reaches(double lx, double ly) const {
    constexpr int kBridge = 12;
    return active() && holds({m_box.x, m_box.y - kBridge, m_box.width, m_box.height + kBridge}, lx, ly);
  }

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

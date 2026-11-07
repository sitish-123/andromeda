#pragma once

#include "core/animation.h"
#include "util/tracked.h"
#include "view/snap.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct wlr_output;
struct wlr_scene_rect;
struct wlr_scene_tree;

namespace andromeda {
  class GlassPanel;
  class Output;
  class Server;
  class View;

  // Snap Layouts flyout (Windows 11; DESIGN.md "Free mode Snap Layouts"): a thick sheet with the six layouts drawn
  // small; a zone under the pointer lights up and a click snaps the window there. Opened by Super+Z, by a window drag
  // reaching the top edge, and by dwelling on a title bar's maximize button. It Grows from its anchor and Returns into
  // it (rulebook P10); zone fills cross on the hover springs (P3).
  class SnapFlyout : public Animatable {
  public:
    SnapFlyout(Server& server, wlr_scene_tree* parent);
    ~SnapFlyout();
    SnapFlyout(const SnapFlyout&) = delete;
    SnapFlyout& operator=(const SnapFlyout&) = delete;

    // Opens for `view` with the sheet's top centre at (x, y) in layout coordinates, kept inside `bounds`. `keyboard`
    // marks the first zone, so arrows and Enter work at once.
    void open(View& view, int x, int y, const wlr_box& bounds, bool keyboard);
    // `animate` false hides at once (interruptions: lock, output removal, workspace switch).
    void close(bool animate = true);
    [[nodiscard]] bool active() const { return m_tree != nullptr && m_open; }
    [[nodiscard]] View* view() const { return m_view.get(); }

    [[nodiscard]] std::optional<SnapZone> zoneAt(double lx, double ly) const;
    [[nodiscard]] bool contains(double lx, double ly) const;
    // The sheet plus the 8 px bridge above it to the button that opened it.
    [[nodiscard]] bool reaches(double lx, double ly) const;
    // Lights the zone under the point (none outside a zone); returns it.
    std::optional<SnapZone> hover(double lx, double ly);
    // A press snaps to the zone under it and closes; inside the sheet it is kept, outside it closes and passes on.
    // Returns whether the press is consumed.
    bool press(double lx, double ly);
    // Arrows walk the zones, Enter snaps, Escape closes; every other key closes and passes on. Returns consumed.
    bool key(uint32_t keysym);
    // Snaps the open view to `zone` and closes.
    void snap(SnapZone zone);
    // Runs after a zone pick snapped the window (snap assist starts there).
    void setOnSnapped(std::function<void(View&, SnapZone)> onSnapped) { m_onSnapped = std::move(onSnapped); }

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override;

  private:
    struct Mini {
      wlr_box box{};
      std::vector<std::pair<SnapZone, wlr_box>> zones;
    };
    void draw();
    void setLit(std::optional<SnapZone> zone);
    // Starts each zone's fill toward its colour for the lit zone; `animate` false jumps.
    void retargetZones(bool animate);
    void scheduleFrame();

    Server& m_server;
    wlr_scene_tree* m_tree = nullptr;
    wlr_output* m_output = nullptr;
    AnimatedValue m_scale{1.0};
    AnimatedValue m_opacity{0.0};
    // One premultiplied RGBA per zone rect, in m_zoneRects order.
    std::vector<std::array<AnimatedValue, 4>> m_zoneFill;
    std::unique_ptr<GlassPanel> m_panel;
    std::vector<wlr_scene_rect*> m_zoneRects; // in Mini/zone order
    std::array<Mini, kSnapLayouts> m_minis{};
    wlr_box m_box{};
    Ref<View> m_view;
    std::optional<SnapZone> m_lit;
    bool m_open = false;
    std::function<void(View&, SnapZone)> m_onSnapped;
  };
} // namespace andromeda

#pragma once

#include "util/tracked.h"
#include "view/snap.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

struct wlr_scene_rect;
struct wlr_scene_tree;

namespace andromeda {
  class Output;
  class View;

  // Snap Layouts flyout (Windows 11; DESIGN.md "Free mode Snap Layouts"): a thick sheet with the six layouts drawn
  // small; a zone under the pointer lights up and a click snaps the window there. Opened by Super+Z, by a window drag
  // reaching the top edge, and by dwelling on a title bar's maximize button.
  class SnapFlyout {
  public:
    explicit SnapFlyout(wlr_scene_tree* parent);
    ~SnapFlyout();
    SnapFlyout(const SnapFlyout&) = delete;
    SnapFlyout& operator=(const SnapFlyout&) = delete;

    // Opens for `view` with the sheet's top centre at (x, y) in layout coordinates, kept inside `bounds`. `keyboard`
    // marks the first zone, so arrows and Enter work at once.
    void open(View& view, int x, int y, const wlr_box& bounds, bool keyboard);
    void close();
    [[nodiscard]] bool active() const { return m_tree != nullptr && m_open; }
    [[nodiscard]] View* view() const { return m_view.get(); }

    [[nodiscard]] std::optional<SnapZone> zoneAt(double lx, double ly) const;
    [[nodiscard]] bool contains(double lx, double ly) const;
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

  private:
    struct Mini {
      wlr_box box{};
      std::vector<std::pair<SnapZone, wlr_box>> zones;
    };
    void draw();
    void setLit(std::optional<SnapZone> zone);

    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_rect* m_sheet = nullptr;
    std::vector<wlr_scene_rect*> m_zoneRects; // in Mini/zone order
    std::array<Mini, kSnapLayouts> m_minis{};
    wlr_box m_box{};
    Ref<View> m_view;
    std::optional<SnapZone> m_lit;
    bool m_open = false;
    std::function<void(View&, SnapZone)> m_onSnapped;
  };
} // namespace andromeda

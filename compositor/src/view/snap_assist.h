#pragma once

#include "util/tracked.h"
#include "view/snap.h"

#include <memory>
#include <optional>
#include <vector>

struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_scene_tree;

namespace andromeda {
  class Server;
  class View;
  namespace ov {
    class AppIcons;
  }

  // Snap assist and snap groups (Windows 11; DESIGN.md "Free mode Snap Layouts"). After a window snaps to a Snap
  // Layouts zone, each empty zone of that layout in turn shows the workspace's other windows as cards (app icon and
  // title); a pick snaps that window there. Windows placed together form a snap group: raising one raises the group,
  // restoring one from minimized restores the group. A member that leaves its zone or workspace leaves the group.
  class SnapAssist {
  public:
    SnapAssist(Server& server, wlr_scene_tree* parent);
    ~SnapAssist();
    SnapAssist(const SnapAssist&) = delete;
    SnapAssist& operator=(const SnapAssist&) = delete;

    // `anchor` just snapped to `zone`; shows the picker in the first empty zone of its layout, if any window can go.
    void start(View& anchor, SnapZone zone);
    void end();
    [[nodiscard]] bool active() const { return m_open; }

    void hover(double lx, double ly);
    // A press on a card picks it; inside the sheet it is kept; outside it ends assist and passes on. Returns consumed.
    bool press(double lx, double ly);
    // Arrows walk the cards, Enter picks, Escape ends; every other key ends and passes on. Returns consumed.
    bool key(uint32_t keysym);

    // The other live members of `view`'s snap group, bottom to top; empty when it is in none.
    [[nodiscard]] std::vector<View*> groupMates(const View* view);

  private:
    struct Card {
      Ref<View> view;
      wlr_box box{};
      wlr_scene_rect* rect = nullptr;
    };
    struct Member {
      Ref<View> view;
      SnapZone zone;
    };
    [[nodiscard]] bool showNextZone();
    void build();
    void draw();
    void pick(size_t index);
    void finish();
    // Ends assist when its anchor window left its zone or went away; returns whether it still holds.
    bool anchorHolds();
    [[nodiscard]] std::optional<size_t> cardAt(double lx, double ly) const;

    Server& m_server;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_tree* m_content = nullptr; // rebuilt per zone
    wlr_scene_rect* m_sheet = nullptr;
    std::unique_ptr<ov::AppIcons> m_icons;
    std::vector<Card> m_cards;
    std::vector<Member> m_members; // the group being formed, anchor first
    std::vector<std::vector<Member>> m_groups;
    wlr_box m_box{};
    SnapZone m_zone;
    size_t m_lit = 0;
    bool m_open = false;
  };
} // namespace andromeda

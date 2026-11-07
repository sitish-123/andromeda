#pragma once

#include "core/animation.h"
#include "util/listener.h"
#include "util/tracked.h"
#include "view/snap.h"

#include <memory>
#include <optional>
#include <vector>

struct wlr_output;
struct wlr_scene_rect;
struct wlr_scene_shadow;
struct wlr_scene_tree;

namespace andromeda {
  class GlassPanel;
  class Server;
  class View;
  class ViewChromeAttachment;
  namespace ov {
    class AppIcons;
  }

  // Snap assist and snap groups (Windows 11; DESIGN.md "Free mode Snap Layouts"). After a window snaps to a Snap
  // Layouts zone, each empty zone of that layout in turn dims under a blurred scrim and shows the workspace's other
  // windows as live thumbnails with their app icon and title; a pick snaps that window there. Windows placed together
  // form a snap group: raising one raises the group, restoring one from minimized restores the group. A member that
  // leaves its zone or workspace leaves the group.
  class SnapAssist : public Animatable {
  public:
    SnapAssist(Server& server, wlr_scene_tree* parent);
    ~SnapAssist() override;
    SnapAssist(const SnapAssist&) = delete;
    SnapAssist& operator=(const SnapAssist&) = delete;

    // `anchor` just snapped to `zone`; shows the picker in the first empty zone of its layout, if any window can go.
    void start(View& anchor, SnapZone zone);
    // `animate` false hides at once (interruptions: lock, output removal, workspace switch).
    void end(bool animate = true);
    [[nodiscard]] bool active() const { return m_open; }

    void hover(double lx, double ly);
    // A press on a card picks it; inside the zone it is kept; outside it ends assist and passes on. Returns consumed.
    bool press(double lx, double ly);
    // Arrows walk the cards, Enter picks, Escape ends; every other key ends and passes on. Returns consumed.
    bool key(uint32_t keysym);

    // The other live members of `view`'s snap group, bottom to top; empty when it is in none.
    [[nodiscard]] std::vector<View*> groupMates(const View* view);
    // The id of `view`'s snap group (stable while the group lives), for the IPC window list.
    [[nodiscard]] std::optional<uint32_t> groupOf(const View* view);

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override;

  private:
    struct Card {
      Ref<View> view;
      wlr_box box{};   // thumbnail slot and label, the hit and lens area
      wlr_box thumb{}; // the thumbnail inside the slot
      double zoom = 1.0;
      wlr_scene_tree* tree = nullptr;     // at `thumb`, clipped to it
      wlr_scene_tree* surfaces = nullptr; // copies of the window's surface buffers, rebuilt on each commit
      std::unique_ptr<ViewChromeAttachment> frame;
      Listener commit;
      Listener destroy;
    };
    struct Member {
      Ref<View> view;
      SnapZone zone;
    };
    [[nodiscard]] bool showNextZone();
    void build();
    void syncThumbnail(Card& card);
    void moveLens(bool animate);
    void placeLens();
    // Applies m_fade to the scrim, lens, cards and labels.
    void applyFade();
    void destroyContent();
    void pick(size_t index);
    void finish();
    // Ends assist when its anchor window left its zone or went away; returns whether it still holds.
    bool anchorHolds();
    [[nodiscard]] std::optional<size_t> cardAt(double lx, double ly) const;

    Server& m_server;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_tree* m_content = nullptr; // rebuilt per zone
    std::unique_ptr<GlassPanel> m_scrim;
    wlr_scene_shadow* m_lensShadow = nullptr;
    wlr_scene_rect* m_lens = nullptr;
    std::unique_ptr<ov::AppIcons> m_icons;
    std::vector<std::unique_ptr<Card>> m_cards;
    std::vector<Member> m_members; // the group being formed, anchor first
    struct Group {
      uint32_t id = 0;
      std::vector<Member> members;
    };
    // Drops members that left their zone or workspace, then groups under 2.
    void pruneGroups();
    std::vector<Group> m_groups;
    uint32_t m_nextGroupId = 1;
    std::array<AnimatedValue, 4> m_lensBox; // left, top, right, bottom
    // Opacity of the whole assist: in on fade, out on fade_out (rulebook 7.3: fade alone for a frame that does not move).
    AnimatedValue m_fade{0.0};
    std::array<float, 4> m_lensColor{};
    std::array<float, 4> m_lensShadowColor{};
    wlr_output* m_output = nullptr;
    wlr_box m_box{};
    SnapZone m_zone;
    size_t m_lit = 0;
    int m_columns = 1;
    bool m_open = false;
  };
} // namespace andromeda

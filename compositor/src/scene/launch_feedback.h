#pragma once

#include "core/animation.h"

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_tree;

namespace andromeda {

  class Output;
  class Server;

  // Launch feedback (rulebook P15): the launching icon's pixels lift off the dock or launcher, move toward the middle
  // of the output and breathe there until the app's first window maps; then they grow and fade into it.
  class LaunchFeedback : public Animatable {
  public:
    // `buffer` is locked for the feedback's life. `source` is in buffer pixels; `from` and `area` are layout boxes:
    // the icon and the output's usable area.
    LaunchFeedback(
        Server& server, Output* output, wlr_scene_tree* parent, wlr_buffer* buffer, const wlr_fbox& source,
        const wlr_box& from, const wlr_box& area
    );
    ~LaunchFeedback() override;
    LaunchFeedback(const LaunchFeedback&) = delete;
    LaunchFeedback& operator=(const LaunchFeedback&) = delete;

    // The window mapped (or the launch expired): grow and fade out.
    void finish();
    [[nodiscard]] bool finished() const;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override { return !finished(); }
    [[nodiscard]] bool animatesOn(const Output* output) const override { return output == m_output; }

  private:
    void apply(double nowMsec);

    Server* m_server = nullptr;
    Output* m_output = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_buffer* m_node = nullptr;
    wlr_buffer* m_buffer = nullptr;
    wlr_box m_from{};
    double m_targetX = 0.0; // centre the icon travels to, layout coordinates
    double m_targetY = 0.0;
    AnimatedValue m_travel{0.0}; // 0 at the icon, 1 at the target
    AnimatedValue m_alpha{0.0};
    AnimatedValue m_exit{0.0}; // 0 while waiting, 1 when grown into the window
    double m_breathStartMsec = -1.0;
    bool m_finishing = false;
  };

} // namespace andromeda

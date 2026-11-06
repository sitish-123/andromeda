#pragma once

#include "core/animation.h"

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

#include <cstdint>
#include <vector>

struct wlr_scene_border;
struct wlr_scene_rect;
struct wlr_scene_shadow;
struct wlr_scene_tree;

namespace andromeda {

  class Output;
  class Server;

  // How the hint draws, all in colors.insert_hint: Fill is the plain tint; Ring is a solid ring over a faint fill (the
  // overview's "drops here" slot); Glow is a solid bar with a soft halo (the overview's gap between workspaces); Edge
  // fades linearly from 35 % at the box's `strong` side to clear at the opposite side (an armed auto-scroll edge).
  enum class HintLook : uint8_t { Fill, Ring, Glow, Edge };

  // Animated drop-target indicator: a rounded rect that fades in on appear and morphs between target boxes. Hiding is
  // immediate, so the hint never lingers over the window a released drag settles into. Alpha and geometry are written
  // only from tickAnimations (plus snap points), never ad hoc.
  class HintRect : public Animatable {
  public:
    HintRect(Server& server, wlr_scene_tree* parent);
    ~HintRect();
    HintRect(const HintRect&) = delete;
    HintRect& operator=(const HintRect&) = delete;

    // `box` in parent-tree coordinates. A zero-sized box hides the hint.
    void show(
        Output* output, const wlr_box& box, int cornerRadius, HintLook look = HintLook::Fill,
        wlr_edges strong = WLR_EDGE_NONE
    );
    void hideImmediate();
    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override { return m_output == output; }
    [[nodiscard]] Output* output() const { return m_output; }

  private:
    void ensureScene();
    void retargetGeometry(const wlr_box& box);
    void applyState();

    Server* m_server = nullptr;
    wlr_scene_tree* m_parent = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_rect* m_rect = nullptr;
    wlr_scene_border* m_ring = nullptr; // Ring look only, made on first use
    wlr_scene_shadow* m_glow = nullptr; // Glow look only, made on first use
    std::vector<wlr_scene_rect*> m_bands; // Edge look only: the gradient steps, made on first use
    wlr_edges m_strong = WLR_EDGE_NONE;
    Output* m_output = nullptr;
    HintLook m_look = HintLook::Fill;
    int m_radius = 0;
    int m_ringWidth = 0; // the size the ring geometry was built for
    int m_ringHeight = 0;
    AnimatedValue m_alpha;
    AnimatedValue m_x;
    AnimatedValue m_y;
    AnimatedValue m_w;
    AnimatedValue m_h;
    wlr_box m_targetBox{};
    bool m_visible = false;
  };

} // namespace andromeda

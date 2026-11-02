#pragma once

#include <array>
#include <optional>

struct wlr_scene_shadow;
struct wlr_scene_tree;
struct wlr_scene_node;

namespace andromeda {

  struct ShadowSnapshot {
    wlr_scene_tree* tree = nullptr;
    wlr_scene_shadow* node = nullptr;
    std::array<float, 4> color{};
  };

  struct ShadowOpacity {
    float light = 1.0F;
    float dark = 1.0F;
  };

  // A shadow from design tokens (elevation.*) instead of appearance.shadow: a key shadow and a contact shadow.
  struct ShadowStyle {
    int softness = 0;
    int offsetX = 0;
    int offsetY = 0;
    // Scale the scheme's shadow color alpha; the active scheme picks one, so a scheme switch needs no new style.
    ShadowOpacity opacity;
    int contactSoftness = 0;
    int contactOffsetY = 0;
    ShadowOpacity contactOpacity; // 0 in both schemes draws no contact shadow
  };

  // Owns the desired-state logic for one SceneFX drop-shadow node. The node is a
  // child of the owner's scene tree and freed by scene-tree teardown (no destructor).
  class SurfaceShadow {
  public:
    // contentWidth/Height: toplevel geometry size. borderTotal: decoration ring width drawn outside the content (0 when
    // borders are disabled/hidden). cornerRadius: radius of the decoration's outer edge (0 = square).
    void update(
        wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius,
        int topExtension = 0, int originX = 0, int originY = 0
    );
    // A style draws the shadow even when appearance.shadow.enabled is off; std::nullopt follows appearance.shadow.
    void setStyle(std::optional<ShadowStyle> style);
    // Owner override of appearance.shadow.enabled. std::nullopt follows the global
    // switch, so a window rule can both drop a shadow the global config draws and
    // draw one it does not. Owners that never call this follow the global switch.
    void setEnabled(std::optional<bool> enabled);
    // Disable the node (unmap/fullscreen/off-output path); update() re-enables.
    void hide();
    // Forget the node pointer (caller is destroying the parent tree externally).
    void reset();
    // Set an opacity multiplier applied to the shadow color (for fade animations).
    void setAlpha(float alpha);
    void setAnimationSource(wlr_scene_node* source);
    // `inPool` puts the copy under every sibling in `parent`; otherwise it sits directly below `source`, a sibling.
    [[nodiscard]] ShadowSnapshot snapshot(wlr_scene_tree* parent, wlr_scene_node* source, bool inPool) const;
    // Null until the first update() creates it.
    [[nodiscard]] const wlr_scene_shadow* node() const { return m_node; }

  private:
    // A style takes the scheme's design-token shadow color; otherwise colors.shadow. Without the fade alpha.
    [[nodiscard]] std::array<float, 4> baseColor(ShadowOpacity opacity) const;
    // Creates `node` when null, then sizes, places and colours it around the decorated box.
    void place(
        wlr_scene_shadow*& node, wlr_scene_tree* parent, int sigma, int offsetX, int offsetY,
        const std::array<float, 4>& color, int decWidth, int decHeight, int borderTotal, int topExtension, int originX,
        int originY, int cornerRadius
    );

    wlr_scene_shadow* m_node = nullptr;
    wlr_scene_shadow* m_contact = nullptr; // only with a style that has a contact shadow
    float m_alpha = 1.0F;
    // Follows appearance.shadow.enabled until an owner overrides it either way.
    std::optional<bool> m_enabled;
    std::optional<ShadowStyle> m_style;
  };

} // namespace andromeda

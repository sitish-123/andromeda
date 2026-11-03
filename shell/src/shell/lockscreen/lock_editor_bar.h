#pragma once

#include <array>
#include <string>
#include <vector>

class Label;
class Node;
class RectNode;
class Renderer;

// Lock screen editor chrome (mock section 2): the top bar (Slots, Free, Wallpaper, Layout, Theme, Done) and the
// Layout and Theme panel under it. Draws and hit-tests only; LockEditor owns the draft and the input.
class LockEditorBar {
public:
  enum class Segment { Slots, Free, Wallpaper, Layout, Theme };
  static constexpr int kSegments = 5;
  static constexpr int kLayouts = 4;
  static constexpr int kThemes = 2;

  enum class Target { None, Segment, Done, LayoutCard, ThemeChip, Panel, Bar };
  struct Hit {
    Target target = Target::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
  };

  struct State {
    Segment mode = Segment::Slots;
    bool panelOpen = false;
    // Index into kLayoutNames / kThemeNames.
    int layout = 0;
    int theme = 0;
    // Keyboard focus ring; Target::None = no ring.
    Hit focus;
    Hit hover;
    bool operator==(const State&) const = default;
  };

  static constexpr std::array<const char*, kLayouts> kLayoutNames{"centered", "glance", "split", "minimal"};
  static constexpr std::array<const char*, kThemes> kThemeNames{"andromeda", "ryoku"};

  void build(Node& root);
  void setState(const State& state);
  [[nodiscard]] const State& state() const noexcept { return m_state; }
  // scale = accessibility.ui_scale.
  void layout(Renderer& renderer, float sw, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;
  // Free and Wallpaper open with L6c and L7; until then they draw dimmed and take no input.
  [[nodiscard]] static bool segmentEnabled(int index) noexcept;
  // Keyboard order: enabled segments, Done, then the panel's cards and chips while it is open.
  [[nodiscard]] static std::vector<Hit> focusOrder(bool panelOpen);

private:
  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    [[nodiscard]] bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct Chip {
    RectNode* bg = nullptr;
    Label* label = nullptr;
    Rect rect;
  };
  struct Card {
    RectNode* bg = nullptr;
    RectNode* clock = nullptr;
    RectNode* line = nullptr;
    Label* label = nullptr;
    Rect rect;
  };

  void layoutChip(Renderer& renderer, Chip& chip, float x, float y, float s, bool on, bool enabled, bool focused);
  void layoutPanel(Renderer& renderer, float sw, float s);
  [[nodiscard]] bool focused(Target target, int index) const noexcept;

  State m_state;
  Node* m_group = nullptr;
  RectNode* m_barBg = nullptr;
  Rect m_barRect;
  std::array<Chip, kSegments> m_segments;
  Chip m_done;
  RectNode* m_doneFocus = nullptr;
  RectNode* m_panelBg = nullptr;
  Rect m_panelRect;
  Label* m_layoutTitle = nullptr;
  Label* m_themeTitle = nullptr;
  Label* m_themeNote = nullptr;
  std::array<Card, kLayouts> m_cards;
  std::array<Chip, kThemes> m_themes;
};

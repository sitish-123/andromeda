#pragma once

#include <array>
#include <vector>

class Label;
class Node;
class RectNode;
class Renderer;

// Slots mode of the lock screen editor (mock section 2): the clock slot outline and, once the slot is picked, the clock
// style gallery at the bottom with the size, 24-hour and seconds options. Draws and hit-tests only.
class LockEditorGallery {
public:
  static constexpr int kSizes = 5;
  // lockscreen.clock_size values; "" = the layout's own size.
  static constexpr std::array<const char*, kSizes> kSizeNames{"", "s", "m", "l", "xl"};

  enum class Target { None, ClockSlot, Style, Size, Hour24, Seconds, Panel };
  struct Hit {
    Target target = Target::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
  };

  struct State {
    bool slotsMode = true;
    bool open = false;
    int style = 0;
    int size = 0;
    bool hour24 = true;
    bool seconds = false;
    Hit focus;
    Hit hover;
    bool operator==(const State&) const = default;
  };

  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    [[nodiscard]] bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  void build(Node& root);
  // slot = the date and clock block of the preview; scale = accessibility.ui_scale.
  void layout(Renderer& renderer, const State& state, Rect slot, float sw, float sh, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;
  // Keyboard order after the bar's: the clock slot, then the gallery's tiles and options while it is open.
  [[nodiscard]] static std::vector<Hit> focusOrder(const State& state, int styleCount);
  [[nodiscard]] const Rect& panelRect() const noexcept { return m_panelRect; }

  struct Chip {
    RectNode* bg = nullptr;
    Label* label = nullptr;
    Rect rect;
  };
  // Option chip (h 28, capsule): white when on, an accent ring when focused. The label text is set by the caller.
  static void layoutChip(Renderer& renderer, Chip& chip, float x, float y, float s, bool on, bool focused, bool hover);

private:
  struct Tile {
    RectNode* bg = nullptr;
    Label* sample = nullptr;
    Label* name = nullptr;
    Rect rect;
  };
  State m_state;
  Node* m_group = nullptr;
  RectNode* m_slot = nullptr;
  Rect m_slotRect;
  RectNode* m_panelBg = nullptr;
  Rect m_panelRect;
  Label* m_title = nullptr;
  std::vector<Tile> m_tiles;
  std::array<Chip, kSizes> m_sizes;
  Chip m_hour24;
  Chip m_seconds;
};

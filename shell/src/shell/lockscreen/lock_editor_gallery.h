#pragma once

#include "ui/auth/analog_clock.h"
#include "ui/auth/flip_clock.h"

#include <array>
#include <cstdint>
#include <vector>

class AnimationManager;
class Label;
class Node;
class RectNode;
class Renderer;
struct ClockStyle;

// Slots mode of the lock screen editor: the clock slot outline and, once the slot is picked, the clock sheet
// (.agent/DESIGN.md "Lock clock styles v2"): filter chips and style tiles drawn in their own style on the left; font
// axis bars, colour swatches, size, 24-hour and seconds on the right. Draws and hit-tests only.
class LockEditorGallery {
public:
  static constexpr int kSizes = 6;
  // lockscreen.clock_size values; "" = the layout's own size, "fill" = the largest that fits.
  static constexpr std::array<const char*, kSizes> kSizeNames{"", "s", "m", "l", "xl", "fill"};
  static constexpr int kFilters = 7;
  static constexpr int kColors = 4;
  // lockscreen.clock_color values.
  static constexpr std::array<const char*, kColors> kColorNames{"white", "accent", "wallpaper", "gradient"};
  // Weight, Width, Round, Slant (clock_style.h ClockAxisValues).
  static constexpr int kAxes = 4;
  static constexpr int kMaterials = 3;
  // lockscreen.clock_material values.
  static constexpr std::array<const char*, kMaterials> kMaterialNames{"solid", "tint", "glass"};

  // Stretch = the drag handle on the clock slot's bottom-right corner.
  enum class Target { None, ClockSlot, Filter, Style, Axis, Material, Color, Size, Hour24, Seconds, Motion, Panel, Stretch };
  struct Hit {
    Target target = Target::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
  };

  struct State {
    bool slotsMode = true;
    bool open = false;
    int filter = 0;
    int style = 0;
    // Raw config values: weight, width, round, slant; 0 (round -1) = the style's own value.
    std::array<int, kAxes> axes{0, 0, -1, 0};
    int material = 0;
    int color = 0;
    int size = 0;
    bool motion = true;
    bool hour24 = true;
    bool seconds = false;
    // Swatch inks, packed 0xRRGGBBAA: the wallpaper tone and the gradient's second stop (0 = unknown).
    std::uint32_t wallpaperInk = 0;
    std::uint32_t gradientTo = 0;
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

  struct AxisRange {
    bool available = false;
    float min = 0.0F;
    float max = 0.0F;
    // The value the style draws with no override.
    float base = 0.0F;
  };
  // The axis range of `style`'s font; Slant counts degrees (0..15).
  [[nodiscard]] static AxisRange axisRange(const ClockStyle& style, int axis);
  // The drawn value of an axis: the override, else the style's own.
  [[nodiscard]] static float axisValue(const ClockStyle& style, int axis, int raw);
  // The config value for a drawn value (rounded; the style's own value maps back to "no override").
  [[nodiscard]] static int axisRaw(const ClockStyle& style, int axis, float value);
  // Styles the filter chip shows: All, Digital, Serif, Stacked, Analog, Animated, Matrix.
  [[nodiscard]] static bool inFilter(const ClockStyle& style, int filter);

  void build(Node& root, AnimationManager& animations);
  // slot = the date and clock block of the preview; scale = accessibility.ui_scale.
  void layout(Renderer& renderer, const State& state, Rect slot, float sw, float sh, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;
  // The axis value (in the axis' own units) under x for an Axis hit.
  [[nodiscard]] float axisValueAt(const State& state, int axis, float x) const;
  // Keyboard order after the bar's: the clock slot, then the sheet's filters, shown tiles and options while open.
  [[nodiscard]] static std::vector<Hit> focusOrder(const State& state, int styleCount);
  [[nodiscard]] const Rect& panelRect() const noexcept { return m_panelRect; }
  [[nodiscard]] const Rect& slotRect() const noexcept { return m_slotRect; }

  struct Chip {
    RectNode* bg = nullptr;
    Label* label = nullptr;
    Rect rect;
  };
  // Option chip (h 28, capsule): white when on, an accent ring when focused. The label text is set by the caller.
  static void layoutChip(Renderer& renderer, Chip& chip, float x, float y, float s, bool on, bool focused, bool hover);

  // Panel backgrounds the editor puts frosted glass under.
  [[nodiscard]] std::vector<const RectNode*> glassBackgrounds() const { return {m_panelBg}; }
  [[nodiscard]] Node* panelGroup() const { return m_group; }

private:
  struct Tile {
    RectNode* bg = nullptr;
    Label* sample = nullptr;
    Label* name = nullptr;
    // Two-tone and Split minutes; Neon glow and 7-segment unlit segments under the sample.
    Label* minutes = nullptr;
    Label* under = nullptr;
    Rect rect;
  };
  struct Bar {
    Label* label = nullptr;
    RectNode* track = nullptr;
    RectNode* fill = nullptr;
    RectNode* knob = nullptr;
    Label* value = nullptr;
    Rect rect;
    Rect trackRect;
  };
  struct Swatch {
    RectNode* ring = nullptr;
    RectNode* dot = nullptr;
    Rect rect;
  };
  void layoutBar(Renderer& renderer, Bar& bar, int axis, float x, float y, float w, float s);

  State m_state;
  Node* m_group = nullptr;
  RectNode* m_slot = nullptr;
  Rect m_slotRect;
  RectNode* m_handle = nullptr;
  Rect m_handleRect;
  RectNode* m_panelBg = nullptr;
  Rect m_panelRect;
  Label* m_title = nullptr;
  std::array<Chip, kFilters> m_filters;
  std::vector<Tile> m_tiles;
  // The Analog, Flip and Two-tone tiles draw their real clock: a dial, two cards, accent minutes.
  AnalogClock m_tileDial;
  AnalogClock m_tileOrbit;
  FlipClock m_tileFlip;
  std::array<Bar, kAxes> m_bars;
  Label* m_materialLabel = nullptr;
  std::array<Chip, kMaterials> m_materials;
  Label* m_colorLabel = nullptr;
  std::array<Swatch, kColors> m_swatches;
  Label* m_sizeLabel = nullptr;
  std::array<Chip, kSizes> m_sizes;
  Chip m_hour24;
  Chip m_seconds;
  Chip m_motion;
};

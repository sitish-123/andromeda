#pragma once

#include "render/core/color.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

class Image;
class Label;
class Node;
class RectNode;
class Renderer;

// Wallpaper tab of the lock screen editor (mock section 3): a bottom panel with Photos / Live / Colours tabs, the
// "Applies to" chip, a 6 x 2 tile grid that scrolls by rows, and the Blur and Dim sliders. Draws and hit-tests only.
class LockEditorWallpaper {
public:
  static constexpr int kTabs = 3;
  static constexpr int kColumns = 6;
  static constexpr int kRows = 2;

  enum class Target { None, Tab, Scope, Tile, Blur, Dim, Panel };
  struct Hit {
    Target target = Target::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
  };

  struct Item {
    enum class Kind { Desktop, Image, Video, Shader, Colour };
    Kind kind = Kind::Image;
    // The lockscreen.wallpaper value: "" for Desktop, a file path, or "color:#rrggbb".
    std::string value;
    std::string name;
    Color colour;
    // A live item's still image (lock_thumbnail::find); empty until one exists.
    std::string still;
    bool operator==(const Item&) const = default;
  };

  struct State {
    bool open = false;
    int tab = 0;
    bool allDisplays = true;
    std::string picked;
    // lockscreen.blur_intensity and tint_intensity, 0..1; the Dim slider spans 0..kMaxDim.
    float blur = 0.5F;
    float dim = 0.3F;
    int scrollRow = 0;
    // Shown when the tab has no items.
    std::string emptyText;
    Hit focus;
    Hit hover;
    bool operator==(const State&) const = default;
  };
  static constexpr float kMaxDim = 0.6F;

  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    [[nodiscard]] bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  // The items of `tab` (0 Photos, 1 Live, 2 Colours). Photos and Live list `directory` (one level, by name); Photos
  // starts with "Same as desktop".
  [[nodiscard]] static std::vector<Item> items(int tab, const std::string& directory);

  void build(Node& root);
  // `items` = the current tab's items; scale = accessibility.ui_scale.
  void layout(Renderer& renderer, const State& state, const std::vector<Item>& items, float sw, float sh, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;
  // The slider value (0..1 of the track) under x for a Blur or Dim hit.
  [[nodiscard]] float sliderFraction(Target target, float x) const;
  // Keyboard order while open: tabs, scope chip, every tile, Blur, Dim.
  // The first row that keeps item `index` in view, starting from `scrollRow`.
  [[nodiscard]] static int rowFor(int index, int scrollRow);
  [[nodiscard]] static std::vector<Hit> focusOrder(const State& state, int itemCount);
  [[nodiscard]] const Rect& panelRect() const noexcept { return m_panelRect; }
  // Called by the owner when a thumbnail finished loading.
  void setThumbnailReadyCallback(std::function<void()> callback) { m_onThumbnail = std::move(callback); }

private:
  struct Tile {
    RectNode* bg = nullptr;
    Image* thumb = nullptr;
    RectNode* ring = nullptr;
    Label* name = nullptr;
    RectNode* badge = nullptr;
    Label* badgeText = nullptr;
    std::string source;
    Rect rect;
  };
  struct Slider {
    Label* label = nullptr;
    RectNode* track = nullptr;
    RectNode* fill = nullptr;
    Label* value = nullptr;
    Rect rect;
    Rect trackRect;
  };
  struct Chip {
    RectNode* bg = nullptr;
    Label* label = nullptr;
    Rect rect;
  };

  void layoutTile(Renderer& renderer, Tile& tile, const Item* item, float x, float y, float w, float h, float s,
                  bool picked, bool focused, bool hover);
  void layoutSlider(Renderer& renderer, Slider& slider, const char* label, float fraction, const std::string& value,
                    float x, float y, float w, float s, bool focused);

  Node* m_group = nullptr;
  RectNode* m_panelBg = nullptr;
  Rect m_panelRect;
  std::array<Chip, kTabs> m_tabs;
  Label* m_scopeKey = nullptr;
  Chip m_scope;
  std::array<Tile, kColumns * kRows> m_tiles;
  int m_firstItem = 0;
  Label* m_empty = nullptr;
  Slider m_blur;
  Slider m_dim;
  Label* m_foot = nullptr;
  std::function<void()> m_onThumbnail;
};

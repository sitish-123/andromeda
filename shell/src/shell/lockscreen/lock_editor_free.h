#pragma once

#include "config/config_types.h"
#include "shell/lockscreen/lock_editor_gallery.h"
#include "shell/lockscreen/lock_free_layout.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AnimationManager;
class DesktopWidget;
class DesktopWidgetFactory;
class Label;
class Node;
class RectNode;
class Renderer;

// Free mode of the lock screen editor (mock section 2): lock widgets as boxes placed anywhere on the 32 grid, or inside
// the Top / Middle bands with "Snap to zones" (a bar toggle); the auth area rejects drops. Draws each box with its live
// widget (DesktopWidgetFactory, as the lock draws it), the zone bands and the bottom toolbar of add chips; owns the
// drag. The editor owns the item list and saves it.
class LockEditorFree {
public:
  // Widget types the lock may show (desktop_widget_factory ids), in add-chip order.
  static constexpr std::array<const char*, 6> kTypes{"clock", "calendar", "weather", "media_player", "sticker", "label"};

  struct Item {
    std::string id;
    std::string type;
    AuthRect box;
    std::unordered_map<std::string, WidgetSettingValue> settings;
  };

  enum class Target { None, Item, Resize, Add, Toolbar };
  struct Hit {
    Target target = Target::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
  };

  struct State {
    bool active = false;
    bool snapZones = true;
    std::vector<Item> items;
    int selected = -1;
    Hit focus;
    Hit hover;
  };

  LockEditorFree();
  ~LockEditorFree();
  LockEditorFree(const LockEditorFree&) = delete;
  LockEditorFree& operator=(const LockEditorFree&) = delete;

  // Live widgets: built by `factory`; `refresh` asks the editor for a new frame when a widget changes.
  void setFactory(const DesktopWidgetFactory* factory, AnimationManager* animations, std::function<void()> refresh);
  // Destroys the live widgets; call while the scene that holds their nodes is still alive.
  void clear();
  void build(Node& root);
  void layout(Renderer& renderer, const State& state, const lock_free::Zones& zones, float sw, float sh, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;
  // Keyboard order after the bar's: the items, then the add chips.
  [[nodiscard]] static std::vector<Hit> focusOrder(const State& state);

  // Pointer drag of item `index` (move, or resize from the bottom right corner); end() returns the placed box, or
  // nullopt when the drop was refused (the item keeps its box).
  void begin(const State& state, int index, bool resize, float x, float y);
  [[nodiscard]] bool dragging() const noexcept { return m_drag.index >= 0; }
  [[nodiscard]] int dragIndex() const noexcept { return m_drag.index; }
  // False while the pointer is still within 2 px of the press: a click, not a drag.
  [[nodiscard]] bool moved(float x, float y) const noexcept;
  void cancel() noexcept { m_drag = {}; }
  // The live box under the pointer while dragging.
  [[nodiscard]] AuthRect drag(float x, float y, float scale) const;
  [[nodiscard]] std::optional<AuthRect> end(
      float x, float y, const State& state, const lock_free::Zones& zones, float sw, float sh, float scale
  );

private:
  struct Box {
    RectNode* bg = nullptr;
    RectNode* ring = nullptr;
    Label* name = nullptr;
    RectNode* handle = nullptr;
    AuthRect rect;
  };
  struct Live {
    std::string id;
    std::string type;
    std::unique_ptr<DesktopWidget> widget;
    Node* node = nullptr;
  };
  // The live widget of `item`, built on first use; nullptr when the factory cannot build its type.
  Live* liveFor(const Item& item, float scale);
  struct Drag {
    int index = -1;
    bool resize = false;
    float startX = 0.0F;
    float startY = 0.0F;
    AuthRect start;
  };

  State m_state;
  float m_scale = 1.0F;
  const DesktopWidgetFactory* m_factory = nullptr;
  AnimationManager* m_animations = nullptr;
  std::function<void()> m_refresh;
  std::vector<Live> m_live;
  Node* m_group = nullptr;
  std::vector<Box> m_boxes;
  std::array<RectNode*, 3> m_zones{};
  std::array<Label*, 3> m_zoneLabels{};
  RectNode* m_toolbarBg = nullptr;
  LockEditorGallery::Rect m_toolbarRect;
  std::array<LockEditorGallery::Chip, kTypes.size()> m_add{};
  Drag m_drag;
};

#pragma once

#include "shell/lockscreen/lock_editor_gallery.h"

#include <array>
#include <string>
#include <vector>

class Label;
class Node;
class RectNode;
class Renderer;

// Slots mode of the lock screen editor (mock section 2), below the clock slot: the widget row (placed widgets and "+"
// places up to 4) with the widget gallery, and the bottom slot (Centered) with its chooser. Draws outlines, "+" marks
// and the panels; LockCards draws the widgets and the card. Draws and hit-tests only.
class LockEditorSlots {
public:
  using Rect = LockEditorGallery::Rect;
  static constexpr int kPlaces = 4;
  static constexpr int kKinds = 4;
  // lockscreen.bottom_slot values, in chooser order.
  static constexpr std::array<const char*, 4> kBottomNames{"media", "phone", "notifications", "none"};

  enum class Panel { None, Widget, Bottom };
  // Place / Kind / BottomKind carry an index; Remove drops the edited place's widget.
  enum class Target { None, Place, BottomSlot, Kind, Remove, BottomKind, Panel };
  struct Hit {
    Target target = Target::None;
    int index = -1;
    bool operator==(const Hit&) const = default;
  };

  struct State {
    bool active = false;
    // Row places shown (0 = no row in this layout) and widgets placed in them.
    int places = 0;
    std::vector<std::string> widgets;
    bool bottom = false;
    int bottomSlot = 0;
    Panel panel = Panel::None;
    // The place the widget gallery edits.
    int editPlace = -1;
    Hit focus;
    Hit hover;
    bool operator==(const State&) const = default;
  };

  void build(Node& root);
  // placeRects = the row's places (LockCards::widgetRect), bottomRect = the bottom slot card; scale = ui_scale.
  void layout(
      Renderer& renderer, const State& state, const std::vector<Rect>& placeRects, Rect bottomRect, float sw, float sh,
      float scale
  );
  [[nodiscard]] Hit hitTest(float x, float y) const;
  // Keyboard order after the clock gallery's: the row places, the bottom slot, then the open panel's chips.
  [[nodiscard]] static std::vector<Hit> focusOrder(const State& state);

private:
  struct Place {
    RectNode* outline = nullptr;
    Label* plus = nullptr;
    Rect rect;
  };

  State m_state;
  Node* m_group = nullptr;
  std::array<Place, kPlaces> m_places{};
  RectNode* m_bottom = nullptr;
  Rect m_bottomRect;
  RectNode* m_panelBg = nullptr;
  Rect m_panelRect;
  Label* m_title = nullptr;
  std::array<LockEditorGallery::Chip, kKinds> m_kinds{};
  LockEditorGallery::Chip m_remove;
  std::array<LockEditorGallery::Chip, kBottomNames.size()> m_bottomKinds{};
};

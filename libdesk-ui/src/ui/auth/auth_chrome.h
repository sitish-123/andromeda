#pragma once

#include <string>
#include <vector>

class Glyph;
class Label;
class Node;
class RectNode;
class Renderer;

// The controls around the auth column on the lock screen and the greeter (rulebook 9.15): the session button (bottom
// right) and its power menu, the session chip left of it, the "Other users" chip (bottom left), and one open menu.
// Draws and hit-tests only; the host owns the actions.
class AuthChrome {
public:
  enum class Menu { None, Users, Session, Power };
  enum class Target { None, Users, Session, Power, Item, Menu };

  struct Item {
    std::string glyph;
    std::string label;
    // Users list: an avatar with this initial replaces the glyph.
    std::string initial;
  };

  struct State {
    bool usersChip = false;
    // Empty = no session chip (one session installed).
    std::string sessionLabel;
    Menu menu = Menu::None;
    std::vector<Item> items;
    int current = -1;
    int hover = -1;
    bool operator==(const State&) const = default;
  };

  struct Hit {
    Target target = Target::None;
    int index = -1;
  };

  void build(Node& root);
  void setState(State state);
  [[nodiscard]] const State& state() const noexcept { return m_state; }
  // scale = accessibility.ui_scale.
  void layout(Renderer& renderer, float sw, float sh, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;

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
    Glyph* glyph = nullptr;
    Label* label = nullptr;
    Rect rect;
  };

  struct Row {
    RectNode* bg = nullptr;
    RectNode* avatar = nullptr;
    Label* initial = nullptr;
    Glyph* glyph = nullptr;
    Label* label = nullptr;
  };

  void layoutChip(Renderer& renderer, Chip& chip, float x, float y, bool alignRight, float s);
  void layoutMenu(Renderer& renderer, float sw, float sh, float s);

  State m_state;
  Node* m_group = nullptr;
  Chip m_users;
  Chip m_session;
  Chip m_power;
  RectNode* m_menuBg = nullptr;
  std::vector<Row> m_rows;
  Rect m_menuRect;
  std::vector<Rect> m_rowRects;
};

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class AnimationManager;
class Label;
class Node;
class RectNode;
class Renderer;

// The Ryoku look (ported from the Ryoku SDDM theme): black, a half-visible dial of minute and second rings turning
// with the time, the hour in Outfit Black beside a pill, the date, and a login column at the bottom right. The surface
// keeps its own password field (invisible, over the mask row) and state; this class only draws and animates.
class RyokuScene {
public:
  struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
  };

  void build(Node& root, AnimationManager& animations);
  void layout(Renderer& renderer, float ox, float oy, float sw, float sh);
  // Turns the rings to the current time: a smooth sweep when smooth, else whole seconds (the idle clock, drawn once
  // a second). True when the hour or date text changed and needs a layout.
  bool tick(bool smooth);

  void setUsers(const std::vector<std::string>& users, std::size_t selected);
  void setSessions(const std::vector<std::string>& sessions, std::size_t selected);
  void setHint(const std::string& text, bool isError);
  void setScanning(bool scanning);
  void setPasswordLength(std::size_t length);
  void setOnReboot(std::function<void()> callback);
  void setOnShutdown(std::function<void()> callback);
  void setOnUserPicked(std::function<void(std::size_t)> callback);
  void setOnSessionPicked(std::function<void(std::size_t)> callback);
  // Called when a menu opens or closes; a face match waits while one is open so the pick can land first.
  void setOnMenuChanged(std::function<void()> callback);
  void closeMenus();
  [[nodiscard]] bool menuOpen() const noexcept { return m_userMenu.open || m_sessionMenu.open; }

  // A rejected password: the hint turns red and the password row shakes.
  void playRejected();
  // Access granted: the dial blasts outward under a white flash, then onDone.
  void playGranted(std::function<void()> onDone);

  [[nodiscard]] Rect passwordRow() const noexcept { return m_passwordRow; }

private:
  struct Ring {
    Node* container = nullptr;
    std::array<RectNode*, 60> ticks{};
    std::array<Label*, 12> numbers{};
    float radius = 0.0f;
    float tickLong = 0.0f;
    float tickShort = 0.0f;
    float numberSize = 0.0f;
  };

  void buildRing(Ring& ring, float tickLong, float tickShort, float numberSize);
  void layoutRing(Renderer& renderer, Ring& ring, float cx, float cy, float numberInset);
  struct Menu {
    Node* group = nullptr;
    Label* opener = nullptr;
    std::vector<Label*> items;
    std::vector<std::string> names;
    std::size_t selected = 0;
    bool open = false;
    bool below = false;
    float reveal = 0.0f;
    std::function<void(std::size_t)> onPick;
  };

  void layoutMenu(Renderer& renderer, Menu& menu, float right, float anchorY);
  void toggleMenu(Menu& menu);
  void setMenuItems(Menu& menu, const std::vector<std::string>& names, std::size_t selected);
  void spotlight(Ring& ring, float ringAngle);
  void fadeColor(Label* label, bool lit);

  AnimationManager* m_animations = nullptr;
  float m_s = 1.0f;
  float m_ringCx = 0.0f;
  float m_ringCy = 0.0f;
  float m_viewH = 0.0f;
  Node* m_root = nullptr;
  RectNode* m_background = nullptr;
  Node* m_blast = nullptr;
  Node* m_hud = nullptr;
  Node* m_column = nullptr;
  RectNode* m_flash = nullptr;
  Ring m_minutes;
  Ring m_seconds;
  RectNode* m_pill = nullptr;
  RectNode* m_pillLine = nullptr;
  Label* m_hour = nullptr;
  Label* m_date = nullptr;
  Label* m_weekday = nullptr;
  Label* m_user = nullptr;
  Label* m_userMark = nullptr;
  Menu m_userMenu;
  Menu m_sessionMenu;
  std::function<void()> m_onMenuChanged;
  Label* m_hint = nullptr;
  Label* m_mask = nullptr;
  Label* m_waiting = nullptr;
  Label* m_session = nullptr;
  Label* m_reboot = nullptr;
  Label* m_shutdown = nullptr;
  RectNode* m_needle = nullptr;
  Rect m_passwordRow;
  std::string m_hourText;
  std::string m_dateText;
  std::size_t m_passwordLength = 0;
  bool m_scanning = false;
  float m_introOffset = 0.9f;
  float m_shake = 0.0f;
  float m_waitingOpacity = 0.4f;
  std::uint64_t m_waitingAnim = 0;
  float m_right = 0.0f;
};

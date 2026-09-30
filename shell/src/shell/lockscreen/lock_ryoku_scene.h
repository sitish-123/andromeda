#pragma once

#include "render/animation/animation_manager.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

class Label;
class Node;
class RectNode;
class Renderer;

// The Ryoku lock screen, matching the greeter's Ryoku login (greeter/src/greeter/ryoku_scene.*): black, the minute
// and second dial turning with the time beside the hour, the date, session actions at the top right, and the user,
// hint, and star-masked password row at the bottom right. LockSurface keeps its real password field (invisible, over
// the mask row) and all auth state; this class only draws and animates.
class LockRyokuScene {
public:
  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
  };
  struct Action {
    std::string label;
    std::function<void()> run;
  };

  void build(Node& root, AnimationManager& animations);
  void layout(Renderer& renderer, float sw, float sh);
  // Turns the rings to the current time: a smooth sweep when smooth, else whole seconds (the idle clock, drawn once
  // a second). True when the hour or date text changed and needs a layout.
  bool tick(bool smooth);

  void setUser(const std::string& name);
  void setHint(const std::string& text, bool isError, bool scanning);
  void setPasswordLength(std::size_t length);
  void setActions(std::vector<Action> actions);

  void playRejected();
  // Unlocked: the dial blasts outward under a white flash, then onDone.
  void playGranted(std::function<void()> onDone);

  [[nodiscard]] Rect passwordRow() const noexcept { return m_passwordRow; }

private:
  struct Ring {
    Node* container = nullptr;
    std::array<RectNode*, 60> ticks{};
    std::array<Label*, 12> numbers{};
    float radius = 0.0F;
    float tickLong = 0.0F;
    float tickShort = 0.0F;
    float numberSize = 0.0F;
  };

  void buildRing(Ring& ring, float tickLong, float tickShort, float numberSize);
  void layoutRing(Renderer& renderer, Ring& ring, float cx, float cy, float numberInset);
  void spotlight(Ring& ring, float ringAngle);
  void fadeColor(Label* label, bool lit);

  AnimationManager* m_animations = nullptr;
  float m_s = 1.0F;
  float m_ringCx = 0.0F;
  float m_ringCy = 0.0F;
  float m_viewH = 0.0F;
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
  Label* m_hint = nullptr;
  Label* m_mask = nullptr;
  Label* m_waiting = nullptr;
  RectNode* m_needle = nullptr;
  std::vector<Label*> m_actionLabels;
  std::vector<std::string> m_actionNames;
  Rect m_passwordRow;
  std::string m_hourText;
  std::string m_dateText;
  std::size_t m_passwordLength = 0;
  bool m_scanning = false;
  float m_introOffset = 0.9F;
  float m_shake = 0.0F;
  float m_waitingOpacity = 0.4F;
  AnimationManager::Id m_waitingAnim = 0;
};

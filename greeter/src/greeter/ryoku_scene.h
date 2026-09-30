#pragma once

#include <array>
#include <functional>
#include <string>

class Label;
class Node;
class RectNode;
class Renderer;

// The Ryoku look (ported from the Ryoku SDDM theme): black, a half-visible dial of minute and second rings turning
// with the time, the hour in Outfit Black beside a pill, the date, and a login column at the bottom right. The surface
// keeps its own password field (invisible, over the mask row) and state; this class only draws.
class RyokuScene {
public:
  struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
  };

  void build(Node& root);
  void layout(Renderer& renderer, float ox, float oy, float sw, float sh);
  // Turns the rings to the current time; true when the hour or date text changed and needs a layout.
  bool tick();

  void setUserName(const std::string& name);
  void setHint(const std::string& text, bool isError);
  void setPasswordLength(std::size_t length);
  void setSessionName(const std::string& name);
  void setOnSession(std::function<void()> callback);
  void setOnReboot(std::function<void()> callback);
  void setOnShutdown(std::function<void()> callback);

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
  void spotlight(Ring& ring, float ringAngle);

  float m_s = 1.0f;
  Node* m_root = nullptr;
  RectNode* m_background = nullptr;
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
  Label* m_session = nullptr;
  Label* m_reboot = nullptr;
  Label* m_shutdown = nullptr;
  RectNode* m_needle = nullptr;
  Rect m_passwordRow;
  std::string m_hourText;
  std::string m_dateText;
  std::size_t m_passwordLength = 0;
  float m_ox = 0.0f;
  float m_oy = 0.0f;
  float m_sw = 0.0f;
  float m_sh = 0.0f;
};

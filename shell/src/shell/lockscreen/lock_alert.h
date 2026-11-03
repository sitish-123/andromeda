#pragma once

#include "ui/auth/auth_layout.h"

#include <string>

class Label;
class Node;
class RectNode;
class Renderer;

// Rulebook 9.10 alert on the lock screen (system-modal): 40 % black scrim, w 320, r 24, p 24, solid popover;
// title-3, body label_2, Cancel (default) and a destructive capsule side by side.
class LockAlert {
public:
  struct State {
    // Empty = no alert.
    std::string title;
    std::string body;
    std::string confirm;
    bool operator==(const State&) const = default;
  };
  enum class Hit { None, Cancel, Confirm, Inside };

  void build(Node& parent);
  void setState(State state);
  [[nodiscard]] bool shown() const noexcept { return !m_state.title.empty(); }
  void layout(Renderer& renderer, float sw, float sh, float scale);
  [[nodiscard]] Hit hitTest(float x, float y) const;

private:
  State m_state;
  Node* m_group = nullptr;
  RectNode* m_scrim = nullptr;
  RectNode* m_panel = nullptr;
  Label* m_title = nullptr;
  Label* m_body = nullptr;
  RectNode* m_cancel = nullptr;
  Label* m_cancelLabel = nullptr;
  RectNode* m_confirm = nullptr;
  Label* m_confirmLabel = nullptr;
  AuthRect m_panelRect;
  AuthRect m_cancelRect;
  AuthRect m_confirmRect;
};

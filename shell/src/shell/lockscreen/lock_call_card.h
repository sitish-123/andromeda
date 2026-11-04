#pragma once

#include "ui/auth/auth_layout.h"

#include <string>

class Glyph;
class Label;
class Node;
class RectNode;
class Renderer;

// Rulebook 9.15 call card: a ringing phone call on the lock screen. Thick glass, w 400, r 24, p 16; avatar 48 on
// fill_2, "Incoming call" + the device (the caller only when notification content is shown); "Decline" (danger, lock
// symbol: it asks for the password first) and "Answer on phone" (success; the phone answers, the desktop cannot).
class LockCallCard {
public:
  struct State {
    // Empty = no ringing call.
    std::string deviceId;
    std::string title;
    std::string detail;
    // The user pressed Decline: the hint asks for the password; the call is declined after unlock.
    bool declinePending = false;
    bool operator==(const State&) const = default;
  };

  void build(Node& parent);
  void setState(State state);
  [[nodiscard]] const State& state() const noexcept { return m_state; }
  // `bottom` = the y the card ends at (32 above the account column); centred on `cx`.
  void layout(Renderer& renderer, float cx, float bottom, float scale);
  [[nodiscard]] bool declineHit(float x, float y) const;

  // Card height at scale 1 with the 32 gap above the account column (for AuthPlacementInput).
  static constexpr float kBlockHeight = 32.0F + 16.0F + 48.0F + 16.0F + 40.0F + 16.0F;

private:
  State m_state;
  Node* m_group = nullptr;
  RectNode* m_bg = nullptr;
  RectNode* m_avatar = nullptr;
  Glyph* m_avatarGlyph = nullptr;
  Label* m_title = nullptr;
  Label* m_detail = nullptr;
  RectNode* m_decline = nullptr;
  Glyph* m_declineGlyph = nullptr;
  Label* m_declineLabel = nullptr;
  RectNode* m_answer = nullptr;
  Label* m_answerLabel = nullptr;
  AuthRect m_declineRect;
};

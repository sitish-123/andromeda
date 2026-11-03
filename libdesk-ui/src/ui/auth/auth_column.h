#pragma once

#include "render/animation/animation_manager.h"

#include <cstddef>
#include <string>

class Glyph;
class Label;
class Node;
class RectNode;
class Renderer;

// The Andromeda auth area (rulebook 9.15): avatar, name, password capsule, hint and owner message, plus the
// lock-from-phone reason card, for the lock screen and the greeter. The host keeps the real password field (invisible)
// and the auth state; this class only draws. The column never moves between states; only the capsule and the hint
// change.
class AuthColumn {
public:
  enum class Trailing { None, Face, Fingerprint, Go, Checking };

  // Status island (rulebook 9.15, top right): battery, network, keyboard layout.
  struct Status {
    std::string batteryGlyph;
    std::string batteryText;
    std::string networkGlyph;
    std::string layoutLabel;
    bool operator==(const Status&) const = default;
  };

  struct State {
    std::string user;
    std::size_t passwordLength = 0;
    // "Password", or the fail-delay countdown.
    std::string placeholder;
    // Fail delay and PAM error: the capsule dims to 60 %.
    bool inputOff = false;
    Trailing trailing = Trailing::None;
    std::string hint;
    bool hintError = false;
    std::string ownerMessage;
    // Empty = no reason card.
    std::string reasonTitle;
    std::string reasonBody;
    // Centered layout clock (rulebook 9.15): hero time, title-3 date 8 above it.
    std::string time;
    std::string date;
    Status status;

    bool operator==(const State&) const = default;
  };

  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
  };

  void build(Node& root, AnimationManager& animations);
  void setState(State state);
  // scale = accessibility.ui_scale.
  void layout(Renderer& renderer, float sw, float sh, float scale);
  // Wrong password: the capsule shakes 3 x 8 px on the snap spring.
  void playRejected();

  [[nodiscard]] Rect capsule() const noexcept { return m_capsuleRect; }

private:
  AnimationManager* m_animations = nullptr;
  State m_state;
  Node* m_group = nullptr;
  RectNode* m_avatar = nullptr;
  Label* m_initial = nullptr;
  Label* m_name = nullptr;
  Node* m_capsuleGroup = nullptr;
  RectNode* m_capsule = nullptr;
  Label* m_field = nullptr;
  RectNode* m_go = nullptr;
  Glyph* m_goGlyph = nullptr;
  Glyph* m_trailingGlyph = nullptr;
  Label* m_hint = nullptr;
  Label* m_owner = nullptr;
  RectNode* m_card = nullptr;
  Glyph* m_cardGlyph = nullptr;
  Label* m_cardTitle = nullptr;
  Label* m_cardBody = nullptr;
  RectNode* m_island = nullptr;
  Glyph* m_batteryGlyph = nullptr;
  Label* m_batteryText = nullptr;
  Glyph* m_networkGlyph = nullptr;
  Label* m_layoutText = nullptr;
  Label* m_date = nullptr;
  Label* m_clock = nullptr;
  Rect m_capsuleRect;
  float m_shakeAmplitude = 8.0F;
};

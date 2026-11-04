#pragma once

#include "render/animation/animation_manager.h"
#include "ui/auth/analog_clock.h"
#include "ui/auth/auth_layout.h"
#include "ui/auth/flip_clock.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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
    // Lock screen charging chip ("Charging · full in 1 h 20 min"); empty while not charging. The island ignores it.
    std::string chargingText;
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
    // Clock style (clock_style.h): one entry per line; empty = `time` on one line in the hero font.
    std::vector<std::string> clockLines;
    std::string clockFamily;
    int clockWeight = 300;
    // Letter spacing in em (the wide style).
    float clockTracking = 0.0F;
    // Outline width in em (the outline style); 0 = filled.
    float clockStroke = 0.0F;
    // Extra font axes ("wdth=75").
    std::string clockVariations;
    // Gradient ink, packed 0xRRGGBBAA; both 0 = solid label color.
    std::uint32_t clockGradientFrom = 0;
    std::uint32_t clockGradientTo = 0;
    // Analog style: draw the dial at this time instead of the text lines; hour < 0 = text clock.
    int analogHour = -1;
    int analogMinute = 0;
    // Flip style: the first line's "hh:mm" on two cards.
    bool flip = false;
    // Live style: small seconds after the first line (mockup: 20 px at a 64 px clock, label_2); empty = none.
    std::string liveSeconds;
    // Two-tone: the part after the first ':' in accent_text.
    bool clockTwoTone = false;
    std::string date;
    // Glance layout: the line 72 above the bottom while the auth sheet is closed.
    std::string glanceHint;
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
  // Centered layout; scale = accessibility.ui_scale.
  void layout(Renderer& renderer, float sw, float sh, float scale);
  // place = placeAuth() for the same sw, sh and scale.
  void layout(Renderer& renderer, const AuthPlacement& place, float sw, float scale);
  // Wrong password: the capsule shakes 3 x 8 px on the snap spring.
  void playRejected();
  // Login accepted: the column (and `alongside`, the chrome) Return on the close spring (scale 0.92, fade); reduced
  // motion fades only. `done` runs once it settled.
  void playAccepted(Node* alongside, std::function<void()> done);
  // Glance layout: the auth sheet Grows from the centre (open spring) or Returns (close spring); instant = no motion.
  void setSheetOpen(bool open, bool instant = false);
  [[nodiscard]] bool sheetOpen() const noexcept { return m_sheetOpen; }

  [[nodiscard]] Rect capsule() const noexcept { return m_capsuleRect; }
  // The layer that recedes to 40 % while the Glance sheet is open; host cards attach here.
  [[nodiscard]] Node& glanceLayer() noexcept { return *m_glanceGroup; }
  // Avatar, name and capsule (the lock screen editor dims it).
  [[nodiscard]] Node& columnLayer() noexcept { return *m_columnGroup; }

private:
  void applySheet();

  AnimationManager* m_animations = nullptr;
  State m_state;
  AuthPlacement m_place;
  bool m_sheetOpen = false;
  bool m_accepted = false;
  // 0 = sheet closed, 1 = open.
  float m_sheetT = 0.0F;
  AnimationManager::Id m_sheetAnim = 0;
  Node* m_group = nullptr;
  // Clock, date and glance hint: recede to 40 % while the Glance sheet is open.
  Node* m_glanceGroup = nullptr;
  Label* m_glanceHint = nullptr;
  // Sheet, reason card and account column: fade in with the sheet in Glance.
  Node* m_columnGroup = nullptr;
  RectNode* m_sheet = nullptr;
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
  // Clock lines (stacked and word styles use up to three); the accent label holds the two-tone minutes.
  std::array<Label*, 3> m_clockLines{};
  Label* m_clockAccent = nullptr;
  Label* m_clockSeconds = nullptr;
  AnalogClock m_analog;
  FlipClock m_flip;
  Rect m_capsuleRect;
  float m_shakeAmplitude = 8.0F;
};

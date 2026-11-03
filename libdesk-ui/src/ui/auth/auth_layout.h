#pragma once

#include <string_view>

// Rulebook 9.15 layouts: where the clock block, the card slots, the auth column and the chrome go on one output.
// Values are logical pixels; sw / sh = output size, scale = accessibility.ui_scale.
enum class AuthLayout { Centered, Glance, Split, Minimal };

// Unknown names = Centered (the default layout).
[[nodiscard]] AuthLayout authLayoutFromName(std::string_view name) noexcept;

struct AuthRect {
  float x = 0.0F;
  float y = 0.0F;
  float w = 0.0F;
  float h = 0.0F;
  [[nodiscard]] bool empty() const noexcept { return w <= 0.0F || h <= 0.0F; }
  [[nodiscard]] float bottom() const noexcept { return y + h; }
  [[nodiscard]] float right() const noexcept { return x + w; }
};

struct AuthPlacementInput {
  AuthLayout layout = AuthLayout::Centered;
  float sw = 0.0F;
  float sh = 0.0F;
  float scale = 1.0F;
  // Slot content present (rulebook 9.15 cards); an empty slot takes no room.
  bool media = false;
  bool chips = false;
  // Glance card column and Split widget row: number of entries.
  int cards = 0;
  // Centered widget row (lockscreen.widgets): number of widgets, 0 = no row.
  int widgets = 0;
  // lockscreen.clock_size at scale 1 (64 / 80 / 96 / 120); 0 = the layout's own size. Lines: 2 or 3 for stacked
  // and word styles.
  float clockSize = 0.0F;
  int clockLines = 1;
  // Minimal: the island and session button show only after the pointer moves.
  bool pointerMoved = false;
  // Lock from phone: the reason card sits 32 above the avatar and takes room from the content above.
  bool reasonCard = false;
  // A ringing phone call: its card (block height at scale 1, gap included) replaces the reason card above the account.
  float callBlock = 0.0F;
};

struct AuthPlacement {
  AuthLayout layout = AuthLayout::Centered;
  // False when even the bare clock would run into the account column (Centered at a large ui_scale).
  bool clock = true;
  // Clock block. Centered and Minimal centre the text on clockX; Glance and Split start it at clockX.
  bool clockCentred = true;
  float clockX = 0.0F;
  // Top of the clock block: clockLines lines, each clockSize high.
  float clockY = 0.0F;
  float clockSize = 80.0F;
  int clockLines = 1;
  [[nodiscard]] float clockHeight() const noexcept { return clockSize * static_cast<float>(clockLines); }
  // Date line (title-3), 8 above the clock, or 8 below it in label_2 (Minimal).
  AuthRect date;
  bool dateSecondary = false;
  // Centered: widget row, media (bottom slot) card, then the chips row. Glance: card column. Split: widget row (cards).
  // Empty = no slot.
  AuthRect widgets;
  AuthRect media;
  AuthRect chips;
  AuthRect cards;
  // Auth column: avatar + name show unless Minimal; the capsule is the fixed anchor of the column.
  bool account = true;
  float columnCx = 0.0F;
  AuthRect capsule;
  // Bottom of the reason or call card: 32 above the avatar (or the capsule in Minimal).
  float cardBottom = 0.0F;
  // Glance: the auth sheet behind the column (thick glass, r 28); empty in the other layouts.
  AuthRect sheet;
  // Glance: "Press any key or look at the camera to unlock", centred, 72 from the bottom (top of the line).
  float glanceHintY = 0.0F;
  bool chrome = true;
  AuthRect sessionButton;
};

[[nodiscard]] AuthPlacement placeAuth(const AuthPlacementInput& in) noexcept;

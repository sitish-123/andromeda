#include "ui/auth/auth_layout.h"

#include <algorithm>
#include <cmath>

namespace {

  // Rulebook 9.15 values at scale 1.
  constexpr float kAvatar = 64.0F;
  constexpr float kNameLine = 20.0F;
  constexpr float kFootLine = 16.0F;
  constexpr float kCapsuleW = 240.0F;
  constexpr float kCapsuleH = 40.0F;
  // Avatar, g 12, name, g 16: the part of the column above the capsule.
  constexpr float kAccountHead = kAvatar + 12.0F + kNameLine + 16.0F;
  // Capsule, g 12, hint line: the hint keeps its line when empty, so the column never moves.
  constexpr float kAccountFoot = kCapsuleH + 12.0F + kFootLine;
  constexpr float kDateLine = 20.0F;
  constexpr float kDateGap = 8.0F;
  constexpr float kCardH = 64.0F;
  constexpr float kCardGap = 8.0F;
  constexpr float kChipH = 24.0F;
  constexpr float kWidgetW = 112.0F;
  constexpr float kWidgetH = 56.0F;
  constexpr float kEdge = 96.0F;
  // Reason card (lock from phone): 32 above the avatar, p 16, title-3 line + footnote line.
  constexpr float kReasonCard = 32.0F + 16.0F * 2.0F + kNameLine + kFootLine;
  // Smallest gap between the content above and the account column (Centered).
  constexpr float kMinGap = 24.0F;
  // Top of the clock block may not go above the status island (top 8, h 32) plus 16.
  constexpr float kMinTop = 8.0F + 32.0F + 16.0F;
  // Split: estimated "00:00" width per font pixel (tabular figures, light weight).
  constexpr float kClockWidthPerSize = 3.0F;
  // Split: the hint and owner message may be 320 wide around the column centre.
  constexpr float kColumnTextHalf = 160.0F;

  float snap(float v) { return std::round(v); }

  AuthRect rect(float x, float y, float w, float h) { return {snap(x), snap(y), snap(w), snap(h)}; }

  float stackHeight(int count, float item, float gap) {
    return count <= 0 ? 0.0F : static_cast<float>(count) * item + static_cast<float>(count - 1) * gap;
  }

  void placeChrome(AuthPlacement& p, const AuthPlacementInput& in, float s) {
    p.sessionButton = rect(in.sw - (24.0F + 40.0F) * s, in.sh - (24.0F + 40.0F) * s, 40.0F * s, 40.0F * s);
  }

  // Room the reason or call card takes above the account column, gap included.
  float aboveAccount(const AuthPlacementInput& in) {
    return in.callBlock > 0.0F ? in.callBlock : in.reasonCard ? kReasonCard : 0.0F;
  }

  void placeCardBottom(AuthPlacement& p, float s) {
    p.cardBottom = p.capsule.y - (p.account ? kAccountHead : 0.0F) * s - 32.0F * s;
  }

  AuthPlacement placeCentered(const AuthPlacementInput& in, float s) {
    AuthPlacement p;
    p.layout = AuthLayout::Centered;
    const float cx = in.sw * 0.5F;
    p.columnCx = cx;
    p.capsule = rect(cx - kCapsuleW * 0.5F * s, in.sh * 0.75F - kCapsuleH * 0.5F * s, kCapsuleW * s, kCapsuleH * s);
    const float accountTop = p.capsule.y - (kAccountHead + aboveAccount(in)) * s;

    p.clockSize = 80.0F * s;
    p.clockX = cx;
    bool media = in.media;
    bool chips = in.chips;
    // The account column is fixed; the content above yields: it moves up, then drops the chips, then the card.
    for (;;) {
      float top = snap(in.sh * 0.25F);
      float h = p.clockSize;
      if (media) {
        h += (20.0F + kCardH) * s;
      }
      if (chips) {
        h += ((media ? kCardGap : 20.0F) + kChipH) * s;
      }
      const float limit = accountTop - kMinGap * s;
      if (top + h > limit) {
        top = std::max((kMinTop + kDateLine + kDateGap) * s, limit - h);
      }
      if (top + h <= limit || (!media && !chips)) {
        p.clockY = snap(top);
        p.clock = top + h <= limit;
        break;
      }
      (chips ? chips : media) = false;
    }
    p.date = rect(0.0F, p.clockY - (kDateGap + kDateLine) * s, in.sw, kDateLine * s);
    float y = p.clockY + p.clockSize;
    if (media) {
      p.media = rect(cx - 180.0F * s, y + 20.0F * s, 360.0F * s, kCardH * s);
      y = p.media.bottom();
    }
    if (chips) {
      p.chips = rect(cx - 180.0F * s, y + (media ? kCardGap : 20.0F) * s, 360.0F * s, kChipH * s);
    }
    placeChrome(p, in, s);
    placeCardBottom(p, s);
    return p;
  }

  AuthPlacement placeGlance(const AuthPlacementInput& in, float s) {
    AuthPlacement p;
    p.layout = AuthLayout::Glance;
    const float cx = in.sw * 0.5F;
    p.clockCentred = false;
    p.clockX = snap(kEdge * s);
    p.clockSize = 80.0F * s;
    p.date = rect(kEdge * s, 120.0F * s, 400.0F * s, kDateLine * s);
    p.clockY = p.date.bottom() + snap(kDateGap * s);
    if (in.cards > 0) {
      p.cards = rect(kEdge * s, 252.0F * s, 400.0F * s, stackHeight(in.cards, kCardH, kCardGap) * s);
    }
    // Sheet: p 32 / 24 / 24 around the column; the capsule keeps its place in the column.
    const float sheetH = (32.0F + kAccountHead + kAccountFoot + 24.0F) * s;
    p.sheet = rect(cx - 180.0F * s, in.sh * 0.5F - sheetH * 0.5F, 360.0F * s, sheetH);
    p.columnCx = cx;
    p.capsule = rect(cx - kCapsuleW * 0.5F * s, p.sheet.y + (32.0F + kAccountHead) * s, kCapsuleW * s, kCapsuleH * s);
    p.glanceHintY = snap(in.sh - (72.0F + kFootLine) * s);
    placeChrome(p, in, s);
    placeCardBottom(p, s);
    return p;
  }

  AuthPlacement placeSplit(const AuthPlacementInput& in, float s) {
    AuthPlacement p;
    p.layout = AuthLayout::Split;
    p.clockCentred = false;
    p.clockX = snap(kEdge * s);
    p.clockSize = 96.0F * s;
    const float rowH = in.cards > 0 ? (24.0F + kWidgetH) * s : 0.0F;
    const float blockH = (kDateLine + kDateGap) * s + p.clockSize + rowH;
    const float blockTop = snap(in.sh * 0.5F - blockH * 0.5F);
    p.date = rect(kEdge * s, blockTop, 400.0F * s, kDateLine * s);
    p.clockY = p.date.bottom() + snap(kDateGap * s);
    if (in.cards > 0) {
      p.cards = rect(kEdge * s, p.clockY + p.clockSize + 24.0F * s, stackHeight(in.cards, kWidgetW, kCardGap) * s, kWidgetH * s);
    }
    p.capsule = rect(in.sw - (160.0F + kCapsuleW) * s, 0.0F, kCapsuleW * s, kCapsuleH * s);
    p.columnCx = p.capsule.x + p.capsule.w * 0.5F;
    const float columnTop = snap(in.sh * 0.5F - (kAccountHead + kAccountFoot) * s * 0.5F);
    p.capsule.y = columnTop + snap(kAccountHead * s);

    const float leftRight = std::max(p.clockX + kClockWidthPerSize * p.clockSize, p.cards.right());
    const bool cardFits = columnTop - aboveAccount(in) * s >= kMinTop * s;
    if (leftRight + kMinGap * s > p.columnCx - kColumnTextHalf * s || !cardFits) {
      return placeCentered(in, s);
    }
    placeChrome(p, in, s);
    placeCardBottom(p, s);
    return p;
  }

  AuthPlacement placeMinimal(const AuthPlacementInput& in, float s) {
    AuthPlacement p;
    p.layout = AuthLayout::Minimal;
    const float cx = in.sw * 0.5F;
    p.clockX = cx;
    p.clockSize = 96.0F * s;
    p.clockY = snap(in.sh * 0.4F);
    p.date = rect(0.0F, p.clockY + p.clockSize + kDateGap * s, in.sw, kDateLine * s);
    p.dateSecondary = true;
    p.account = false;
    p.columnCx = cx;
    p.capsule = rect(cx - kCapsuleW * 0.5F * s, in.sh - (96.0F + kCapsuleH) * s, kCapsuleW * s, kCapsuleH * s);
    p.chrome = in.pointerMoved;
    placeChrome(p, in, s);
    placeCardBottom(p, s);
    return p;
  }

} // namespace

AuthLayout authLayoutFromName(std::string_view name) noexcept {
  if (name == "glance") {
    return AuthLayout::Glance;
  }
  if (name == "split") {
    return AuthLayout::Split;
  }
  if (name == "minimal") {
    return AuthLayout::Minimal;
  }
  return AuthLayout::Centered;
}

AuthPlacement placeAuth(const AuthPlacementInput& in) noexcept {
  const float s = std::max(0.1F, in.scale);
  switch (in.layout) {
  case AuthLayout::Glance:
    return placeGlance(in, s);
  case AuthLayout::Split:
    return placeSplit(in, s);
  case AuthLayout::Minimal:
    return placeMinimal(in, s);
  case AuthLayout::Centered:
    break;
  }
  return placeCentered(in, s);
}

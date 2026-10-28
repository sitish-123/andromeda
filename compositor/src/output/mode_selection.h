#pragma once

#include "config/value_parse.h"

#include <cstdint>

struct wlr_output;
struct wlr_output_mode;

namespace andromeda {

  // Whether an output that has no matching config section exposes enough
  // information for Andromeda to choose a safe initial mode automatically.
  [[nodiscard]] bool outputCanAutoEnable(wlr_output* output);

  // The advertised mode a configured `mode` names, or nullptr when the output advertises no mode at that resolution.
  // A configured refresh picks the closest advertised one; without a refresh the preferred mode wins, then the
  // highest refresh.
  [[nodiscard]] wlr_output_mode* selectOutputMode(wlr_output* output, const OutputMode& configured);

  // The mode to enable an unconfigured output with: the highest refresh at the preferred mode's resolution, since a TV
  // can prefer 4K@30 while offering 4K@60. nullptr when the output advertises no modes.
  [[nodiscard]] wlr_output_mode* fastestPreferredMode(wlr_output* output);

  // The mode to retry with after `staged` failed to commit, or nullptr when the output offers no other one. `staged`
  // is null for a custom mode, which the preferred advertised mode still replaces.
  [[nodiscard]] wlr_output_mode* preferredFallbackMode(wlr_output* output, const wlr_output_mode* staged);

  // Whether `output` is lit at `mode` already. Staging it again would only force a modeset.
  [[nodiscard]] bool liveModeMatches(const wlr_output& output, const wlr_output_mode* mode);

  // Whether `enabled` has to be staged. Re-enabling a lit output would only force a modeset.
  [[nodiscard]] bool outputEnableNeedsStaging(const wlr_output& output, bool enabled);

  // Whether committing `renderFormat` with HDR on or off changes the live render format or HDR state, which may need
  // a modeset when the live mode is kept.
  [[nodiscard]] bool colorChangeNeedsModeset(const wlr_output& output, uint32_t renderFormat, bool hdr);

} // namespace andromeda

#pragma once

#include "notification/focus.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Focus triggers (docs/focus.md §1, §6 F3): what starts and ends a Focus without the user.
namespace focus {

  enum class TriggerKind : std::uint8_t { Schedule, Fullscreen, Screencast, Output, Wifi };

  struct Trigger {
    std::string name; // config table name; startedBy is "trigger:<name>"
    std::string focusId;
    TriggerKind kind = TriggerKind::Schedule;
    std::uint8_t days = 0x7F; // bit 0 = Sunday .. bit 6 = Saturday; a schedule's start day
    int startMinute = 0;      // minutes after local midnight; end <= start runs past midnight
    int endMinute = 0;
    std::string match; // Fullscreen: app id (empty: any app); Output: connector; Wifi: SSID
  };

  struct TriggerInputs {
    int weekday = 0;            // 0 = Sunday, local time
    int minute = 0;             // minutes after local midnight
    std::optional<std::string> fullscreenApp; // nullopt: no fullscreen window
    bool screencast = false;
    std::vector<std::string> outputs;
    std::string ssid;
  };

  struct TriggerMatch {
    std::string focusId;
    std::string startedBy;
    bool operator==(const TriggerMatch&) const = default;
  };

  // The Focus the triggers want on. Event triggers (fullscreen, screencast, output, Wi-Fi) win over schedules, in
  // config order; among schedules the later start wins (docs/focus.md §1).
  [[nodiscard]] std::optional<TriggerMatch> wanted(std::span<const Trigger> triggers, const TriggerInputs& in);

  // Minutes from `in` until a schedule may change wanted(), at most one week; nullopt without schedules.
  [[nodiscard]] std::optional<int> minutesToScheduleChange(std::span<const Trigger> triggers, TriggerInputs in);

  // The trigger layer's own state: the Focus it started, and a trigger the user ended while it still matched (it stays
  // off until the trigger stops matching).
  struct TriggerState {
    std::optional<std::string> applied;   // startedBy of the Focus the triggers started
    std::optional<std::string> dismissed; // startedBy the user ended
  };

  enum class TriggerAction : std::uint8_t { None, Start, End };

  struct TriggerStep {
    TriggerAction action = TriggerAction::None;
    TriggerMatch match; // for Start
  };

  // One transition. A Focus the user (or a rule) started is never replaced or ended here.
  [[nodiscard]] TriggerStep arbitrate(
      TriggerState& state, const std::optional<TriggerMatch>& want, const std::optional<Active>& active
  );

} // namespace focus

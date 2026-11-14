#include "notification/focus_triggers.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace focus {

  namespace {

    constexpr int kDay = 24 * 60;
    constexpr Logger kLog("focus");

    std::optional<int> parseMinute(std::string_view hhmm) {
      int h = 0;
      int m = 0;
      if (hhmm.size() != 5 || hhmm[2] != ':'
          || std::from_chars(hhmm.data(), hhmm.data() + 2, h).ptr != hhmm.data() + 2
          || std::from_chars(hhmm.data() + 3, hhmm.data() + 5, m).ptr != hhmm.data() + 5 || h > 23 || m > 59) {
        return std::nullopt;
      }
      return h * 60 + m;
    }

    std::optional<TriggerKind> parseKind(std::string_view kind) {
      constexpr std::array<std::pair<std::string_view, TriggerKind>, 5> kKinds{{
          {"schedule", TriggerKind::Schedule},
          {"fullscreen", TriggerKind::Fullscreen},
          {"screencast", TriggerKind::Screencast},
          {"output", TriggerKind::Output},
          {"wifi", TriggerKind::Wifi},
      }};
      const auto* it = std::ranges::find(kKinds, kind, &std::pair<std::string_view, TriggerKind>::first);
      return it != kKinds.end() ? std::optional(it->second) : std::nullopt;
    }

    std::optional<std::uint8_t> parseDays(const std::vector<std::string>& days) {
      constexpr std::array<std::string_view, 7> kDays{"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
      if (days.empty()) {
        return 0x7F;
      }
      std::uint8_t mask = 0;
      for (const std::string& day : days) {
        const auto* it = std::ranges::find(kDays, day);
        if (it == kDays.end()) {
          return std::nullopt;
        }
        mask |= static_cast<std::uint8_t>(1U << (it - kDays.begin()));
      }
      return mask;
    }

    bool onDay(std::uint8_t days, int weekday) { return (days >> (((weekday % 7) + 7) % 7) & 1U) != 0; }

    // Minutes since the schedule started when it is on at `in`, else nullopt.
    std::optional<int> scheduleAge(const Trigger& t, const TriggerInputs& in) {
      if (t.endMinute > t.startMinute) {
        if (onDay(t.days, in.weekday) && in.minute >= t.startMinute && in.minute < t.endMinute) {
          return in.minute - t.startMinute;
        }
        return std::nullopt;
      }
      if (onDay(t.days, in.weekday) && in.minute >= t.startMinute) {
        return in.minute - t.startMinute;
      }
      if (onDay(t.days, in.weekday - 1) && in.minute < t.endMinute) {
        return in.minute + kDay - t.startMinute;
      }
      return std::nullopt;
    }

    bool eventOn(const Trigger& t, const TriggerInputs& in) {
      switch (t.kind) {
      case TriggerKind::Fullscreen:
        return in.fullscreenApp.has_value() && (t.match.empty() || *in.fullscreenApp == t.match);
      case TriggerKind::Screencast:
        return in.screencast;
      case TriggerKind::Output:
        return std::ranges::contains(in.outputs, t.match);
      case TriggerKind::Wifi:
        return !t.match.empty() && in.ssid == t.match;
      case TriggerKind::Schedule:
        return false;
      }
      return false;
    }

    TriggerMatch matchOf(const Trigger& t) { return {.focusId = t.focusId, .startedBy = "trigger:" + t.name}; }

  } // namespace

  std::vector<Trigger> triggersFromConfig(std::span<const FocusTriggerConfig> config) {
    std::vector<Trigger> out;
    for (const FocusTriggerConfig& c : config) {
      const auto kind = parseKind(c.kind);
      const auto days = parseDays(c.days);
      const bool schedule = kind == TriggerKind::Schedule;
      const auto start = schedule ? parseMinute(c.start) : std::optional(0);
      const auto end = schedule ? parseMinute(c.end) : std::optional(0);
      if (c.focus.empty() || !kind || !days || !start || !end || (kind == TriggerKind::Output && c.match.empty())
          || (kind == TriggerKind::Wifi && c.match.empty())) {
        kLog.warn("focus trigger \"{}\" skipped: needs focus, a known kind, days sun..sat, start/end HH:MM, match",
                  c.name);
        continue;
      }
      out.push_back({.name = c.name, .focusId = c.focus, .kind = *kind, .days = *days, .startMinute = *start,
                     .endMinute = *end, .match = c.match});
    }
    return out;
  }

  std::optional<TriggerMatch> wanted(std::span<const Trigger> triggers, const TriggerInputs& in) {
    for (const Trigger& t : triggers) {
      if (t.kind != TriggerKind::Schedule && eventOn(t, in)) {
        return matchOf(t);
      }
    }
    const Trigger* latest = nullptr;
    int youngest = 0;
    for (const Trigger& t : triggers) {
      if (t.kind != TriggerKind::Schedule) {
        continue;
      }
      if (const auto age = scheduleAge(t, in); age.has_value() && (latest == nullptr || *age < youngest)) {
        latest = &t;
        youngest = *age;
      }
    }
    return latest != nullptr ? std::optional(matchOf(*latest)) : std::nullopt;
  }

  std::optional<int> minutesToScheduleChange(std::span<const Trigger> triggers, TriggerInputs in) {
    if (std::ranges::none_of(triggers, [](const Trigger& t) { return t.kind == TriggerKind::Schedule; })) {
      return std::nullopt;
    }
    const auto now = wanted(triggers, in);
    for (int step = 1; step <= 7 * kDay; ++step) {
      if (++in.minute == kDay) {
        in.minute = 0;
        in.weekday = (in.weekday + 1) % 7;
      }
      const bool edge = std::ranges::any_of(triggers, [&in](const Trigger& t) {
        return t.kind == TriggerKind::Schedule && (t.startMinute == in.minute || t.endMinute == in.minute);
      });
      if (edge && wanted(triggers, in) != now) {
        return step;
      }
    }
    return 7 * kDay;
  }

  TriggerStep arbitrate(
      TriggerState& state, const std::optional<TriggerMatch>& want, const std::optional<Active>& active
  ) {
    // Our Focus is gone or replaced: the user ended it, so that trigger stays off while it matches.
    if (state.applied.has_value() && (!active.has_value() || active->startedBy != *state.applied)) {
      state.dismissed = std::move(state.applied);
      state.applied.reset();
    }
    if (state.dismissed.has_value() && (!want.has_value() || want->startedBy != *state.dismissed)) {
      state.dismissed.reset();
    }
    const bool ours = active.has_value() && state.applied.has_value() && active->startedBy == *state.applied;
    if (active.has_value() && !ours) {
      return {};
    }
    const bool usable = want.has_value() && want->startedBy != state.dismissed;
    if (usable) {
      if (ours && active->focusId == want->focusId && *state.applied == want->startedBy) {
        return {};
      }
      state.applied = want->startedBy;
      return {.action = TriggerAction::Start, .match = *want};
    }
    if (ours) {
      state.applied.reset();
      return {.action = TriggerAction::End};
    }
    return {};
  }

} // namespace focus

// Focus triggers (docs/focus.md §1): which Focus the triggers want, and the trigger layer's transitions.
#include "notification/focus_triggers.h"

#include <iostream>
#include <random>
#include <vector>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  using focus::TriggerKind;
  using focus::TriggerAction;

  focus::Trigger schedule(const char* name, const char* focusId, int start, int end, std::uint8_t days = 0x7F) {
    return {.name = name, .focusId = focusId, .kind = TriggerKind::Schedule, .days = days, .startMinute = start,
            .endMinute = end};
  }

  std::optional<std::string> wantedBy(const std::vector<focus::Trigger>& t, const focus::TriggerInputs& in) {
    const auto w = focus::wanted(t, in);
    return w ? std::optional(w->startedBy) : std::nullopt;
  }

} // namespace

int main() {
  bool ok = true;

  // Overnight schedule 22:00-07:00, Mondays only: on Monday night and the Tuesday morning after.
  {
    const std::vector t{schedule("sleep", "sleep", 22 * 60, 7 * 60, 1U << 1)};
    ok &= check(wantedBy(t, {.weekday = 1, .minute = 23 * 60}) == "trigger:sleep", "overnight: on Monday 23:00");
    ok &= check(wantedBy(t, {.weekday = 2, .minute = 6 * 60}) == "trigger:sleep", "overnight: on Tuesday 06:00");
    ok &= check(!wantedBy(t, {.weekday = 2, .minute = 7 * 60}), "overnight: off Tuesday 07:00");
    ok &= check(!wantedBy(t, {.weekday = 1, .minute = 6 * 60}), "overnight: off Monday 06:00 (Sunday not set)");
    ok &= check(focus::minutesToScheduleChange(t, {.weekday = 1, .minute = 21 * 60}) == 60,
                "overnight: next change in 60 min");
  }

  // Config tables: a bad kind, day or time is skipped; days and HH:MM become the mask and minutes.
  {
    const std::vector<FocusTriggerConfig> config{
        {.name = "sleep", .focus = "sleep", .days = {"mon", "tue"}, .start = "22:00", .end = "07:30"},
        {.name = "bad-kind", .focus = "x", .kind = "moon"},
        {.name = "bad-day", .focus = "x", .days = {"monday"}, .start = "09:00", .end = "10:00"},
        {.name = "bad-time", .focus = "x", .start = "9:00", .end = "10:00"},
        {.name = "cast", .focus = "present", .kind = "screencast"},
    };
    const auto t = focus::triggersFromConfig(config);
    ok &= check(t.size() == 2 && t[0].days == 0b110 && t[0].startMinute == 22 * 60 && t[0].endMinute == 7 * 60 + 30
                    && t[1].kind == TriggerKind::Screencast,
                "config: two valid tables parsed, three skipped");
  }

  // Overlapping schedules: the later start wins.
  {
    const std::vector t{schedule("work", "work", 9 * 60, 17 * 60), schedule("meet", "meet", 14 * 60, 15 * 60)};
    ok &= check(wantedBy(t, {.minute = 10 * 60}) == "trigger:work", "overlap: only work at 10:00");
    ok &= check(wantedBy(t, {.minute = 14 * 60 + 30}) == "trigger:meet", "overlap: later start wins at 14:30");
    ok &= check(wantedBy(t, {.minute = 16 * 60}) == "trigger:work", "overlap: work again at 16:00");
  }

  // Event triggers beat schedules; fullscreen matches any app or one app.
  {
    const std::vector t{
        schedule("work", "work", 0, 0),
        {.name = "game", .focusId = "gaming", .kind = TriggerKind::Fullscreen, .match = "steam"},
        {.name = "full", .focusId = "dnd", .kind = TriggerKind::Fullscreen},
        {.name = "cast", .focusId = "present", .kind = TriggerKind::Screencast},
    };
    ok &= check(wantedBy(t, {.minute = 600}) == "trigger:work", "event: schedule alone");
    ok &= check(wantedBy(t, {.minute = 600, .fullscreenApp = "steam"}) == "trigger:game", "event: fullscreen app");
    ok &= check(wantedBy(t, {.minute = 600, .fullscreenApp = "mpv"}) == "trigger:full", "event: fullscreen any");
    ok &= check(wantedBy(t, {.minute = 600, .screencast = true}) == "trigger:cast", "event: screencast beats schedule");
  }

  // The user ends a trigger Focus: it stays off until the trigger stops matching.
  {
    focus::TriggerState s;
    const focus::TriggerMatch want{.focusId = "work", .startedBy = "trigger:work"};
    const focus::Active on{.focusId = "work", .startedBy = "trigger:work"};
    ok &= check(focus::arbitrate(s, want, std::nullopt).action == TriggerAction::Start, "dismiss: first start");
    ok &= check(focus::arbitrate(s, want, on).action == TriggerAction::None, "dismiss: steady");
    ok &= check(focus::arbitrate(s, want, std::nullopt).action == TriggerAction::None, "dismiss: user ended it");
    ok &= check(focus::arbitrate(s, std::nullopt, std::nullopt).action == TriggerAction::None, "dismiss: unmatch");
    ok &= check(focus::arbitrate(s, want, std::nullopt).action == TriggerAction::Start, "dismiss: matches again");
    ok &= check(focus::arbitrate(s, std::nullopt, on).action == TriggerAction::End, "dismiss: trigger ends it");
    const focus::Active user{.focusId = "dnd", .startedBy = "user"};
    ok &= check(focus::arbitrate(s, want, user).action == TriggerAction::None, "user Focus is never replaced");
  }

  // Property: random sequences never touch a Focus the triggers did not start, never restart a dismissed trigger.
  {
    std::mt19937 rng(7);
    const std::vector<std::optional<focus::TriggerMatch>> wants{
        std::nullopt, focus::TriggerMatch{"a", "trigger:a"}, focus::TriggerMatch{"b", "trigger:b"}};
    for (int run = 0; run < 2000; ++run) {
      focus::TriggerState s;
      std::optional<focus::Active> active;
      for (int i = 0; i < 40; ++i) {
        switch (rng() % 4) {
        case 0:
          active.reset();
          break;
        case 1:
          active = focus::Active{.focusId = "dnd", .startedBy = "user"};
          break;
        default:
          break;
        }
        const auto& want = wants[rng() % wants.size()];
        const bool foreign = active && (!s.applied || active->startedBy != *s.applied);
        const auto step = focus::arbitrate(s, want, active);
        if (foreign && step.action != TriggerAction::None) {
          ok &= check(false, "property: acted on a Focus the triggers did not start");
          run = 2000;
          break;
        }
        if (step.action == TriggerAction::Start) {
          if (s.dismissed && *s.dismissed == step.match.startedBy) {
            ok &= check(false, "property: restarted a dismissed trigger");
            run = 2000;
            break;
          }
          active = focus::Active{.focusId = step.match.focusId, .startedBy = step.match.startedBy};
        } else if (step.action == TriggerAction::End) {
          active.reset();
        }
        if (s.applied && (!active || active->startedBy != *s.applied)) {
          ok &= check(false, "property: applied does not match the active Focus");
          run = 2000;
          break;
        }
      }
    }
  }

  return ok ? 0 : 1;
}

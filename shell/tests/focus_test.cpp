// Focus decisions (docs/focus.md §3), one check per rule, plus app identity order and the Focus lifetime.
#include "notification/focus.h"

#include <iostream>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  using focus::Verdict;

} // namespace

int main() {
  bool ok = true;
  focus::Focus work{.id = "work", .name = "Work", .apps = {"org.gnome.Calendar"}, .calls = focus::People::Contacts,
                    .repeatedCalls = true, .messages = focus::People::Favourites};
  const focus::Incoming chat{.app = "org.telegram.desktop"};

  // 1. No Focus: today's rules decide.
  ok &= check(focus::decide(nullptr, chat, false).verdict == Verdict::Rules, "rule 1: no Focus defers to the rules");

  // 2. Time-sensitive breaks through when the Focus allows it.
  for (const focus::Incoming& n : {focus::Incoming{.app = "x", .critical = true},
                                   focus::Incoming{.app = "x", .portalPriority = "urgent"},
                                   focus::Incoming{.app = "x", .portalCategory = "alarm.ringing"},
                                   focus::Incoming{.app = "x", .portalCategory = "call.incoming"},
                                   focus::Incoming{.app = "x", .portalCategory = "weather.warning.extreme"},
                                   focus::Incoming{.app = "x", .portalCategory = "cellbroadcast.danger.extreme"}}) {
    ok &= check(focus::decide(&work, n, false).verdict == Verdict::Show, "rule 2: time-sensitive shows");
  }
  ok &= check(focus::decide(&work, {.app = "x", .portalPriority = "high"}, false).verdict == Verdict::Hold,
              "rule 2: high priority is not time-sensitive");
  focus::Focus strict = work;
  strict.timeSensitive = false;
  ok &= check(focus::decide(&strict, {.app = "x", .critical = true}, false).verdict == Verdict::Hold,
              "rule 2: a Focus that turns time-sensitive off holds a critical notification");

  // 3. Link calls and messages follow the people rule.
  using focus::LinkKind;
  ok &= check(focus::decide(&work, {.app = "link", .link = LinkKind::Call, .contact = true}, false).verdict
                  == Verdict::Show,
              "rule 3: a contact's call shows under Contacts");
  ok &= check(focus::decide(&work, {.app = "link", .link = LinkKind::Call, .favourite = true}, false).verdict
                  == Verdict::Show,
              "rule 3: Contacts includes favourites");
  ok &= check(focus::decide(&work, {.app = "link", .link = LinkKind::Call}, false).verdict == Verdict::Hold,
              "rule 3: a stranger's call is held");
  focus::Focus noRepeat = work;
  noRepeat.repeatedCalls = false;
  ok &= check(focus::decide(&noRepeat, {.app = "link", .link = LinkKind::Call, .repeatedCall = true}, false).verdict
                  == Verdict::Hold,
              "rule 3: with repeated calls off, a repeated call is held");
  ok &= check(focus::decide(&work, {.app = "link", .link = LinkKind::Call, .repeatedCall = true}, false).verdict
                  == Verdict::Show,
              "rule 3: a repeated call breaks through");
  ok &= check(focus::decide(&work, {.app = "link", .link = LinkKind::Message, .contact = true}, false).verdict
                  == Verdict::Hold,
              "rule 3: a contact's message is held under Favourites");
  ok &= check(focus::decide(&work, {.app = "link", .link = LinkKind::Message, .favourite = true}, false).verdict
                  == Verdict::Show,
              "rule 3: a favourite's message shows");

  // 4. The app list: allow mode shows listed apps, deny mode shows unlisted ones.
  ok &= check(focus::decide(&work, {.app = "org.gnome.Calendar.desktop"}, false).verdict == Verdict::Show,
              "rule 4: an allowed app shows");
  focus::Focus deny = work;
  deny.appMode = focus::AppMode::Deny;
  ok &= check(focus::decide(&deny, chat, false).verdict == Verdict::Show, "rule 4: an app not denied shows");
  focus::Focus listedWithSuffix = work;
  listedWithSuffix.apps = {"org.gnome.Calendar.desktop"};
  ok &= check(focus::decide(&listedWithSuffix, {.app = "org.gnome.Calendar"}, false).verdict == Verdict::Show,
              "rule 4: a list entry with .desktop matches the app");
  ok &= check(focus::decide(&deny, {.app = "org.gnome.Calendar"}, false).verdict == Verdict::Hold,
              "rule 4: a denied app is held");

  // 5. Everything else is held.
  ok &= check(focus::decide(&work, chat, false).verdict == Verdict::Hold, "rule 5: an unlisted app is held");

  // 6. A shared screen hides the content of what shows, never of what is held.
  ok &= check(focus::decide(nullptr, chat, true).hideContent, "rule 6: shared screen hides content with no Focus");
  ok &= check(focus::decide(&work, {.app = "org.gnome.Calendar"}, true).hideContent,
              "rule 6: shared screen hides a shown notification's content");
  ok &= check(!focus::decide(&work, chat, true).hideContent, "rule 6: a held notification has no banner to hide");

  // Identity: desktop entry, then portal app id, then scope app id, then app name.
  ok &= check(focus::identity("org.a.desktop", "org.b", "c", "D") == "org.a", "identity: desktop entry first");
  ok &= check(focus::identity("", "org.b", "c", "D") == "org.b", "identity: portal app id second");
  ok &= check(focus::identity("", "", "c", "D") == "c", "identity: scope app id third");
  ok &= check(focus::identity("", "", "", "D") == "D", "identity: app name last");

  // Lifetime: a timed Focus ends at its end time; a restored Focus of an unknown id is dropped.
  focus::FocusService service;
  int changes = 0;
  service.setChangedCallback([&changes] { ++changes; });
  ok &= check(!service.start("nope", std::nullopt, "user"), "start: an unknown Focus is refused");
  ok &= check(service.start("work", 1000, "user"), "start: a built-in Focus starts");
  ok &= check(service.current(999) != nullptr && service.current(999)->id == "work", "current: on before its end");
  ok &= check(service.current(1000) == nullptr, "current: off at its end time");
  ok &= check(service.expire(1000) && !service.active().has_value(), "expire: ends a Focus past its end");
  ok &= check(changes == 2, "the change callback runs on start and on end");
  service.restore({.focusId = "removed-focus"});
  ok &= check(!service.active().has_value(), "restore: a Focus that no longer exists is dropped");
  service.restore({.focusId = "dnd"});
  ok &= check(service.current(0) != nullptr && service.current(0)->id == "dnd", "restore: a saved Focus comes back");

  return ok ? 0 : 1;
}

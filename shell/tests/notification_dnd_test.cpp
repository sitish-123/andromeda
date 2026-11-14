#include "notification/notification_manager.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

} // namespace

int main() {
  NotificationManager manager;
  int closeCallbacks = 0;
  int closedEvents = 0;

  manager.setCloseCallback([&closeCallbacks](uint32_t, CloseReason, const std::string&) { ++closeCallbacks; });
  manager.addEventCallback([&closedEvents](const Notification&, NotificationEvent event) {
    if (event == NotificationEvent::Closed) {
      ++closedEvents;
    }
  });

  const auto addNotification = [&manager](std::string summary) {
    return manager.addOrReplace(
        NotificationRequest{
            .appName = "dnd-test",
            .summary = std::move(summary),
            .timeout = 0,
            .transient = true,
        }
    );
  };

  const uint32_t firstId = addNotification("first");
  const uint32_t secondId = addNotification("second");

  bool ok = true;
  ok &= check(firstId != secondId, "notifications receive distinct IDs");
  ok &= check(manager.all().size() == 2, "notifications are active before DND");

  manager.setDoNotDisturb(true);
  ok &= check(manager.doNotDisturb(), "DND is enabled");
  ok &= check(manager.all().size() == 2, "enabling DND preserves active notifications");
  ok &= check(closeCallbacks == 0, "enabling DND does not emit protocol close callbacks");
  ok &= check(closedEvents == 0, "enabling DND does not emit closed events");

  manager.setDoNotDisturb(false);
  ok &= check(!manager.doNotDisturb(), "DND is disabled");
  ok &= check(manager.all().size() == 2, "disabling DND preserves active notifications");
  ok &= check(closeCallbacks == 0, "disabling DND does not emit protocol close callbacks");
  ok &= check(closedEvents == 0, "disabling DND does not emit closed events");

  manager.setDoNotDisturb(true);
  manager.setFilters({NotificationFilterConfig{
      .name = "medication",
      .match = "medication",
      .bypassDnd = true,
  }});
  const uint32_t bypassId = manager.addOrReplace(
      NotificationRequest{
          .appName = "Medication Reminder",
          .summary = "Take medication",
          .timeout = 0,
          .transient = true,
      }
  );
  ok &= check(manager.all().back().id == bypassId, "DND bypass notification remains active");
  ok &= check(
      manager.all().back().dndPolicy == NotificationDndPolicy::Bypass,
      "matching filter promotes notification to DND bypass"
  );

  const uint32_t ordinaryId = manager.addOrReplace(
      NotificationRequest{
          .appName = "Other",
          .summary = "Ordinary",
          .timeout = 0,
          .transient = true,
      }
  );
  ok &= check(manager.all().back().id == ordinaryId, "ordinary DND notification remains active");
  ok &= check(
      manager.all().back().dndPolicy == NotificationDndPolicy::Respect,
      "unmatched notification continues to respect DND"
  );

  // docs/focus.md §3: a Work Focus holds an app not on its list (no banner) and lets a critical one through; ending it
  // reports what it held, by app.
  manager.setDoNotDisturb(false);
  int banners = 0;
  manager.addEventCallback([&banners](const Notification&, NotificationEvent event) {
    if (event == NotificationEvent::Added) {
      ++banners;
    }
  });
  std::string endedFocus;
  std::vector<std::pair<std::string, int>> summary;
  manager.setFocusEndedCallback([&](const focus::Focus& ended, const auto& held) {
    endedFocus = ended.name;
    summary = held;
  });
  ok &= check(manager.startFocus("work", std::nullopt, "user"), "Work Focus starts");
  addNotification("held one");
  addNotification("held two");
  ok &= check(banners == 0, "Work Focus holds an unlisted app's notifications");
  (void)manager.addOrReplace(NotificationRequest{
      .appName = "Alarm", .summary = "Wake", .urgency = Urgency::Critical, .timeout = 0, .transient = true,
  });
  ok &= check(banners == 1, "a critical notification breaks through the Focus");
  manager.endFocus();
  ok &= check(endedFocus == "Work", "ending the Focus reports it");
  ok &= check(summary == std::vector<std::pair<std::string, int>>{{"dnd-test", 2}}, "the summary counts held by app");

  return ok ? 0 : 1;
}

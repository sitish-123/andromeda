#include "notification/notification_manager.h"
#include "shell/lockscreen/lock_notice_groups.h"
#include "tests/test_check.h"

#include <chrono>
#include <deque>

namespace {

  NotificationHistoryEntry entry(std::string app, std::optional<std::string> desktop, int minute, bool seen) {
    NotificationHistoryEntry e;
    e.notification.appName = std::move(app);
    e.notification.desktopEntry = std::move(desktop);
    e.notification.summary = "secret summary";
    e.notification.receivedWallClock = WallTimePoint{} + std::chrono::minutes(minute);
    e.seen = seen;
    return e;
  }

} // namespace

int main() {
  std::deque<NotificationHistoryEntry> history;
  history.push_back(entry("Mail", "org.mail", 1, false));
  history.push_back(entry("WhatsApp", "com.whatsapp", 2, false));
  history.push_back(entry("Mail", "org.mail", 5, false));
  history.push_back(entry("Mail (beta)", "org.mail", 3, false));
  history.push_back(entry("Slack", "com.slack", 9, true));
  history.push_back(entry("WhatsApp", "com.whatsapp", 4, false));

  const auto groups = groupUnseenNotices(history);
  // Seen entries drop out; one group per desktop entry; the group with the newest notification leads.
  TEST_CHECK(groups.size() == 2);
  TEST_CHECK(groups[0].app == "Mail" && groups[0].count == 3);
  TEST_CHECK(groups[0].newest == WallTimePoint{} + std::chrono::minutes(5));
  TEST_CHECK(groups[1].app == "WhatsApp" && groups[1].count == 2);

  TEST_CHECK(groupUnseenNotices({}).empty());
  return 0;
}

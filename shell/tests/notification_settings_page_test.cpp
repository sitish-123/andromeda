#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

// A notification with the x-andromeda-settings-page hint opens that page on its default action; others do not.
int main() {
  NotificationManager manager;
  std::vector<std::string> opened;
  manager.setSettingsPageCallback([&opened](const std::string& page) { opened.push_back(page); });

  const uint32_t update = manager.addOrReplace(NotificationRequest{
      .appName = "Software Update",
      .summary = "Software Update",
      .body = "3 updates are available",
      .sender = ":1.7",
      .actions = {"default", "Open"},
      .settingsPage = "software-update",
  });
  const uint32_t plain = manager.addOrReplace(NotificationRequest{
      .appName = "chat-app",
      .summary = "msg",
      .body = "hi",
      .sender = ":1.8",
      .actions = {"default", "Open"},
  });

  TEST_CHECK(manager.invokeAction(plain, "default", true));
  TEST_CHECK(opened.empty());
  TEST_CHECK(manager.invokeAction(update, "default", true));
  TEST_CHECK(opened.size() == 1 && opened[0] == "software-update");
  return 0;
}

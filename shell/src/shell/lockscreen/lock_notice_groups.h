#pragma once

#include "notification/notification.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

struct NotificationHistoryEntry;

// Rulebook 9.15 notification group: one row per app with the count of unseen notifications. Content stays out
// (lockscreen.notification_content = hidden); the lock screen shows app, icon, count and the newest time only.
struct LockNoticeGroup {
  std::string app;
  std::string icon;
  int count = 0;
  std::optional<WallTimePoint> newest;
  bool operator==(const LockNoticeGroup&) const = default;
};

// Unseen history entries grouped by app (desktop entry, else app name), the newest group first.
[[nodiscard]] std::vector<LockNoticeGroup> groupUnseenNotices(const std::deque<NotificationHistoryEntry>& history);

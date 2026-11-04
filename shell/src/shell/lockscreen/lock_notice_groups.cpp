#include "shell/lockscreen/lock_notice_groups.h"

#include "notification/notification_manager.h"

#include <algorithm>

std::vector<LockNoticeGroup> groupUnseenNotices(const std::deque<NotificationHistoryEntry>& history) {
  std::vector<LockNoticeGroup> groups;
  std::vector<std::string> keys;
  for (const NotificationHistoryEntry& entry : history) {
    if (entry.seen) {
      continue;
    }
    const Notification& n = entry.notification;
    const std::string key = n.desktopEntry.value_or(n.appName);
    const auto it = std::ranges::find(keys, key);
    if (it == keys.end()) {
      keys.push_back(key);
      groups.push_back({.app = n.appName, .icon = n.icon.value_or(std::string{}), .count = 1, .newest = n.receivedWallClock});
      continue;
    }
    LockNoticeGroup& group = groups[static_cast<std::size_t>(it - keys.begin())];
    ++group.count;
    if (n.receivedWallClock.has_value() && (!group.newest.has_value() || *n.receivedWallClock > *group.newest)) {
      group.newest = n.receivedWallClock;
      if (n.icon.has_value()) {
        group.icon = *n.icon;
      }
    }
  }
  std::ranges::stable_sort(groups, [](const LockNoticeGroup& a, const LockNoticeGroup& b) {
    return a.newest.value_or(WallTimePoint{}) > b.newest.value_or(WallTimePoint{});
  });
  return groups;
}

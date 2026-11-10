#pragma once

#include "shell/lockscreen/lock_notice_groups.h"

#include <functional>
#include <memory>
#include <sdbus-c++/Types.h>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

// Read-only client of org.andromeda.Shell1.LockNotices for `noctalia lock`. Empty while the shell is off the bus.
class LockNoticeClient {
public:
  explicit LockNoticeClient(SessionBus& bus);
  ~LockNoticeClient();

  void setChangeCallback(std::function<void()> callback);
  [[nodiscard]] const std::vector<LockNoticeGroup>& groups() const noexcept { return m_groups; }

private:
  void refresh();
  void store(std::vector<LockNoticeGroup> groups);

  sdbus::Slot m_ownerMatch;
  std::unique_ptr<sdbus::IProxy> m_shell;
  std::vector<LockNoticeGroup> m_groups;
  std::function<void()> m_changeCallback;
};

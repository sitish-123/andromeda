#pragma once

#include "dbus/link/link_phone_state.h"

#include <functional>
#include <memory>

class SessionBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

// Read-only client of andromeda-linkd for `noctalia lock`: the phone row and the call card. It posts no
// notifications, plays no sounds and rings nothing; the shell's LinkService does that. A call that rang before this
// client started shows from its next Call signal.
class LinkLockClient final : public LinkPhoneState {
public:
  explicit LinkLockClient(SessionBus& bus);
  ~LinkLockClient() override;

  void setChangeCallback(std::function<void()> callback);
  void declineCall(const std::string& deviceId) override;

private:
  void refresh();
  void notify();

  std::unique_ptr<sdbus::IProxy> m_daemon;
  std::unique_ptr<sdbus::IProxy> m_link;
  std::function<void()> m_changeCallback;
};

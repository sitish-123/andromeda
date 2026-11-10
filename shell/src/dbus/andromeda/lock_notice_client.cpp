#include "dbus/andromeda/lock_notice_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "shell/lockscreen/lock_notice_wire.h"

#include <algorithm>
#include <map>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Message.h>

namespace {

  constexpr Logger kLog("lock");

  const sdbus::ServiceName kShellBusName{"org.andromeda.Shell1"};
  const sdbus::ObjectPath kShellPath{"/org/andromeda/Shell1"};
  constexpr auto kShellInterface = "org.andromeda.Shell1";
  constexpr auto kOwnerRule = "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                              "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.andromeda.Shell1'";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

} // namespace

LockNoticeClient::LockNoticeClient(SessionBus& bus) {
  m_ownerMatch = bus.connection().addMatch(
      kOwnerRule,
      [this](sdbus::Message message) {
        std::string name;
        std::string oldOwner;
        std::string newOwner;
        message >> name >> oldOwner >> newOwner;
        store({});
        if (!newOwner.empty()) {
          refresh();
        }
      },
      sdbus::return_slot
  );

  m_shell = sdbus::createProxy(bus.connection(), kShellBusName, kShellPath);
  m_shell->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& invalidated
            ) {
        if (interfaceName != kShellInterface) {
          return;
        }
        if (const auto it = changed.find(lock_notice_wire::kProperty); it != changed.end()) {
          try {
            store(lock_notice_wire::decode(it->second.get<std::vector<lock_notice_wire::Group>>()));
          } catch (const sdbus::Error& e) {
            kLog.warn("LockNotices: {}", e.what());
          }
        } else if (std::ranges::contains(invalidated, lock_notice_wire::kProperty)) {
          refresh();
        }
      });

  refresh();
}

LockNoticeClient::~LockNoticeClient() = default;

void LockNoticeClient::setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

void LockNoticeClient::refresh() {
  m_shell->callMethodAsync("Get")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kShellInterface}, std::string{lock_notice_wire::kProperty})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (error.has_value()) {
          kLog.debug("LockNotices unavailable: {}", error->what());
          return;
        }
        try {
          store(lock_notice_wire::decode(value.get<std::vector<lock_notice_wire::Group>>()));
        } catch (const sdbus::Error& e) {
          kLog.warn("LockNotices: {}", e.what());
        }
      });
}

void LockNoticeClient::store(std::vector<LockNoticeGroup> groups) {
  if (groups == m_groups) {
    return;
  }
  m_groups = std::move(groups);
  kLog.debug("lock notices: {} groups", m_groups.size());
  if (m_changeCallback) {
    m_changeCallback();
  }
}

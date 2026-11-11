#include "dbus/link/link_lock_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <sdbus-c++/IProxy.h>
#include <string_view>

namespace {

  constexpr Logger kLog("link");

  const sdbus::ServiceName kLinkBusName{"org.andromeda.Link1"};
  const sdbus::ObjectPath kLinkPath{"/org/andromeda/Link1"};
  constexpr auto kLinkInterface = "org.andromeda.Link1";
  const sdbus::ServiceName kDaemonBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDaemonPath{"/org/freedesktop/DBus"};
  constexpr auto kDaemonInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  using VariantMap = std::map<std::string, sdbus::Variant>;

  // The Link1 properties the phone row and the call card read; the client fetches and follows no others.
  constexpr std::array<std::string_view, 4> kPhoneProperties{"Devices", "DeviceStatus", "Grants", "NotificationCounts"};

  bool isPhoneProperty(std::string_view name) { return std::ranges::contains(kPhoneProperties, name); }

} // namespace

LinkLockClient::LinkLockClient(SessionBus& bus) {
  m_daemon = sdbus::createProxy(bus.connection(), kDaemonBusName, kDaemonPath);
  m_daemon->uponSignal("NameOwnerChanged")
      .onInterface(kDaemonInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kLinkBusName) {
          return;
        }
        clearPhoneState();
        m_call.reset();
        notify();
        if (!newOwner.empty()) {
          refresh();
        }
      });

  m_link = sdbus::createProxy(bus.connection(), kLinkBusName, kLinkPath);
  m_link->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const VariantMap& changed, const std::vector<std::string>& invalidated
            ) {
        if (interfaceName != kLinkInterface) {
          return;
        }
        if (std::ranges::any_of(invalidated, isPhoneProperty)) {
          refresh();
          return;
        }
        if (!std::ranges::any_of(changed | std::views::keys, isPhoneProperty)) {
          return;
        }
        applyPhoneProperties(changed);
        notify();
      });
  m_link->uponSignal("Call")
      .onInterface(kLinkInterface)
      .call([this](
                const std::string& deviceId, const std::string& state, const std::string& number,
                const std::string& name, const std::string& app
            ) {
        applyCall(deviceId, state, number, name, app);
        notify();
      });

  m_daemon->callMethodAsync("NameHasOwner")
      .onInterface(kDaemonInterface)
      .withArguments(std::string{kLinkBusName})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, bool owned) {
        if (!error.has_value() && owned) {
          refresh();
        }
      });
}

LinkLockClient::~LinkLockClient() = default;

void LinkLockClient::setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

void LinkLockClient::declineCall(const std::string& deviceId) {
  m_link->callMethodAsync("CallAction")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, std::string{"decline"})
      .uponReplyInvoke([](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          kLog.warn("CallAction failed: {}", error->what());
        }
      });
}

void LinkLockClient::refresh() {
  for (const std::string_view property : kPhoneProperties) {
    m_link->callMethodAsync("Get")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kLinkInterface}, std::string{property})
        .uponReplyInvoke([this, name = std::string{property}](std::optional<sdbus::Error> error, sdbus::Variant value) {
          if (error.has_value()) {
            kLog.debug("link property {} unavailable: {}", name, error->what());
            return;
          }
          applyPhoneProperties({{name, std::move(value)}});
          notify();
        });
  }
}

void LinkLockClient::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

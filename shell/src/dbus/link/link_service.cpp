#include "dbus/link/link_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <utility>

namespace {

  constexpr Logger kLog("link");

  const sdbus::ServiceName kLinkBusName{"org.umbriel.Link1"};
  const sdbus::ObjectPath kLinkPath{"/org/umbriel/Link1"};
  constexpr auto kLinkInterface = "org.umbriel.Link1";
  const sdbus::ServiceName kDaemonBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDaemonPath{"/org/freedesktop/DBus"};
  constexpr auto kDaemonInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  // The daemon's window length; it reports the close itself through Pairing.
  constexpr auto kPairingWindow = std::chrono::seconds(120);

  using VariantMap = std::map<std::string, sdbus::Variant>;

  void logFailure(std::string_view method, const std::optional<sdbus::Error>& error) {
    if (error.has_value()) {
      kLog.warn("{} failed: {}", method, error->what());
    }
  }

} // namespace

LinkService::LinkService(SessionBus& bus) {
  m_daemon = sdbus::createProxy(bus.connection(), kDaemonBusName, kDaemonPath);
  m_daemon->uponSignal("NameOwnerChanged")
      .onInterface(kDaemonInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kLinkBusName) {
          return;
        }
        // A restart hands the name straight to the new owner, so any owner means reload.
        detach();
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
        if (!invalidated.empty()) {
          refresh();
          return;
        }
        apply(changed);
      });
  m_link->uponSignal("PairingFinished")
      .onInterface(kLinkInterface)
      .call([this](const std::string& /*deviceId*/, const std::string& name) {
        m_pairing.reset();
        m_outcome = LinkPairingOutcome{.paired = true, .detail = name};
        notify();
      });
  m_link->uponSignal("PairingFailed").onInterface(kLinkInterface).call([this](const std::string& reason) {
    m_pairing.reset();
    m_outcome = LinkPairingOutcome{.paired = false, .detail = reason};
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

LinkService::~LinkService() = default;

void LinkService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void LinkService::refresh() {
  m_link->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kLinkInterface})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, VariantMap properties) {
        if (error.has_value()) {
          kLog.debug("link properties unavailable: {}", error->what());
          return;
        }
        m_available = true;
        apply(properties);
      });
}

void LinkService::apply(const std::map<std::string, sdbus::Variant>& properties) {
  if (const auto it = properties.find("Devices"); it != properties.end()) {
    try {
      m_devices.clear();
      for (const auto& device : it->second.get<std::vector<sdbus::Struct<std::string, std::string, bool>>>()) {
        m_devices.push_back({.id = device.get<0>(), .name = device.get<1>(), .connected = device.get<2>()});
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Devices: {}", e.what());
    }
  }
  if (const auto it = properties.find("Pairing"); it != properties.end()) {
    try {
      if (!it->second.get<bool>()) {
        m_pairing.reset();
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Pairing: {}", e.what());
    }
  }
  notify();
}

void LinkService::detach() {
  if (!m_available) {
    return;
  }
  m_available = false;
  m_devices.clear();
  m_pairing.reset();
  notify();
}

void LinkService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LinkService::startPairing() {
  m_link->callMethodAsync("StartPairing")
      .onInterface(kLinkInterface)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, std::string code, std::string uri) {
        if (error.has_value()) {
          logFailure("StartPairing", error);
          m_outcome = LinkPairingOutcome{.paired = false, .detail = error->getMessage()};
        } else if (code.size() != 6 || !std::ranges::all_of(code, [](char c) { return c >= '0' && c <= '9'; })) {
          kLog.warn("StartPairing returned a malformed code");
        } else {
          m_pairing = LinkPairing{
              .code = std::move(code),
              .uri = std::move(uri),
              .deadline = std::chrono::steady_clock::now() + kPairingWindow,
          };
          m_outcome.reset();
        }
        notify();
      });
}

void LinkService::cancelPairing() {
  m_link->callMethodAsync("CancelPairing")
      .onInterface(kLinkInterface)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("CancelPairing", error); });
}

void LinkService::unpair(const std::string& deviceId) {
  m_link->callMethodAsync("Unpair")
      .onInterface(kLinkInterface)
      .withArguments(deviceId)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("Unpair", error); });
}

void LinkService::registerIpc(IpcService& ipc, std::function<void()> showPairing) {
  ipc.bind(noctalia::cli::msg::linkDevices, [this](const std::string&) -> std::string {
    if (!m_available) {
      return "error: umbriel-linkd is not running\n";
    }
    std::string out;
    for (const auto& device : m_devices) {
      out += device.id + (device.connected ? " connected " : " disconnected ") + device.name + "\n";
    }
    return out;
  });
  ipc.bind(
      noctalia::cli::msg::linkPair, [this, showPairing = std::move(showPairing)](const std::string&) -> std::string {
        if (!m_available) {
          return "error: umbriel-linkd is not running\n";
        }
        startPairing();
        showPairing();
        return "ok\n";
      }
  );
  ipc.bind(noctalia::cli::msg::linkPairing, [this](const std::string&) -> std::string {
    if (m_pairing.has_value()) {
      return "open " + m_pairing->code + " " + m_pairing->uri + "\n";
    }
    if (m_outcome.has_value()) {
      return (m_outcome->paired ? "paired " : "failed ") + m_outcome->detail + "\n";
    }
    return "none\n";
  });
  ipc.bind(noctalia::cli::msg::linkUnpair, [this](const std::string& args) -> std::string {
    const std::string id = StringUtils::trim(args);
    if (!std::ranges::contains(m_devices, id, &LinkDevice::id)) {
      return "error: no paired device " + id + "\n";
    }
    unpair(id);
    return "ok\n";
  });
}

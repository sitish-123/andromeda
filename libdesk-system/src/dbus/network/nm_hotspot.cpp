#include "dbus/network/nm_hotspot.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <map>
#include <optional>
#include <random>
#include <sdbus-c++/sdbus-c++.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("hotspot");

  const sdbus::ServiceName kNmBusName{"org.freedesktop.NetworkManager"};
  const sdbus::ObjectPath kNmPath{"/org/freedesktop/NetworkManager"};
  const sdbus::ObjectPath kSettingsPath{"/org/freedesktop/NetworkManager/Settings"};
  constexpr auto kNmInterface = "org.freedesktop.NetworkManager";
  constexpr auto kSettingsInterface = "org.freedesktop.NetworkManager.Settings";
  constexpr auto kConnectionInterface = "org.freedesktop.NetworkManager.Settings.Connection";
  constexpr auto kActiveInterface = "org.freedesktop.NetworkManager.Connection.Active";
  constexpr auto kWirelessInterface = "org.freedesktop.NetworkManager.Device.Wireless";
  constexpr std::uint32_t kWifiCapAp = 0x40;
  constexpr std::uint32_t kActiveStateActivating = 1;
  constexpr std::uint32_t kActiveStateActivated = 2;
  constexpr std::uint32_t kActiveStateDeactivated = 4;

  using Settings = std::map<std::string, std::map<std::string, sdbus::Variant>>;
  using Properties = std::map<sdbus::PropertyName, sdbus::Variant>;
  using Proxy = std::shared_ptr<sdbus::IProxy>;
  // An error message, or the found object path (empty when none matched).
  using Found = std::function<void(std::optional<std::string> error, std::string path)>;

  // Callbacks hold their own proxy: destroying a proxy cancels its pending calls.
  Proxy proxy(SystemBus& bus, const std::string& path) {
    return Proxy(sdbus::createProxy(bus.connection(), kNmBusName, sdbus::ObjectPath{path}));
  }

  bool isAccessPoint(const Settings& settings) {
    const auto wireless = settings.find("802-11-wireless");
    if (wireless == settings.end()) {
      return false;
    }
    const auto mode = wireless->second.find("mode");
    try {
      return mode != wireless->second.end() && mode->second.get<std::string>() == "ap";
    } catch (const sdbus::Error&) {
      return false;
    }
  }

  // One async check per object; reports the first object in list order that matched.
  struct FirstMatch {
    std::vector<sdbus::ObjectPath> paths;
    std::vector<bool> matched;
    std::size_t pending = 0;
    Found done;

    static std::shared_ptr<FirstMatch> start(std::vector<sdbus::ObjectPath> paths, Found done) {
      auto match = std::make_shared<FirstMatch>();
      match->matched.assign(paths.size(), false);
      match->pending = paths.size();
      match->paths = std::move(paths);
      match->done = std::move(done);
      if (match->pending == 0) {
        match->done(std::nullopt, {});
      }
      return match;
    }

    void settle(std::size_t index, bool match) {
      matched[index] = match;
      if (--pending != 0) {
        return;
      }
      for (std::size_t i = 0; i < paths.size(); ++i) {
        if (matched[i]) {
          done(std::nullopt, paths[i]);
          return;
        }
      }
      done(std::nullopt, {});
    }
  };

  void isAccessPointProfile(SystemBus& bus, const std::string& path, std::function<void(bool)> done) {
    try {
      auto connection = proxy(bus, path);
      connection->callMethodAsync("GetSettings")
          .onInterface(kConnectionInterface)
          .uponReplyInvoke([connection, done](std::optional<sdbus::Error> error, const Settings& settings) {
            done(!error.has_value() && isAccessPoint(settings));
          });
    } catch (const sdbus::Error&) {
      done(false);
    }
  }

  void findProfile(SystemBus& bus, Found done) {
    try {
      auto settings = proxy(bus, kSettingsPath);
      settings->callMethodAsync("ListConnections")
          .onInterface(kSettingsInterface)
          .uponReplyInvoke([settings, &bus, done](std::optional<sdbus::Error> error,
                                                  std::vector<sdbus::ObjectPath> paths) {
            if (error.has_value()) {
              done(error->what(), {});
              return;
            }
            auto match = FirstMatch::start(std::move(paths), done);
            for (std::size_t i = 0; i < match->paths.size(); ++i) {
              isAccessPointProfile(bus, match->paths[i], [match, i](bool ap) { match->settle(i, ap); });
            }
          });
    } catch (const sdbus::Error& e) {
      done(e.what(), {});
    }
  }

  void findApDevice(SystemBus& bus, sdbus::IProxy& nm, Found done) {
    try {
      nm.callMethodAsync("GetDevices")
          .onInterface(kNmInterface)
          .uponReplyInvoke([&bus, done](std::optional<sdbus::Error> error, std::vector<sdbus::ObjectPath> paths) {
            if (error.has_value()) {
              done(error->what(), {});
              return;
            }
            auto match = FirstMatch::start(std::move(paths), done);
            for (std::size_t i = 0; i < match->paths.size(); ++i) {
              try {
                // Only Wi-Fi devices have the Wireless interface; the others answer with an error.
                auto device = proxy(bus, match->paths[i]);
                device->getPropertyAsync("WirelessCapabilities")
                    .onInterface(kWirelessInterface)
                    .uponReplyInvoke([device, match, i](std::optional<sdbus::Error> e, const sdbus::Variant& caps) {
                      bool ap = false;
                      try {
                        ap = !e.has_value() && (caps.get<std::uint32_t>() & kWifiCapAp) != 0;
                      } catch (const sdbus::Error&) {
                      }
                      match->settle(i, ap);
                    });
              } catch (const sdbus::Error&) {
                match->settle(i, false);
              }
            }
          });
    } catch (const sdbus::Error& e) {
      done(e.what(), {});
    }
  }

  // An error message, or the found active connection and its NM state (path empty when none matched).
  using ActiveFound = std::function<void(std::optional<std::string> error, std::string path, std::uint32_t state)>;

  // The active connection of an access-point profile that is activating or up. Checks only active Wi-Fi
  // connections: listing every saved profile costs ~30 D-Bus calls.
  void findActive(SystemBus& bus, sdbus::IProxy& nm, ActiveFound done) {
    try {
      nm.getPropertyAsync("ActiveConnections")
          .onInterface(kNmInterface)
          .uponReplyInvoke([&bus, done](std::optional<sdbus::Error> error, const sdbus::Variant& value) {
            std::vector<sdbus::ObjectPath> paths;
            try {
              if (error.has_value()) {
                done(error->what(), {}, 0);
                return;
              }
              paths = value.get<std::vector<sdbus::ObjectPath>>();
            } catch (const sdbus::Error& e) {
              done(e.what(), {}, 0);
              return;
            }
            auto states = std::make_shared<std::map<std::string, std::uint32_t>>();
            auto match = FirstMatch::start(std::move(paths), [states, done](std::optional<std::string> e, std::string path) {
              const auto state = states->find(path);
              done(std::move(e), path, state != states->end() ? state->second : 0);
            });
            for (std::size_t i = 0; i < match->paths.size(); ++i) {
              try {
                auto active = proxy(bus, match->paths[i]);
                active->getAllPropertiesAsync()
                    .onInterface(kActiveInterface)
                    .uponReplyInvoke([active, &bus, match, states, i](std::optional<sdbus::Error> e,
                                                                      const Properties& props) {
                      std::string profile;
                      try {
                        const auto type = props.find(sdbus::PropertyName{"Type"});
                        const auto state = props.find(sdbus::PropertyName{"State"});
                        const auto connection = props.find(sdbus::PropertyName{"Connection"});
                        if (!e.has_value() && type != props.end() && state != props.end() && connection != props.end()
                            && type->second.get<std::string>() == "802-11-wireless"
                            && (state->second.get<std::uint32_t>() == kActiveStateActivating
                                || state->second.get<std::uint32_t>() == kActiveStateActivated)) {
                          profile = connection->second.get<sdbus::ObjectPath>();
                          (*states)[match->paths[i]] = state->second.get<std::uint32_t>();
                        }
                      } catch (const sdbus::Error&) {
                      }
                      if (profile.empty()) {
                        match->settle(i, false);
                        return;
                      }
                      isAccessPointProfile(bus, profile, [match, i](bool ap) { match->settle(i, ap); });
                    });
              } catch (const sdbus::Error&) {
                match->settle(i, false);
              }
            }
          });
    } catch (const sdbus::Error& e) {
      done(e.what(), {}, 0);
    }
  }

  // NMActiveConnectionStateReason as NetworkManager names it; empty for unknown or none.
  std::string_view reasonName(std::uint32_t reason) {
    static constexpr std::string_view kNames[] = {
        "", "", "user-disconnected", "device-disconnected", "service-stopped", "ip-config-invalid",
        "connect-timeout", "service-start-timeout", "service-start-failed", "no-secrets", "login-failed",
        "connection-removed", "dependency-failed", "device-realize-failed", "device-removed",
    };
    return reason < std::size(kNames) ? kNames[reason] : std::string_view{};
  }

  std::string hostname() {
    char name[256] = {};
    return ::gethostname(name, sizeof(name) - 1) == 0 && name[0] != '\0' ? std::string(name) : std::string("Noctalia");
  }

  std::string randomKey() {
    constexpr std::string_view kAlphabet = "abcdefghijkmnpqrstuvwxyz23456789";
    std::random_device random;
    std::uniform_int_distribution<std::size_t> pick(0, kAlphabet.size() - 1);
    std::string key;
    for (int i = 0; i < 12; ++i) {
      key += kAlphabet[pick(random)];
    }
    return key;
  }

  Settings newProfile() {
    const std::string ssid = hostname();
    Settings settings;
    settings["connection"]["id"] = sdbus::Variant{std::string("Hotspot")};
    settings["connection"]["type"] = sdbus::Variant{std::string("802-11-wireless")};
    settings["connection"]["autoconnect"] = sdbus::Variant{false};
    settings["802-11-wireless"]["ssid"] = sdbus::Variant{std::vector<std::uint8_t>(ssid.begin(), ssid.end())};
    settings["802-11-wireless"]["mode"] = sdbus::Variant{std::string("ap")};
    settings["802-11-wireless-security"]["key-mgmt"] = sdbus::Variant{std::string("wpa-psk")};
    settings["802-11-wireless-security"]["proto"] = sdbus::Variant{std::vector<std::string>{"rsn"}};
    settings["802-11-wireless-security"]["psk"] = sdbus::Variant{randomKey()};
    settings["ipv4"]["method"] = sdbus::Variant{std::string("shared")};
    settings["ipv6"]["method"] = sdbus::Variant{std::string("ignore")};
    return settings;
  }

} // namespace

NmHotspot::NmHotspot(SystemBus& bus) : m_bus(bus), m_nm(sdbus::createProxy(bus.connection(), kNmBusName, kNmPath)) {
  m_nm->uponSignal("PropertiesChanged")
      .onInterface("org.freedesktop.DBus.Properties")
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName == kNmInterface && changed.contains("ActiveConnections")) {
          startQuery();
        }
      });
  startQuery();
}

NmHotspot::~NmHotspot() = default;

void NmHotspot::notify(bool changed) {
  if (changed && m_onChange) {
    m_onChange();
  }
}

bool NmHotspot::setEnabled(bool enabled) {
  if (!m_machine.apply(enabled ? HotspotEvent::RequestOn : HotspotEvent::RequestOff)) {
    return false;
  }
  notify(true);
  if (enabled) {
    startActivate();
  } else {
    startDeactivate();
  }
  return true;
}

void NmHotspot::startQuery() {
  const std::weak_ptr<int> alive = m_lifetime;
  const std::uint64_t token = m_machine.beginQuery();
  findActive(m_bus, *m_nm, [this, alive, token](std::optional<std::string> error, std::string path, std::uint32_t state) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.debug("hotspot state unavailable: {}", *error);
      return;
    }
    const HotspotObservation seen = path.empty()                       ? HotspotObservation::Off
                                    : state == kActiveStateActivated ? HotspotObservation::On
                                                                     : HotspotObservation::Activating;
    std::string failure;
    if (seen == HotspotObservation::Off && m_machine.state() == HotspotState::Activating) {
      failure = m_failReason.empty() ? i18n::tr("control-center.detail.wifi.hotspot-failed")
                                     : i18n::tr("control-center.detail.wifi.hotspot-failed-reason", "reason", m_failReason);
      kLog.warn("hotspot stopped while starting: {}", m_failReason.empty() ? "connection gone" : m_failReason);
    }
    notify(m_machine.observe(token, seen, failure));
  });
}

void NmHotspot::watchActive(const std::string& path) {
  m_failReason.clear();
  try {
    m_activeWatch = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{path});
    m_activeWatch->uponSignal("StateChanged")
        .onInterface(kActiveInterface)
        .call([this](std::uint32_t state, std::uint32_t reason) {
          if (state == kActiveStateDeactivated) {
            m_failReason = reasonName(reason);
          }
          startQuery();
        });
  } catch (const sdbus::Error& e) {
    m_activeWatch.reset();
    kLog.debug("hotspot connection watch failed: {}", e.what());
  }
}

void NmHotspot::startActivate() {
  const std::weak_ptr<int> alive = m_lifetime;
  // A reply without error carries the new active connection; it stays Activating until NM reports it Activated.
  const auto settle = [this, alive](std::optional<std::string> error, const std::string& active = {}) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.warn("hotspot activation failed: {}", *error);
    } else {
      watchActive(active);
    }
    notify(m_machine.apply(error.has_value() ? HotspotEvent::ActivateFailed : HotspotEvent::ActivateOk,
                           error.value_or("")));
    startQuery();
  };
  findProfile(m_bus, [this, alive, settle](std::optional<std::string> error, std::string profile) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      settle(error);
      return;
    }
    findApDevice(m_bus, *m_nm, [this, alive, settle, profile](std::optional<std::string> deviceError, std::string device) {
      if (alive.expired()) {
        return;
      }
      if (deviceError.has_value() || device.empty()) {
        settle(deviceError.value_or("no Wi-Fi device can host a hotspot"));
        return;
      }
      const auto onReply = [settle](const std::optional<sdbus::Error>& reply, const sdbus::ObjectPath& active) {
        settle(reply.has_value() ? std::optional<std::string>(reply->what()) : std::nullopt, active);
      };
      // Activation can wait on polkit.
      try {
        if (!profile.empty()) {
          m_nm->callMethodAsync("ActivateConnection")
              .onInterface(kNmInterface)
              .withArguments(sdbus::ObjectPath{profile}, sdbus::ObjectPath{device}, sdbus::ObjectPath{"/"})
              .uponReplyInvoke([onReply](std::optional<sdbus::Error> reply, sdbus::ObjectPath active) {
                onReply(reply, active);
              });
        } else {
          m_nm->callMethodAsync("AddAndActivateConnection")
              .onInterface(kNmInterface)
              .withArguments(newProfile(), sdbus::ObjectPath{device}, sdbus::ObjectPath{"/"})
              .uponReplyInvoke([onReply](std::optional<sdbus::Error> reply, sdbus::ObjectPath /*profile*/,
                                         sdbus::ObjectPath active) { onReply(reply, active); });
        }
      } catch (const sdbus::Error& e) {
        settle(e.what());
      }
    });
  });
}

void NmHotspot::startDeactivate() {
  const std::weak_ptr<int> alive = m_lifetime;
  const auto settle = [this, alive](std::optional<std::string> error) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.warn("hotspot deactivation failed: {}", *error);
    }
    notify(m_machine.apply(error.has_value() ? HotspotEvent::DeactivateFailed : HotspotEvent::DeactivateOk,
                           error.value_or("")));
    startQuery();
  };
  findActive(m_bus, *m_nm, [this, alive, settle](std::optional<std::string> error, std::string active,
                                                 std::uint32_t /*state*/) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value() || active.empty()) {
      settle(error);
      return;
    }
    try {
      m_nm->callMethodAsync("DeactivateConnection")
          .onInterface(kNmInterface)
          .withArguments(sdbus::ObjectPath{active})
          .uponReplyInvoke([settle](std::optional<sdbus::Error> reply) {
            settle(reply.has_value() ? std::optional<std::string>(reply->what()) : std::nullopt);
          });
    } catch (const sdbus::Error& e) {
      settle(e.what());
    }
  });
}

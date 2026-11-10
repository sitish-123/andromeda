#include "dbus/network/nm_hotspot.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <sdbus-c++/sdbus-c++.h>
#include <unistd.h>
#include <utility>
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
  constexpr auto kDeviceInterface = "org.freedesktop.NetworkManager.Device";
  constexpr auto kAccessPointInterface = "org.freedesktop.NetworkManager.AccessPoint";
  constexpr std::uint32_t kDeviceStateDisconnected = 30;
  constexpr auto kDeviceWait = std::chrono::seconds(10);
  const sdbus::ServiceName kSystem1BusName{"org.andromeda.System1"};
  const sdbus::ObjectPath kSystem1NetworkPath{"/org/andromeda/System1/Network"};
  constexpr auto kSystem1NetworkInterface = "org.andromeda.System1.Network";

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

  // An error message, or the found active connection, its NM state and its first device (path empty when none
  // matched).
  using ActiveFound = std::function<void(std::optional<std::string> error, std::string path, std::uint32_t state,
                                         std::string device)>;

  // The name System1 gives an access-point interface beside a Wi-Fi client: `<client>ap`.
  bool isBesideClientName(std::string_view interface) { return interface.size() > 2 && interface.ends_with("ap"); }

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
                done(error->what(), {}, 0, {});
                return;
              }
              paths = value.get<std::vector<sdbus::ObjectPath>>();
            } catch (const sdbus::Error& e) {
              done(e.what(), {}, 0, {});
              return;
            }
            auto states = std::make_shared<std::map<std::string, std::uint32_t>>();
            auto devices = std::make_shared<std::map<std::string, std::string>>();
            auto match = FirstMatch::start(std::move(paths), [states, devices, done](std::optional<std::string> e,
                                                                                     std::string path) {
              const auto state = states->find(path);
              const auto device = devices->find(path);
              done(std::move(e), path, state != states->end() ? state->second : 0,
                   device != devices->end() ? device->second : std::string());
            });
            for (std::size_t i = 0; i < match->paths.size(); ++i) {
              try {
                auto active = proxy(bus, match->paths[i]);
                active->getAllPropertiesAsync()
                    .onInterface(kActiveInterface)
                    .uponReplyInvoke([active, &bus, match, states, devices, i](std::optional<sdbus::Error> e,
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
                          const auto onDevices = props.find(sdbus::PropertyName{"Devices"});
                          if (onDevices != props.end()) {
                            const auto list = onDevices->second.get<std::vector<sdbus::ObjectPath>>();
                            if (!list.empty()) {
                              (*devices)[match->paths[i]] = list.front();
                            }
                          }
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
      done(e.what(), {}, 0, {});
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

  Proxy system1(SystemBus& bus) {
    return Proxy(sdbus::createProxy(bus.connection(), kSystem1BusName, kSystem1NetworkPath));
  }

  // Reads one property of a NetworkManager object; nullopt on any error.
  template <typename T>
  void readProperty(SystemBus& bus, const std::string& path, const char* interface, const char* name,
                    std::function<void(std::optional<T>)> done) {
    try {
      auto object = proxy(bus, path);
      object->getPropertyAsync(name).onInterface(interface).uponReplyInvoke(
          [object, done](std::optional<sdbus::Error> error, const sdbus::Variant& value) {
            std::optional<T> result;
            try {
              if (!error.has_value()) {
                result = value.get<T>();
              }
            } catch (const sdbus::Error&) {
            }
            done(std::move(result));
          });
    } catch (const sdbus::Error&) {
      done(std::nullopt);
    }
  }

  // The first Wi-Fi device in infrastructure mode with an access point, and that access point; empty when none.
  void findClient(SystemBus& bus, sdbus::IProxy& nm, std::function<void(HotspotClientScan)> done) {
    try {
      nm.callMethodAsync("GetDevices")
          .onInterface(kNmInterface)
          .uponReplyInvoke([&bus, done](std::optional<sdbus::Error> error, std::vector<sdbus::ObjectPath> paths) {
            if (error.has_value() || paths.empty()) {
              done({});
              return;
            }
            struct Scan {
              std::vector<HotspotWifiDevice> devices;
              std::size_t pending = 0;
            };
            auto scan = std::make_shared<Scan>(Scan{.devices = std::vector<HotspotWifiDevice>(paths.size()),
                                                    .pending = paths.size()});
            const auto settle = [scan, done] {
              if (--scan->pending == 0) {
                done(hotspotClientScan(scan->devices));
              }
            };
            for (std::size_t i = 0; i < paths.size(); ++i) {
              try {
                auto device = proxy(bus, paths[i]);
                device->getAllPropertiesAsync()
                    .onInterface(kWirelessInterface)
                    .uponReplyInvoke([device, scan, settle, i, path = std::string(paths[i])](
                                         std::optional<sdbus::Error> e, const Properties& props) {
                      try {
                        const auto mode = props.find(sdbus::PropertyName{"Mode"});
                        const auto ap = props.find(sdbus::PropertyName{"ActiveAccessPoint"});
                        if (!e.has_value() && mode != props.end() && ap != props.end()) {
                          scan->devices[i] = HotspotWifiDevice{.path = path,
                                                               .mode = mode->second.get<std::uint32_t>(),
                                                               .accessPoint = ap->second.get<sdbus::ObjectPath>()};
                        }
                      } catch (const sdbus::Error&) {
                      }
                      settle();
                    });
              } catch (const sdbus::Error&) {
                settle();
              }
            }
          });
    } catch (const sdbus::Error&) {
      done({});
    }
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
          startPlan();
        }
      });
  m_nm->uponSignal("DeviceAdded").onInterface(kNmInterface).call([this](const sdbus::ObjectPath& /*device*/) {
    if (!m_ap.interface.empty() && m_ap.device.empty()) {
      checkApDevice();
    }
  });
  startQuery();
  startPlan();
}

NmHotspot::~NmHotspot() {
  if (m_ap.interface.empty()) {
    return;
  }
  // The shell exits: nothing waits for the replies. System1 also removes the interface when this bus name leaves.
  try {
    auto system = system1(m_bus);
    if (!m_activePath.empty()) {
      system->callMethod("DeactivateConnection")
          .onInterface(kSystem1NetworkInterface)
          .withArguments(sdbus::ObjectPath{m_activePath})
          .dontExpectReply();
    }
    system->callMethod("RemoveApInterface")
        .onInterface(kSystem1NetworkInterface)
        .withArguments(m_ap.interface)
        .dontExpectReply();
  } catch (const sdbus::Error& e) {
    kLog.warn("hotspot interface {} left behind: {}", m_ap.interface, e.what());
  }
}

void NmHotspot::notify(bool changed) {
  if (changed && m_onChange) {
    m_onChange();
  }
}

void NmHotspot::advance(bool changed) {
  notify(changed);
  if (const auto work = m_machine.takeWork()) {
    run(*work);
  }
}

void NmHotspot::run(HotspotWork work) {
  switch (work) {
  case HotspotWork::Activate:
    startActivate();
    return;
  case HotspotWork::CreateInterface:
    startCreateInterface();
    return;
  case HotspotWork::AwaitDevice:
    startAwaitDevice();
    return;
  case HotspotWork::ActivateOnInterface:
    startActivateOnInterface();
    return;
  case HotspotWork::Deactivate:
    startDeactivate();
    return;
  case HotspotWork::RemoveInterface:
    startRemoveInterface();
    return;
  }
}

bool NmHotspot::setEnabled(bool enabled) {
  const bool beside = enabled && m_plan.kind == ClientPlan::Kind::BesideClient;
  const HotspotEvent event = !enabled ? HotspotEvent::RequestOff
                             : beside ? HotspotEvent::RequestOnBesideClient
                                      : HotspotEvent::RequestOn;
  if (!m_machine.apply(event)) {
    return false;
  }
  if (beside) {
    m_ap = ApRun{.client = m_plan.client, .channel = m_plan.channel, .interface = {}, .device = {}};
  }
  advance(true);
  return true;
}

void NmHotspot::setPlan(ClientPlan plan) {
  const bool changed = plan.kind != m_plan.kind;
  m_plan = std::move(plan);
  const bool first = m_machine.apply(HotspotEvent::PlanKnown);
  notify(changed || first);
  followClientChannel();
}

void NmHotspot::followClientChannel() {
  if (m_plan.kind != ClientPlan::Kind::BesideClient || m_ap.channel.channel == 0 || m_ap.client != m_plan.client
      || (m_ap.channel.band == m_plan.channel.band && m_ap.channel.channel == m_plan.channel.channel)) {
    return;
  }
  // Only a running hotspot restarts; one that is still starting restarts once it runs.
  if (!m_machine.apply(HotspotEvent::ClientChannelChanged)) {
    return;
  }
  kLog.info("Wi-Fi client {} moved to channel {}: hotspot restarts there", m_plan.client, m_plan.channel.channel);
  m_ap.channel = m_plan.channel;
  advance(true);
}

void NmHotspot::watchClient(const std::string& device, const std::string& ap) {
  const auto watch = [this](std::unique_ptr<sdbus::IProxy>& slot, const std::string& path, const char* interface,
                            const char* property) {
    if (path.empty() || path == "/") {
      slot.reset();
      return;
    }
    if (slot && slot->getObjectPath() == path) {
      return;
    }
    try {
      slot = sdbus::createProxy(m_bus.connection(), kNmBusName, sdbus::ObjectPath{path});
      slot->uponSignal("PropertiesChanged")
          .onInterface("org.freedesktop.DBus.Properties")
          .call([this, interface, property](const std::string& interfaceName,
                                            const std::map<std::string, sdbus::Variant>& changed,
                                            const std::vector<std::string>& /*invalidated*/) {
            if (interfaceName == interface && changed.contains(property)) {
              startPlan();
            }
          });
    } catch (const sdbus::Error& e) {
      slot.reset();
      kLog.debug("Wi-Fi client watch failed: {}", e.what());
    }
  };
  watch(m_clientDeviceWatch, device, kWirelessInterface, "ActiveAccessPoint");
  watch(m_clientApWatch, ap, kAccessPointInterface, "Frequency");
}

void NmHotspot::startPlan() {
  const std::weak_ptr<int> alive = m_lifetime;
  const std::uint64_t token = ++m_planToken;
  const auto current = [this, alive, token] { return !alive.expired() && token == m_planToken; };
  const ClientPlan turnsOff{.kind = ClientPlan::Kind::TurnsOffClient, .client = {}, .channel = {}};
  findClient(m_bus, *m_nm, [this, current, turnsOff](const HotspotClientScan& scan) {
    if (!current()) {
      return;
    }
    watchClient(scan.watch, scan.accessPoint);
    if (scan.client.empty()) {
      setPlan({});
      return;
    }
    const std::string& device = scan.client;
    const std::string& ap = scan.accessPoint;
    readProperty<std::uint32_t>(m_bus, ap, kAccessPointInterface, "Frequency", [this, current, turnsOff, device](
                                                                                    std::optional<std::uint32_t> mhz) {
      if (!current()) {
        return;
      }
      const std::optional<HotspotChannel> channel = mhz.has_value() ? hotspotChannelOf(*mhz) : std::nullopt;
      if (!channel.has_value()) {
        kLog.warn("Wi-Fi client on {} MHz: no hotspot beside it, the hotspot turns off Wi-Fi", mhz.value_or(0));
        setPlan(turnsOff);
        return;
      }
      readProperty<std::string>(m_bus, device, kDeviceInterface, "Interface", [this, current, turnsOff, channel](
                                                                                  std::optional<std::string> client) {
        if (!current()) {
          return;
        }
        if (!client.has_value()) {
          setPlan(turnsOff);
          return;
        }
        try {
          auto system = system1(m_bus);
          system->callMethodAsync("CanRunApWithClient")
              .onInterface(kSystem1NetworkInterface)
              .withArguments(*client)
              .uponReplyInvoke([this, system, current, turnsOff, client = *client,
                                channel = *channel](std::optional<sdbus::Error> error, bool can) {
                if (!current()) {
                  return;
                }
                if (error.has_value()) {
                  kLog.warn("CanRunApWithClient failed, the hotspot turns off Wi-Fi: {}", error->what());
                }
                setPlan(!error.has_value() && can
                            ? ClientPlan{.kind = ClientPlan::Kind::BesideClient, .client = client, .channel = channel}
                            : turnsOff);
              });
        } catch (const sdbus::Error& e) {
          kLog.warn("CanRunApWithClient failed, the hotspot turns off Wi-Fi: {}", e.what());
          setPlan(turnsOff);
        }
      });
    });
  });
}

void NmHotspot::startQuery() {
  const std::weak_ptr<int> alive = m_lifetime;
  const std::uint64_t token = m_machine.beginQuery();
  findActive(m_bus, *m_nm, [this, alive, token](std::optional<std::string> error, std::string path, std::uint32_t state,
                                                 std::string device) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.debug("hotspot state unavailable: {}", *error);
      return;
    }
    const auto finish = [this, token, path](HotspotObservation seen, const std::string& interface,
                                            const std::string& device) {
      m_activePath = path;
      std::string failure;
      if (seen == HotspotObservation::Off && m_machine.state() == HotspotState::Activating) {
        failure = m_failReason.empty()
                      ? i18n::tr("control-center.detail.wifi.hotspot-failed")
                      : i18n::tr("control-center.detail.wifi.hotspot-failed-reason", "reason", m_failReason);
        kLog.warn("hotspot stopped while starting: {}", m_failReason.empty() ? "connection gone" : m_failReason);
      }
      // A plain activation that NM ran on such an interface is not adopted.
      const bool starting = m_machine.state() == HotspotState::Activating;
      const bool changed = m_machine.observe(token, seen, failure);
      if (changed && !starting && seen == HotspotObservation::OnBesideClient && m_ap.interface.empty()) {
        kLog.info("hotspot on {} adopted; turning it off removes the interface", interface);
        m_ap = ApRun{
            .client = interface.substr(0, interface.size() - 2), .channel = {}, .interface = interface, .device = device};
      }
      advance(changed);
      followClientChannel();
    };
    if (path.empty() || state != kActiveStateActivated || device.empty()) {
      finish(path.empty() ? HotspotObservation::Off
             : state == kActiveStateActivated ? HotspotObservation::On
                                              : HotspotObservation::Activating,
             {}, {});
      return;
    }
    readProperty<std::string>(m_bus, device, kDeviceInterface, "Interface",
                              [alive, finish, device](std::optional<std::string> interface) {
                                if (alive.expired()) {
                                  return;
                                }
                                const bool beside = interface.has_value() && isBesideClientName(*interface);
                                finish(beside ? HotspotObservation::OnBesideClient : HotspotObservation::On,
                                       interface.value_or(""), device);
                              });
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

void NmHotspot::finishActivate(std::optional<std::string> error, const std::string& active) {
  if (error.has_value()) {
    kLog.warn("hotspot activation failed: {}", *error);
  } else {
    watchActive(active);
  }
  advance(m_machine.apply(error.has_value() ? HotspotEvent::ActivateFailed : HotspotEvent::ActivateOk,
                          error.value_or("")));
  startQuery();
}

void NmHotspot::startActivate() {
  const std::weak_ptr<int> alive = m_lifetime;
  const auto settle = [this, alive](std::optional<std::string> error, const std::string& active = {}) {
    if (!alive.expired()) {
      finishActivate(std::move(error), active);
    }
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
    advance(m_machine.apply(error.has_value() ? HotspotEvent::DeactivateFailed : HotspotEvent::DeactivateOk,
                            error.value_or("")));
    startQuery();
  };
  findActive(m_bus, *m_nm, [this, alive, settle](std::optional<std::string> error, std::string active,
                                                 std::uint32_t /*state*/, std::string /*device*/) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value() || active.empty()) {
      settle(error);
      return;
    }
    try {
      auto system = system1(m_bus);
      system->callMethodAsync("DeactivateConnection")
          .onInterface(kSystem1NetworkInterface)
          .withArguments(sdbus::ObjectPath{active})
          .uponReplyInvoke([system, settle](std::optional<sdbus::Error> reply) {
            settle(reply.has_value() ? std::optional<std::string>(reply->what()) : std::nullopt);
          });
    } catch (const sdbus::Error& e) {
      settle(e.what());
    }
  });
}

void NmHotspot::startCreateInterface() {
  const std::weak_ptr<int> alive = m_lifetime;
  const auto settle = [this, alive](std::optional<std::string> error, std::string ap) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.warn("hotspot interface beside {} not created: {}", m_ap.client, *error);
    } else {
      kLog.info("hotspot interface {} created beside {}", ap, m_ap.client);
      m_ap.interface = std::move(ap);
    }
    advance(m_machine.apply(error.has_value() ? HotspotEvent::InterfaceFailed : HotspotEvent::InterfaceCreated,
                            error.value_or("")));
  };
  try {
    auto system = system1(m_bus);
    system->callMethodAsync("CreateApInterface")
        .onInterface(kSystem1NetworkInterface)
        .withArguments(m_ap.client)
        .uponReplyInvoke([system, settle](std::optional<sdbus::Error> error, std::string ap) {
          settle(error.has_value() ? std::optional<std::string>(error->what()) : std::nullopt, std::move(ap));
        });
  } catch (const sdbus::Error& e) {
    settle(e.what(), {});
  }
}

void NmHotspot::startAwaitDevice() {
  m_deviceTimeout.start(kDeviceWait, [this] {
    kLog.warn("hotspot interface {} not ready in NetworkManager", m_ap.interface);
    advance(m_machine.apply(HotspotEvent::DeviceTimeout,
                            i18n::tr("control-center.detail.wifi.hotspot-failed-reason", "reason", "device-timeout")));
  });
  checkApDevice();
}

void NmHotspot::checkApDevice() {
  const std::weak_ptr<int> alive = m_lifetime;
  const std::string ap = m_ap.interface;
  // NetworkManager can activate on the device from state Disconnected on.
  const auto ready = [this, alive, ap](const std::string& device, std::uint32_t state) {
    if (alive.expired() || ap != m_ap.interface || state < kDeviceStateDisconnected
        || !m_machine.apply(HotspotEvent::DeviceReady)) {
      return;
    }
    m_ap.device = device;
    advance(true);
  };
  try {
    m_nm->callMethodAsync("GetDeviceByIpIface")
        .onInterface(kNmInterface)
        .withArguments(ap)
        .uponReplyInvoke([this, alive, ready](std::optional<sdbus::Error> error, sdbus::ObjectPath device) {
          // An error means NetworkManager does not list the interface yet; DeviceAdded checks again.
          if (alive.expired() || error.has_value()) {
            return;
          }
          try {
            m_apDeviceWatch = sdbus::createProxy(m_bus.connection(), kNmBusName, device);
            m_apDeviceWatch->uponSignal("StateChanged")
                .onInterface(kDeviceInterface)
                .call([ready, device](std::uint32_t state, std::uint32_t /*old*/, std::uint32_t /*reason*/) {
                  ready(device, state);
                });
          } catch (const sdbus::Error& e) {
            kLog.debug("hotspot device watch failed: {}", e.what());
          }
          readProperty<std::uint32_t>(m_bus, device, kDeviceInterface, "State",
                                      [ready, device](std::optional<std::uint32_t> state) {
                                        ready(device, state.value_or(0));
                                      });
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("hotspot device lookup failed: {}", e.what());
  }
}

void NmHotspot::startActivateOnInterface() {
  m_deviceTimeout.stop();
  m_apDeviceWatch.reset();
  const std::weak_ptr<int> alive = m_lifetime;
  const auto settle = [this, alive](std::optional<std::string> error, const std::string& active = {}) {
    if (!alive.expired()) {
      finishActivate(std::move(error), active);
    }
  };
  // A volatile copy of the profile, bound to the new interface on the client's channel; NetworkManager deletes
  // it when it goes down, and the saved profile stays unchanged.
  const auto activateCopy = [this, alive, settle](Settings settings) {
    if (alive.expired()) {
      return;
    }
    settings["connection"].erase("uuid");
    settings["connection"]["interface-name"] = sdbus::Variant{m_ap.interface};
    settings["connection"]["autoconnect"] = sdbus::Variant{false};
    settings["802-11-wireless"].erase("mac-address");
    settings["802-11-wireless"]["band"] = sdbus::Variant{std::string(m_ap.channel.band)};
    settings["802-11-wireless"]["channel"] = sdbus::Variant{m_ap.channel.channel};
    const std::map<std::string, sdbus::Variant> options{{"persist", sdbus::Variant{std::string("volatile")}}};
    try {
      m_nm->callMethodAsync("AddAndActivateConnection2")
          .onInterface(kNmInterface)
          .withArguments(settings, sdbus::ObjectPath{m_ap.device}, sdbus::ObjectPath{"/"}, options)
          .uponReplyInvoke([settle](std::optional<sdbus::Error> reply, sdbus::ObjectPath /*profile*/,
                                    sdbus::ObjectPath active, std::map<std::string, sdbus::Variant> /*result*/) {
            settle(reply.has_value() ? std::optional<std::string>(reply->what()) : std::nullopt, active);
          });
    } catch (const sdbus::Error& e) {
      settle(e.what());
    }
  };
  findProfile(m_bus, [this, alive, settle, activateCopy](std::optional<std::string> error, std::string profile) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      settle(error);
      return;
    }
    try {
      if (profile.empty()) {
        // Saves the new profile so its key stays the same next time.
        Settings settings = newProfile();
        auto store = system1(m_bus);
        store->callMethodAsync("AddConnection")
            .onInterface(kSystem1NetworkInterface)
            .withArguments(settings)
            .uponReplyInvoke([store](std::optional<sdbus::Error> reply, sdbus::ObjectPath /*profile*/) {
              if (reply.has_value()) {
                kLog.warn("hotspot profile not saved: {}", reply->what());
              }
            });
        activateCopy(std::move(settings));
        return;
      }
      auto connection = proxy(m_bus, profile);
      connection->callMethodAsync("GetSettings")
          .onInterface(kConnectionInterface)
          .uponReplyInvoke([connection, settle, activateCopy](std::optional<sdbus::Error> reply, Settings settings) {
            if (reply.has_value()) {
              settle(reply->what());
              return;
            }
            connection->callMethodAsync("GetSecrets")
                .onInterface(kConnectionInterface)
                .withArguments(std::string("802-11-wireless-security"))
                .uponReplyInvoke([connection, activateCopy, settings](std::optional<sdbus::Error> secretsError,
                                                                      const Settings& secrets) {
                  // Without the stored key NetworkManager asks a secret agent.
                  if (secretsError.has_value()) {
                    kLog.debug("hotspot secrets unavailable: {}", secretsError->what());
                  }
                  Settings merged = settings;
                  for (const auto& [setting, values] : secrets) {
                    for (const auto& [key, value] : values) {
                      merged[setting][key] = value;
                    }
                  }
                  activateCopy(std::move(merged));
                });
          });
    } catch (const sdbus::Error& e) {
      settle(e.what());
    }
  });
}

void NmHotspot::startRemoveInterface() {
  m_deviceTimeout.stop();
  m_apDeviceWatch.reset();
  const std::weak_ptr<int> alive = m_lifetime;
  const std::string ap = std::exchange(m_ap, {}).interface;
  const auto settle = [this, alive, ap](std::optional<std::string> error) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value()) {
      kLog.warn("hotspot interface {} not removed: {}", ap, *error);
    } else {
      kLog.info("hotspot interface {} removed", ap);
    }
    advance(m_machine.apply(error.has_value() ? HotspotEvent::RemoveFailed : HotspotEvent::InterfaceRemoved,
                            error.value_or("")));
    startQuery();
  };
  try {
    auto system = system1(m_bus);
    system->callMethodAsync("RemoveApInterface")
        .onInterface(kSystem1NetworkInterface)
        .withArguments(ap)
        .uponReplyInvoke([system, settle](std::optional<sdbus::Error> error) {
          settle(error.has_value() ? std::optional<std::string>(error->what()) : std::nullopt);
        });
  } catch (const sdbus::Error& e) {
    settle(e.what());
  }
}

#include "system/keyboard_backlight_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/clamp.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("keyboard-backlight");

  // andromeda-system (docs/system-health.md) wraps UPower's keyboard backlights; the shell reads only its API.
  const sdbus::ServiceName kUpowerBusName{"org.andromeda.System1"};
  const sdbus::ObjectPath kUpowerPath{"/org/andromeda/System1"};
  constexpr auto kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kKbdBacklightInterface = "org.andromeda.System1.KbdBacklight";
  constexpr std::string_view kKbdBacklightPathPrefix = "/org/andromeda/System1/KbdBacklight/";

  bool isKbdBacklightPath(const sdbus::ObjectPath& path) {
    return std::string_view{path}.starts_with(kKbdBacklightPathPrefix);
  }

  std::optional<std::string> rejectArgs(std::string_view command, const std::string& args) {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return "error: " + std::string(command) + " takes no arguments\n";
  }

  std::optional<int> parseInt(const std::string& s) {
    int value = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec == std::errc() && result.ptr == s.data() + s.size()) {
      return value;
    }
    return std::nullopt;
  }

} // namespace

struct KeyboardBacklightService::Device {
  std::string path;
  std::unique_ptr<sdbus::IProxy> proxy;
  int brightness = 0;
  int maxBrightness = 0;
};

KeyboardBacklightService::KeyboardBacklightService(SystemBus& bus) : m_bus(bus) {
  m_upowerProxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, kUpowerPath);
  m_upowerProxy->uponSignal("InterfacesAdded")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath& path, const std::map<std::string, std::map<std::string, sdbus::Variant>>&) {
        if (isKbdBacklightPath(path)) {
          rescanDevices();
        }
      });
  m_upowerProxy->uponSignal("InterfacesRemoved")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath& path, const std::vector<std::string>&) {
        if (isKbdBacklightPath(path)) {
          rescanDevices();
        }
      });
  rescanDevices();
}

KeyboardBacklightService::~KeyboardBacklightService() = default;

struct KeyboardBacklightService::Batch {
  std::vector<std::unique_ptr<Device>> devices;
  std::vector<bool> failed;
  int remaining = 0;
};

void KeyboardBacklightService::rescanDevices() {
  m_batch.reset();
  const std::weak_ptr<int> aliveToken = m_aliveToken;
  using ManagedObjects = std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>>;
  m_upowerProxy->callMethodAsync("GetManagedObjects")
      .onInterface(kObjectManagerInterface)
      .uponReplyInvoke([this, aliveToken](std::optional<sdbus::Error> e, const ManagedObjects& objects) {
        if (aliveToken.expired()) {
          return;
        }
        if (e.has_value()) {
          kLog.warn("failed to enumerate keyboard backlights: {}", e->what());
          return;
        }
        std::vector<sdbus::ObjectPath> paths;
        for (const auto& [path, interfaces] : objects) {
          if (interfaces.contains(kKbdBacklightInterface)) {
            paths.push_back(path);
          }
        }
        probeDevices(paths);
      });
}

void KeyboardBacklightService::probeDevices(const std::vector<sdbus::ObjectPath>& paths) {
  auto batch = std::make_shared<Batch>();
  m_batch = batch;
  const std::weak_ptr<int> aliveToken = m_aliveToken;
  const std::weak_ptr<Batch> weakBatch = batch;

  const auto settle = [this, aliveToken, weakBatch](std::size_t index, std::optional<sdbus::Error> e) {
    const auto live = weakBatch.lock();
    if (aliveToken.expired() || live == nullptr) {
      return;
    }
    if (e.has_value()) {
      live->failed[index] = true;
      kLog.warn("failed to initialize keyboard backlight {}: {}", live->devices[index]->path, e->what());
    }
    if (--live->remaining == 0) {
      commitDevices(*live);
    }
  };

  batch->remaining = static_cast<int>(paths.size()) * 2;
  batch->failed.assign(paths.size(), false);
  for (std::size_t i = 0; i < paths.size(); ++i) {
    auto device = std::make_unique<Device>();
    device->path = paths[i];
    device->proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, paths[i]);
    batch->devices.push_back(std::move(device));
  }
  if (paths.empty()) {
    commitDevices(*batch);
    return;
  }
  for (std::size_t i = 0; i < paths.size(); ++i) {
    auto& device = *batch->devices[i];
    device.proxy->getPropertyAsync("MaxBrightness")
        .onInterface(kKbdBacklightInterface)
        .uponReplyInvoke([settle, weakBatch, i](std::optional<sdbus::Error> e, sdbus::Variant value) {
          if (const auto live = weakBatch.lock(); live != nullptr && !e.has_value()) {
            live->devices[i]->maxBrightness = value.get<std::int32_t>();
          }
          settle(i, std::move(e));
        });
    device.proxy->getPropertyAsync("Brightness")
        .onInterface(kKbdBacklightInterface)
        .uponReplyInvoke([settle, weakBatch, i](std::optional<sdbus::Error> e, sdbus::Variant value) {
          if (const auto live = weakBatch.lock(); live != nullptr && !e.has_value()) {
            live->devices[i]->brightness = value.get<std::int32_t>();
          }
          settle(i, std::move(e));
        });
  }
}

void KeyboardBacklightService::commitDevices(Batch& batch) {
  std::vector<std::unique_ptr<Device>> devices;
  devices.reserve(batch.devices.size());
  for (std::size_t i = 0; i < batch.devices.size(); ++i) {
    auto& device = batch.devices[i];
    if (batch.failed[i]) {
      continue;
    }
    if (device->maxBrightness < 0) {
      kLog.warn("keyboard backlight {} reported invalid maximum {}", device->path, device->maxBrightness);
      continue;
    }
    device->brightness = util::clampOrdered(device->brightness, 0, device->maxBrightness);

    const std::string devicePath = device->path;
    device->proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, devicePath](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          const auto property = changedProperties.find("Brightness");
          if (interfaceName != kKbdBacklightInterface || property == changedProperties.end()) {
            return;
          }
          const auto value = property->second.get<std::int32_t>();
          const auto it = std::ranges::find_if(m_devices, [&devicePath](const auto& candidate) {
            return candidate->path == devicePath;
          });
          if (it == m_devices.end()) {
            return;
          }
          auto& changed = **it;
          const int brightness = util::clampOrdered<int>(value, 0, changed.maxBrightness);
          if (brightness == changed.brightness) {
            return;
          }
          changed.brightness = brightness;
          publishBrightness(changed);
        });
    devices.push_back(std::move(device));
  }

  m_devices = std::move(devices);
  m_batch.reset();
  if (m_devices.empty()) {
    m_brightness = 0;
    m_maxBrightness = 0;
    kLog.info("no keyboard backlights available");
    return;
  }

  m_brightness = m_devices.front()->brightness;
  m_maxBrightness = m_devices.front()->maxBrightness;
  kLog.info("keyboard backlight service active ({} device(s))", m_devices.size());
}

void KeyboardBacklightService::publishBrightness(const Device& device) {
  m_brightness = device.brightness;
  m_maxBrightness = device.maxBrightness;
  if (m_changeCallback) {
    m_changeCallback();
  }
}

bool KeyboardBacklightService::setBrightness(Device& device, int value) {
  const int clamped = util::clampOrdered(value, 0, device.maxBrightness);
  try {
    device.proxy->callMethod("SetBrightness").onInterface(kKbdBacklightInterface).withArguments(clamped);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to set keyboard backlight {}: {}", device.path, e.what());
    return false;
  }
  if (device.brightness != clamped) {
    device.brightness = clamped;
    publishBrightness(device);
  }
  return true;
}

bool KeyboardBacklightService::setPercent(int percent) {
  bool success = true;
  for (auto& device : m_devices) {
    const int raw = static_cast<int>(std::round(percent / 100.0 * device->maxBrightness));
    if (!setBrightness(*device, raw)) {
      success = false;
    }
  }
  return success;
}

bool KeyboardBacklightService::adjustBrightness(int delta) {
  bool success = true;
  for (auto& device : m_devices) {
    if (!setBrightness(*device, device->brightness + delta)) {
      success = false;
    }
  }
  return success;
}

bool KeyboardBacklightService::toggleBrightness() {
  bool success = true;
  for (auto& device : m_devices) {
    if (!setBrightness(*device, device->brightness > 0 ? 0 : device->maxBrightness)) {
      success = false;
    }
  }
  return success;
}

void KeyboardBacklightService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void KeyboardBacklightService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::keyboardBacklightSet, [this](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() != 1) {
      return "error: keyboard-backlight-set requires <value>\n";
    }
    const auto parsed = parseInt(parts[0]);
    if (!parsed.has_value() || *parsed < 0 || *parsed > 100) {
      return "error: invalid keyboard backlight value (use 0-100 for percentage)\n";
    }
    if (!available()) {
      return "error: keyboard backlight unavailable\n";
    }
    return setPercent(*parsed) ? "ok\n" : "error: failed to set keyboard backlight\n";
  });

  ipc.bind(noctalia::cli::msg::keyboardBacklightUp, [this](const std::string& args) -> std::string {
    if (auto err = rejectArgs("keyboard-backlight-up", args); err.has_value()) {
      return *err;
    }
    if (!available()) {
      return "error: keyboard backlight unavailable\n";
    }
    return adjustBrightness(1) ? "ok\n" : "error: failed to set keyboard backlight\n";
  });

  ipc.bind(noctalia::cli::msg::keyboardBacklightDown, [this](const std::string& args) -> std::string {
    if (auto err = rejectArgs("keyboard-backlight-down", args); err.has_value()) {
      return *err;
    }
    if (!available()) {
      return "error: keyboard backlight unavailable\n";
    }
    return adjustBrightness(-1) ? "ok\n" : "error: failed to set keyboard backlight\n";
  });

  ipc.bind(noctalia::cli::msg::keyboardBacklightToggle, [this](const std::string& args) -> std::string {
    if (auto err = rejectArgs("keyboard-backlight-toggle", args); err.has_value()) {
      return *err;
    }
    if (!available()) {
      return "error: keyboard backlight unavailable\n";
    }
    return toggleBrightness() ? "ok\n" : "error: failed to set keyboard backlight\n";
  });
}

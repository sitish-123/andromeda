#include "dbus/upower/upower_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "dbus/upower/upower_charge_limit_support.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  // andromeda-system (docs/system-health.md) wraps UPower; the shell reads only its API.
  const sdbus::ServiceName kSystemBusName{"org.andromeda.System1"};
  const sdbus::ObjectPath kRootPath{"/org/andromeda/System1"};
  const sdbus::ObjectPath kPowerPath{"/org/andromeda/System1/Power"};
  constexpr auto kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";
  constexpr auto kPowerInterface = "org.andromeda.System1.Power";
  constexpr auto kDeviceInterface = "org.andromeda.System1.Battery";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr std::string_view kDisplayDeviceSuffix = "/DisplayDevice";
  // Kind and State strings in UPowerDeviceType and BatteryState enum order.
  constexpr std::array<std::string_view, 29> kKinds{
      "unknown",  "line-power", "battery",     "ups",          "monitor",        "mouse",        "keyboard",
      "pda",      "phone",      "media-player", "tablet",      "computer",       "gaming-input", "pen",
      "touchpad", "modem",      "network",     "headset",      "speakers",       "headphones",   "video",
      "other-audio", "remote-control", "printer", "scanner",   "camera",         "wearable",     "toy",
      "bluetooth-generic",
  };
  constexpr std::array<std::string_view, 7> kStates{
      "unknown", "charging", "discharging", "empty", "fully-charged", "pending-charge", "pending-discharge",
  };

  template <std::size_t N> std::uint32_t indexOf(const std::array<std::string_view, N>& names, std::string_view name) {
    const auto it = std::ranges::find(names, name);
    return it == names.end() ? 0U : static_cast<std::uint32_t>(it - names.begin());
  }

} // namespace

std::string batteryStateLabel(BatteryState state) {
  switch (state) {
  case BatteryState::Charging:
    return i18n::tr("power.battery.states.charging");
  case BatteryState::Discharging:
    return i18n::tr("power.battery.states.discharging");
  case BatteryState::FullyCharged:
    return i18n::tr("power.battery.states.plugged-in");
  case BatteryState::Empty:
    return i18n::tr("power.battery.states.empty");
  case BatteryState::PendingCharge:
    return i18n::tr("power.battery.states.plugged-in");
  case BatteryState::PendingDischarge:
    return i18n::tr("power.battery.states.pending-discharge");
  case BatteryState::Unknown:
  default:
    return i18n::tr("power.battery.states.battery");
  }
}

std::optional<bool> batteryStatePlugged(BatteryState state) {
  switch (state) {
  case BatteryState::Charging:
  case BatteryState::FullyCharged:
  case BatteryState::PendingCharge:
    return true;
  case BatteryState::Discharging:
  case BatteryState::PendingDischarge:
    return false;
  case BatteryState::Unknown:
  case BatteryState::Empty:
    return std::nullopt;
  }
  return std::nullopt;
}

const char* batteryGlyphName(double percentage, BatteryState state) {
  if (state == BatteryState::Charging) {
    return "battery-charging";
  }
  if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
    return "battery-plugged";
  }
  if (state == BatteryState::Unknown && percentage <= 0.0) {
    return "battery-exclamation";
  }
  if (percentage >= 85.0) {
    return "battery-4";
  }
  if (percentage >= 55.0) {
    return "battery-3";
  }
  if (percentage >= 30.0) {
    return "battery-2";
  }
  if (percentage >= 10.0) {
    return "battery-1";
  }
  return "battery-0";
}

const char* batteryDeviceGlyphName(UPowerDeviceType type) {
  switch (type) {
  case UPowerDeviceType::Mouse:
    return "mouse-2";
  case UPowerDeviceType::Keyboard:
    return "keyboard";
  case UPowerDeviceType::Phone:
  case UPowerDeviceType::Pda:
    return "device-mobile";
  default:
    return "bluetooth";
  }
}

namespace {

  using Props = std::map<std::string, sdbus::Variant>;

  template <typename T> std::optional<T> optProp(const Props& props, std::string_view name) {
    const auto it = props.find(std::string(name));
    if (it == props.end()) {
      return std::nullopt;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  template <typename T> T propOr(const Props& props, std::string_view name, T fallback) {
    return optProp<T>(props, name).value_or(std::move(fallback));
  }

  void merge(Props& props, const Props& changed) {
    for (const auto& [name, value] : changed) {
      props.insert_or_assign(name, value);
    }
  }

  std::optional<std::uint32_t> thresholdPercent(std::uint32_t value) {
    return value > 100U ? std::nullopt : std::optional<std::uint32_t>{value};
  }

  bool chargeLimitIsExternallyManaged(const UPowerChargeLimitState& state) {
    return state.enabledAvailable && !state.enabled && state.hasRestrictiveThreshold();
  }

  bool sameDeviceInfoExceptChargeLimit(const UPowerDeviceInfo& lhs, const UPowerDeviceInfo& rhs) {
    auto lhsWithoutChargeLimit = lhs;
    auto rhsWithoutChargeLimit = rhs;
    lhsWithoutChargeLimit.chargeLimit = {};
    rhsWithoutChargeLimit.chargeLimit = {};
    return lhsWithoutChargeLimit == rhsWithoutChargeLimit;
  }

  bool chargeLimitPropertiesChanged(
      const std::map<std::string, sdbus::Variant>& changed, const std::vector<std::string>& invalidated
  ) {
    if (changed.empty() && invalidated.empty()) {
      return true;
    }

    constexpr std::array<std::string_view, 4> properties{"NativePath", "Kind", "PowerSupply", "ChargeLimit"};
    return std::ranges::any_of(properties, [&changed, &invalidated](std::string_view property) {
      return std::ranges::any_of(changed, [property](const auto& entry) { return entry.first == property; })
          || std::ranges::find(invalidated, property) != invalidated.end();
    });
  }

  bool isAuthorizationError(const sdbus::Error& error) {
    const auto& name = error.getName();
    if (name == sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}
        || name == sdbus::Error::Name{"org.freedesktop.PolicyKit1.Error.NotAuthorized"}
        || name == sdbus::Error::Name{"org.freedesktop.UPower.Device.PermissionDenied"}) {
      return true;
    }

    // UPower reports PolicyKit rejection as GeneralError with this canonical message. Its error
    // name is shared with unrelated hardware failures, so only classify recognizable denial text.
    const std::string message = StringUtils::toLower(error.what());
    return message.contains("operation is not allowed")
        || message.contains("not authorized")
        || message.contains("authorization")
        || message.contains("authentication")
        || message.contains("access denied");
  }

  bool isBatteryCapableDeviceType(UPowerDeviceType type) {
    return type != UPowerDeviceType::Unknown && type != UPowerDeviceType::LinePower;
  }

  // A battery belonging to a peripheral rather than to the system. UPower reports peripheral packs
  // with PowerSupply=false, so only PowerSupply batteries and UPS units power the machine itself.
  bool isPeripheralBattery(const UPowerDeviceInfo& info) {
    return info.isPresent
        && isBatteryCapableDeviceType(info.type)
        && !info.isLaptopBattery()
        && info.type != UPowerDeviceType::Ups;
  }

  bool isAutoSelector(std::string_view selector) {
    const std::string normalized = StringUtils::toLower(StringUtils::trim(selector));
    return normalized.empty() || normalized == "auto";
  }

  bool hasSelectorSuffix(std::string_view value, std::string_view selector) {
    if (value.empty() || selector.empty() || value.size() < selector.size()) {
      return false;
    }
    const std::size_t start = value.size() - selector.size();
    if (value.substr(start) != selector) {
      return false;
    }
    if (start == 0) {
      return true;
    }
    const char before = value[start - 1];
    return before == '/' || before == '_' || before == '-' || before == ':' || before == '.';
  }

  bool selectorMatchesField(const std::string& value, std::string_view selector) {
    return std::string_view(value) == selector || hasSelectorSuffix(value, selector);
  }

  BatteryState decodeBatteryState(std::string_view name) {
    const std::uint32_t raw = indexOf(kStates, name);
    if (raw >= 1 && raw <= 6) {
      return static_cast<BatteryState>(raw);
    }
    return BatteryState::Unknown;
  }

  UPowerState deviceState(const Props& props, bool onBattery) {
    UPowerState next;
    next.onBattery = onBattery;
    next.percentage = propOr<double>(props, "Percent", 0.0);
    next.isPresent = propOr<bool>(props, "Present", false);
    next.state = decodeBatteryState(propOr<std::string>(props, "State", ""));
    next.timeToEmpty = propOr<std::int64_t>(props, "TimeToEmptyS", 0);
    next.timeToFull = propOr<std::int64_t>(props, "TimeToFullS", 0);
    next.energyRate = propOr<double>(props, "RateW", 0.0);
    next.energy = propOr<double>(props, "EnergyWh", 0.0);

    // Fallback calculation for timeToEmpty / timeToFull if they are reported as 0 or less
    if (next.state == BatteryState::Discharging && next.timeToEmpty <= 0 && next.energyRate > 0.0 && next.energy > 0.0) {
      next.timeToEmpty = static_cast<std::int64_t>(std::round((next.energy / next.energyRate) * 3600.0));
    } else if (next.state == BatteryState::Charging && next.timeToFull <= 0 && next.energyRate > 0.0) {
      const auto energyFull = propOr<double>(props, "EnergyFullWh", 0.0);
      if (energyFull > next.energy) {
        next.timeToFull = static_cast<std::int64_t>(std::round(((energyFull - next.energy) / next.energyRate) * 3600.0));
      }
    }
    return next;
  }

  constexpr Logger kLog("upower");

  UPowerDeviceInfo makeDummyBatteryDevice() {
    UPowerDeviceInfo info;
    info.path = "/org/freedesktop/UPower/devices/dummy_battery";
    info.nativePath = "dummy_BAT0";
    info.model = "Dummy Battery";
    info.type = UPowerDeviceType::Battery;
    info.powerSupply = true;
    info.isPresent = true;
    info.energyFull = 54.0;
    info.energyFullDesign = 54.0;
    info.state.percentage = 67.0;
    info.state.energyRate = 12.5;
    info.state.state = BatteryState::Discharging;
    info.state.timeToEmpty = 3 * 3600 + 15 * 60;
    info.state.energy = 36.2;
    info.state.isPresent = true;
    info.state.onBattery = true;
    return info;
  }

} // namespace

// Battery properties by object path, from GetManagedObjects and PropertiesChanged: andromeda-system sends values
// in its signals, so the shell never blocks on a property read.
struct UPowerService::PropertyCache {
  std::map<std::string, Props> byPath;

  [[nodiscard]] const Props& at(const std::string& path) const {
    static const Props kEmpty;
    const auto it = byPath.find(path);
    return it != byPath.end() ? it->second : kEmpty;
  }
};

std::optional<double> UPowerDeviceInfo::healthPercent() const {
  if (energyFullDesign <= 0.0 || energyFull <= 0.0) {
    return std::nullopt;
  }
  return std::clamp(energyFull / energyFullDesign * 100.0, 0.0, 100.0);
}

bool UPowerChargeLimitState::hasRestrictiveThreshold() const {
  // Huawei-WMI EC expresses "no limit / full charge" as start=95 end=100 rather than 0/100.
  if (effectiveStart == 95U && effectiveEnd == 100U) {
    return false;
  }
  return (effectiveStart.has_value() && *effectiveStart > 0U && *effectiveStart < 100U)
      || (effectiveEnd.has_value() && *effectiveEnd < 100U);
}

bool UPowerDeviceInfo::sameCatalogEntry(const UPowerDeviceInfo& other) const {
  return path == other.path
      && nativePath == other.nativePath
      && vendor == other.vendor
      && model == other.model
      && serial == other.serial
      && type == other.type
      && powerSupply == other.powerSupply
      && isPresent == other.isPresent;
}

bool upowerDeviceMatchesSelector(const UPowerDeviceInfo& info, std::string_view selector) {
  const std::string trimmed = StringUtils::trim(selector);
  if (trimmed.empty()) {
    return false;
  }
  return selectorMatchesField(info.path, trimmed)
      || selectorMatchesField(info.nativePath, trimmed)
      || selectorMatchesField(info.model, trimmed)
      || selectorMatchesField(info.serial, trimmed)
      || selectorMatchesField(info.vendor, trimmed);
}

UPowerService::UPowerService(SystemBus& bus) : m_bus(bus), m_props(std::make_unique<PropertyCache>()) {
  m_upowerProxy = sdbus::createProxy(m_bus.connection(), kSystemBusName, kRootPath);
  m_powerProxy = sdbus::createProxy(m_bus.connection(), kSystemBusName, kPowerPath);

  m_powerProxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const Props& changed, const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName == kPowerInterface) {
          m_onBattery = propOr<bool>(changed, "OnBattery", m_onBattery);
          (void)refreshDefaultState();
        }
      });

  m_upowerProxy->uponSignal("InterfacesAdded")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath&, const std::map<std::string, std::map<std::string, sdbus::Variant>>&) {
        rescanDevices();
      });

  m_upowerProxy->uponSignal("InterfacesRemoved")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath& path, const std::vector<std::string>&) {
        invalidateChargeThresholdRequest(std::string(path));
        rescanDevices();
      });

  if (SysUtils::isEnvFlagOn("NOCTALIA_DUMMY_BATTERY")) {
    m_dummyDevice = makeDummyBatteryDevice();
    kLog.info("dummy battery enabled ({:.0F}% discharging)", m_dummyDevice->state.percentage);
  }

  rescanDevices();
}

UPowerService::~UPowerService() { m_lifetimeToken.reset(); }

void UPowerService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void UPowerService::refresh() { rescanDevices(); }

std::vector<UPowerDeviceInfo> UPowerService::batteryDevices() const {
  std::vector<UPowerDeviceInfo> devices;
  devices.reserve(m_devices.size() + (m_dummyDevice ? 1 : 0));
  for (const auto& device : m_devices) {
    if (device.info.isPresent && isBatteryCapableDeviceType(device.info.type)) {
      devices.push_back(device.info);
    }
  }
  if (m_dummyDevice && m_dummyDevice->isPresent) {
    devices.push_back(*m_dummyDevice);
  }
  return devices;
}

UPowerState UPowerService::stateForDevice(std::string_view selector) const {
  if (isAutoSelector(selector)) {
    return m_state;
  }

  if (const auto* device = deviceForSelector(selector); device != nullptr) {
    return device->state;
  }

  UPowerState missing;
  missing.onBattery = m_onBattery;
  return missing;
}

void UPowerService::rescanDevices() {
  if (m_rescanInFlight) {
    m_rescanQueued = true;
    return;
  }
  m_rescanInFlight = true;

  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  using ManagedObjects = std::map<sdbus::ObjectPath, std::map<std::string, Props>>;
  m_upowerProxy->callMethodAsync("GetManagedObjects")
      .onInterface(kObjectManagerInterface)
      .uponReplyInvoke([this, lifetimeToken](std::optional<sdbus::Error> err, ManagedObjects objects) {
        if (lifetimeToken.expired()) {
          return;
        }
        m_rescanInFlight = false;
        if (err.has_value()) {
          kLog.warn("andromeda-system GetManagedObjects failed: {}", err->what());
          refreshDisplayDeviceProxy(std::nullopt);
          if (!finishFirstSnapshot()) {
            (void)refreshDefaultState();
          }
        } else {
          std::optional<std::string> display;
          std::vector<std::string> paths;
          m_props->byPath.clear();
          for (auto& [path, interfaces] : objects) {
            if (const auto power = interfaces.find(kPowerInterface); path == kPowerPath && power != interfaces.end()) {
              m_onBattery = propOr<bool>(power->second, "OnBattery", false);
            }
            const auto device = interfaces.find(kDeviceInterface);
            if (device == interfaces.end()) {
              continue;
            }
            m_props->byPath.insert_or_assign(std::string(path), std::move(device->second));
            if (std::string_view(path).ends_with(kDisplayDeviceSuffix)) {
              display = std::string(path);
            } else {
              paths.emplace_back(path);
            }
          }
          refreshDisplayDeviceProxy(display);
          applyEnumeratedDevices(paths);
        }
        if (m_rescanQueued) {
          m_rescanQueued = false;
          rescanDevices();
        }
      });
}

bool UPowerService::finishFirstSnapshot() {
  if (!m_firstSnapshotPending) {
    return false;
  }
  m_firstRescanDone = true;
  m_firstSnapshotPending = false;
  m_state = readDefaultState();
  if (m_state.isPresent) {
    kLog.info(
        "battery {:.0F}% state={} ({})", m_state.percentage, static_cast<int>(m_state.state),
        m_state.onBattery ? "on battery" : "on AC"
    );
  } else {
    kLog.info("connected (no system battery present)");
  }
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::Initial});
  }
  return true;
}

void UPowerService::applyEnumeratedDevices(const std::vector<std::string>& paths) {
  std::vector<TrackedDevice> nextDevices;
  nextDevices.reserve(paths.size());
  for (const auto& path : paths) {
    try {
      const std::string& devicePath = path;
      const auto previous = std::ranges::find_if(m_devices, [&devicePath](const TrackedDevice& device) {
        return device.info.path == devicePath;
      });
      std::optional<bool> knownMethod =
          previous != m_devices.end() ? previous->chargeThresholdMethodAvailable : std::nullopt;
      auto proxy = sdbus::createProxy(m_bus.connection(), kSystemBusName, sdbus::ObjectPath{path});
      auto info = readDeviceInfo(devicePath, knownMethod);
      if (!isBatteryCapableDeviceType(info.type)) {
        continue;
      }

      const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
      proxy->uponSignal("PropertiesChanged")
          .onInterface(kPropertiesInterface)
          .call([this, lifetimeToken, devicePath](
                    const std::string& interfaceName, const Props& changed, const std::vector<std::string>& invalidated
                ) {
            if (!lifetimeToken.expired() && interfaceName == kDeviceInterface) {
              merge(m_props->byPath[devicePath], changed);
              const auto changes = refreshDevice(devicePath, chargeLimitPropertiesChanged(changed, invalidated));
              emitChangedIfNeeded(changes.devicesChanged, changes.chargeLimitChanged, changes.deviceCatalogChanged);
            }
          });

      std::optional<std::uint64_t> requestId;
      if (previous != m_devices.end()) {
        info.chargeLimit.requestPending = previous->info.chargeLimit.requestPending;
        info.chargeLimit.requestedEnabled = previous->info.chargeLimit.requestedEnabled;
        info.chargeLimit.operationError = previous->info.chargeLimit.operationError;
        requestId = previous->chargeThresholdRequestId;
      }
      nextDevices.push_back(
          TrackedDevice{std::move(info), std::shared_ptr<sdbus::IProxy>(std::move(proxy)), knownMethod, requestId}
      );
    } catch (const sdbus::Error&) {
      continue;
    }
  }

  std::ranges::sort(nextDevices, [](const TrackedDevice& lhs, const TrackedDevice& rhs) {
    return lhs.info.path < rhs.info.path;
  });

  bool devicesChanged = m_devices.size() != nextDevices.size();
  bool chargeLimitChanged = false;
  bool deviceCatalogChanged = devicesChanged;
  if (!devicesChanged) {
    for (std::size_t i = 0; i < m_devices.size(); ++i) {
      if (!m_devices[i].info.sameCatalogEntry(nextDevices[i].info)) {
        deviceCatalogChanged = true;
      }
      if (!sameDeviceInfoExceptChargeLimit(m_devices[i].info, nextDevices[i].info)) {
        devicesChanged = true;
      }
      chargeLimitChanged = chargeLimitChanged || m_devices[i].info.chargeLimit != nextDevices[i].info.chargeLimit;
    }
  }
  m_devices = std::move(nextDevices);
  if (deviceCatalogChanged) {
    kLog.debug("tracking {} UPower battery-capable device(s)", m_devices.size());
  }
  if (finishFirstSnapshot()) {
    return;
  }
  if (devicesChanged) {
    emitChangedIfNeeded(true, chargeLimitChanged, deviceCatalogChanged);
  } else if (!refreshDefaultState()) {
    emitChangedIfNeeded(false, chargeLimitChanged, deviceCatalogChanged);
  }
}

UPowerState UPowerService::readDefaultState() const {
  UPowerState next;
  const bool onBattery = m_onBattery;
  next.onBattery = onBattery;

  if (m_displayDeviceProxy != nullptr) {
    next = deviceState(m_props->at(m_displayDevicePath), onBattery);
    if (next.isPresent) {
      return next;
    }
  }

  const auto* device = defaultSystemBattery();
  if (device == nullptr) {
    return next;
  }

  next = device->state;
  if (m_dummyDevice && device == &*m_dummyDevice) {
    return next;
  }
  next.onBattery = onBattery;
  return next;
}

UPowerDeviceInfo UPowerService::readDeviceInfoBase(std::string path) const {
  const Props& props = m_props->at(path);
  UPowerDeviceInfo info;
  info.path = std::move(path);
  info.nativePath = propOr<std::string>(props, "NativePath", "");
  info.vendor = propOr<std::string>(props, "Vendor", "");
  info.model = propOr<std::string>(props, "Model", "");
  info.serial = propOr<std::string>(props, "Serial", "");
  info.type = static_cast<UPowerDeviceType>(indexOf(kKinds, propOr<std::string>(props, "Kind", "")));
  info.powerSupply = propOr<bool>(props, "PowerSupply", false);
  info.energyFull = propOr<double>(props, "EnergyFullWh", 0.0);
  info.energyFullDesign = propOr<double>(props, "EnergyFullDesignWh", 0.0);
  info.chargeCycles = propOr<std::int32_t>(props, "Cycles", -1);
  info.state = deviceState(props, m_onBattery);
  info.isPresent = info.state.isPresent;
  return info;
}

UPowerChargeLimitState UPowerService::readChargeLimitState(
    const UPowerDeviceInfo& info, std::optional<bool>& chargeThresholdMethodAvailable
) const {
  UPowerChargeLimitState state;
  if (!info.isLaptopBattery()) {
    chargeThresholdMethodAvailable.reset();
    return state;
  }

  using ChargeLimit = sdbus::Struct<bool, bool, std::uint32_t, std::uint32_t, std::uint32_t>;
  const auto limit = optProp<ChargeLimit>(m_props->at(info.path), "ChargeLimit");
  if (!limit.has_value()) {
    return state;
  }
  const auto& [supported, enabled, start, end, settings] = *limit;
  state.supported = supported;
  chargeThresholdMethodAvailable = supported;
  state.methodAvailable = supported;
  state.enabledAvailable = true;
  state.enabled = enabled;
  state.supportedSettings = settings;
  if ((settings & 1U) != 0U) {
    state.configuredStart = thresholdPercent(start);
  }
  if ((settings & 2U) != 0U) {
    state.configuredEnd = thresholdPercent(end);
  }
  const auto effective = upower::detail::readChargeThresholdsFromSysfs(info.nativePath);
  state.effectiveStart = effective.start;
  state.effectiveEnd = effective.end;
  return state;
}

UPowerDeviceInfo
UPowerService::readDeviceInfo(std::string path, std::optional<bool>& chargeThresholdMethodAvailable) const {
  auto info = readDeviceInfoBase(std::move(path));
  info.chargeLimit = readChargeLimitState(info, chargeThresholdMethodAvailable);
  return info;
}

void UPowerService::requestHistory(
    std::string_view devicePath, std::string_view type, std::uint32_t timespanSeconds, std::uint32_t resolution,
    std::function<void(std::vector<UPowerHistoryPoint>)> done
) {
  const auto it = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& d) { return d.info.path == devicePath; });
  if (it == m_devices.end() || it->proxy == nullptr) {
    done({});
    return;
  }
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    it->proxy->callMethodAsync("GetHistory")
        .onInterface(kDeviceInterface)
        .withArguments(std::string(type), timespanSeconds, resolution)
        .uponReplyInvoke([lifetimeToken, done = std::move(done), keepAlive = it->proxy](
                             std::optional<sdbus::Error> error,
                             std::vector<sdbus::Struct<std::uint32_t, double, std::uint32_t>> items
                         ) {
          (void)keepAlive;
          if (lifetimeToken.expired()) {
            return;
          }
          std::vector<UPowerHistoryPoint> points;
          if (!error) {
            points.reserve(items.size());
            for (const auto& item : items) {
              const std::uint32_t raw = std::get<2>(item);
              points.push_back({
                  .time = std::get<0>(item),
                  .value = std::get<1>(item),
                  .state = raw <= 6 ? static_cast<BatteryState>(raw) : BatteryState::Unknown,
              });
            }
          }
          done(std::move(points));
        });
  } catch (const sdbus::Error&) {
    done({});
  }
}

bool UPowerService::enableChargeThreshold(std::string_view devicePath, bool enabled) {
  const auto it = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& device) {
    return device.info.path == devicePath;
  });
  if (it == m_devices.end()
      || !it->info.isLaptopBattery()
      || !it->info.chargeLimit.supported
      || !it->info.chargeLimit.methodAvailable
      || !it->info.chargeLimit.enabledAvailable
      || chargeLimitIsExternallyManaged(it->info.chargeLimit)
      || it->info.chargeLimit.requestPending) {
    return false;
  }

  const std::string path = it->info.path;
  const auto proxy = it->proxy;
  const std::uint64_t requestId = m_nextChargeThresholdRequestId++;
  it->chargeThresholdRequestId = requestId;
  auto& operation = it->info.chargeLimit;
  operation.requestPending = true;
  operation.requestedEnabled = enabled;
  operation.operationError = ChargeLimitOperationError::None;
  emitControlOperationChanged();

  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    // UINT32_MAX keeps the start and end values the battery has.
    proxy->callMethodAsync("SetChargeLimit")
        .onInterface(kDeviceInterface)
        .withArguments(enabled, UINT32_MAX, UINT32_MAX)
        .uponReplyInvoke([this, lifetimeToken, path, requestId, keepAlive = proxy](std::optional<sdbus::Error> error) {
          if (lifetimeToken.expired() || !chargeThresholdRequestIsCurrent(path, requestId)) {
            return;
          }
          auto finish = [this, path, requestId, error](std::optional<Props> props) {
            if (props.has_value()) {
              m_props->byPath.insert_or_assign(path, std::move(*props));
            }
            const auto current = std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) {
              return device.info.path == path;
            });
            if (current == m_devices.end() || current->chargeThresholdRequestId != requestId) {
              return;
            }
            auto changes = refreshTrackedDevice(*current, true);
            current->chargeThresholdRequestId.reset();
            current->info.chargeLimit.requestPending = false;
            current->info.chargeLimit.requestedEnabled.reset();
            current->info.chargeLimit.operationError = !error.has_value()
                ? ChargeLimitOperationError::None
                : (isAuthorizationError(*error) ? ChargeLimitOperationError::PermissionDenied
                                                : ChargeLimitOperationError::Failed);
            if (error.has_value()) {
              kLog.warn("charge threshold change failed device={} err={}", path, error->what());
            }
            changes.chargeLimitChanged = true;
            emitChangedIfNeeded(changes.devicesChanged, changes.chargeLimitChanged, changes.deviceCatalogChanged);
          };
          // Read the device again on every completion: UPower may have changed a subset of the hardware state
          // even when the method reports an error.
          try {
            keepAlive->callMethodAsync("GetAll")
                .onInterface(kPropertiesInterface)
                .withArguments(std::string(kDeviceInterface))
                .uponReplyInvoke([lifetimeToken, finish, keepAlive](std::optional<sdbus::Error> readError, Props props) {
                  if (!lifetimeToken.expired()) {
                    finish(readError.has_value() ? std::nullopt : std::optional<Props>{std::move(props)});
                  }
                });
          } catch (const sdbus::Error&) {
            finish(std::nullopt);
          }
        });
  } catch (const sdbus::Error& error) {
    const auto current =
        std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) { return device.info.path == path; });
    if (current != m_devices.end() && current->chargeThresholdRequestId == requestId) {
      current->chargeThresholdRequestId.reset();
      current->info.chargeLimit.requestPending = false;
      current->info.chargeLimit.requestedEnabled.reset();
      current->info.chargeLimit.operationError = ChargeLimitOperationError::Failed;
    }
    kLog.warn("charge threshold change dispatch failed device={} err={}", path, error.what());
    emitControlOperationChanged();
    return false;
  }
  return true;
}

const UPowerDeviceInfo* UPowerService::defaultSystemBattery() const noexcept {
  for (const auto& device : m_devices) {
    if (device.info.isLaptopBattery() && device.info.isPresent) {
      return &device.info;
    }
  }
  if (m_dummyDevice && m_dummyDevice->isLaptopBattery() && m_dummyDevice->isPresent) {
    return &*m_dummyDevice;
  }
  return nullptr;
}

const UPowerDeviceInfo* UPowerService::deviceForSelector(std::string_view selector) const {
  const std::string trimmed = StringUtils::trim(selector);
  if (trimmed.empty()) {
    return nullptr;
  }

  for (const auto& device : m_devices) {
    if (isBatteryCapableDeviceType(device.info.type) && upowerDeviceMatchesSelector(device.info, trimmed)) {
      return &device.info;
    }
  }
  if (m_dummyDevice
      && isBatteryCapableDeviceType(m_dummyDevice->type)
      && upowerDeviceMatchesSelector(*m_dummyDevice, trimmed)) {
    return &*m_dummyDevice;
  }
  return nullptr;
}

const UPowerDeviceInfo* UPowerService::peripheralBatteryForSerial(std::string_view serial) const {
  if (serial.empty()) {
    return nullptr;
  }
  for (const auto& device : m_devices) {
    if (isPeripheralBattery(device.info) && StringUtils::equalsInsensitive(device.info.serial, serial)) {
      return &device.info;
    }
  }
  return nullptr;
}

void UPowerService::refreshDisplayDeviceProxy(const std::optional<std::string>& displayPath) {
  if (!displayPath.has_value()) {
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
    return;
  }
  const std::string& nextPath = *displayPath;
  if (nextPath.empty() || nextPath == "/") {
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
    return;
  }
  if (m_displayDeviceProxy != nullptr && m_displayDevicePath == nextPath) {
    return;
  }

  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kSystemBusName, sdbus::ObjectPath{nextPath});
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, nextPath](
                  const std::string& interfaceName, const Props& changed, const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName == kDeviceInterface) {
            merge(m_props->byPath[nextPath], changed);
            (void)refreshDefaultState();
          }
        });

    m_displayDevicePath = nextPath;
    m_displayDeviceProxy = std::move(proxy);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to track UPower display device {}: {}", nextPath, e.what());
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
  }
}

bool UPowerService::refreshDefaultState() {
  if (m_firstSnapshotPending) {
    return false;
  }
  const UPowerState next = readDefaultState();
  if (next == m_state) {
    return false;
  }

  m_state = next;
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::DeviceState});
  }
  return true;
}

UPowerService::RefreshChanges UPowerService::refreshTrackedDevice(TrackedDevice& device, bool refreshChargeLimit) {
  RefreshChanges changes;
  auto next = readDeviceInfoBase(device.info.path);
  const bool laptopCapabilityChanged = next.isLaptopBattery() != device.info.isLaptopBattery();
  if (refreshChargeLimit || laptopCapabilityChanged) {
    next.chargeLimit = readChargeLimitState(next, device.chargeThresholdMethodAvailable);
    next.chargeLimit.requestPending = device.info.chargeLimit.requestPending;
    next.chargeLimit.requestedEnabled = device.info.chargeLimit.requestedEnabled;
    next.chargeLimit.operationError = device.info.chargeLimit.operationError;
  } else {
    next.chargeLimit = device.info.chargeLimit;
  }

  changes.devicesChanged = !sameDeviceInfoExceptChargeLimit(next, device.info);
  changes.chargeLimitChanged = next.chargeLimit != device.info.chargeLimit;
  changes.deviceCatalogChanged = !device.info.sameCatalogEntry(next);
  if (next != device.info) {
    device.info = std::move(next);
  }
  return changes;
}

UPowerService::RefreshChanges UPowerService::refreshDevice(std::string_view devicePath, bool refreshChargeLimit) {
  const auto device = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& candidate) {
    return candidate.info.path == devicePath;
  });
  if (device == m_devices.end()) {
    return {};
  }
  return refreshTrackedDevice(*device, refreshChargeLimit);
}

bool UPowerService::chargeThresholdRequestIsCurrent(const std::string& path, std::uint64_t requestId) const {
  return std::ranges::any_of(m_devices, [&path, requestId](const TrackedDevice& device) {
    return device.info.path == path && device.chargeThresholdRequestId == requestId;
  });
}

void UPowerService::emitChangedIfNeeded(bool devicesChanged, bool chargeLimitChanged, bool deviceCatalogChanged) {
  if (!devicesChanged) {
    if (chargeLimitChanged && m_changeCallback) {
      m_changeCallback(UPowerChange{.origin = ChangeOrigin::ChargeLimit});
    }
    return;
  }

  m_state = readDefaultState();
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::DeviceState, .deviceCatalogChanged = deviceCatalogChanged});
  }
}

void UPowerService::emitControlOperationChanged() {
  if (m_changeCallback) {
    m_changeCallback(UPowerChange{.origin = ChangeOrigin::ChargeLimit});
  }
}

void UPowerService::invalidateChargeThresholdRequest(std::string_view devicePath) {
  const auto device = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& candidate) {
    return candidate.info.path == devicePath;
  });
  if (device == m_devices.end()) {
    return;
  }
  device->chargeThresholdRequestId.reset();
  device->info.chargeLimit.requestPending = false;
  device->info.chargeLimit.requestedEnabled.reset();
}

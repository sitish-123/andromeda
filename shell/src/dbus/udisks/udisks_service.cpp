#include "dbus/udisks/udisks_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <sdbus-c++/sdbus-c++.h>
#include <string_view>
#include <vector>

namespace {

  constexpr Logger kLog("drives");

  const sdbus::ServiceName kBusName{"org.freedesktop.UDisks2"};
  const sdbus::ObjectPath kRootPath{"/org/freedesktop/UDisks2"};
  constexpr auto kObjectManager = "org.freedesktop.DBus.ObjectManager";
  constexpr auto kBlock = "org.freedesktop.UDisks2.Block";
  constexpr auto kFilesystem = "org.freedesktop.UDisks2.Filesystem";
  constexpr auto kDrive = "org.freedesktop.UDisks2.Drive";

  using Properties = std::map<std::string, sdbus::Variant>;
  using ObjectInterfaces = std::map<std::string, Properties>;

  template <typename T> T propertyOr(const Properties& properties, std::string_view name, T fallback) {
    const auto it = properties.find(std::string(name));
    if (it == properties.end()) {
      return fallback;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  std::string label(const Properties& block) {
    std::string name = propertyOr<std::string>(block, "IdLabel", {});
    return name.empty() ? propertyOr<std::string>(block, "IdType", "drive") : name;
  }

} // namespace

UDisksService::UDisksService(SystemBus& bus) : m_bus(bus) {
  m_root = sdbus::createProxy(m_bus.connection(), kBusName, kRootPath);
  m_root->uponSignal("InterfacesAdded")
      .onInterface(kObjectManager)
      .call([this](const sdbus::ObjectPath& path, const ObjectInterfaces& interfaces) {
        const auto block = interfaces.find(kBlock);
        const auto filesystem = interfaces.find(kFilesystem);
        if (block == interfaces.end() || filesystem == interfaces.end()) {
          return;
        }
        const Properties& props = block->second;
        const bool automount = propertyOr<bool>(props, "HintAuto", false)
            && !propertyOr<bool>(props, "HintIgnore", false)
            && !propertyOr<bool>(props, "HintSystem", true);
        const auto mountPoints =
            propertyOr<std::vector<std::vector<std::uint8_t>>>(filesystem->second, "MountPoints", {});
        if (automount && mountPoints.empty()) {
          mount(path, label(props), propertyOr<sdbus::ObjectPath>(props, "Drive", sdbus::ObjectPath{"/"}));
        }
      });
  m_root->uponSignal("InterfacesRemoved")
      .onInterface(kObjectManager)
      .call([this](const sdbus::ObjectPath& path, const std::vector<std::string>& /*interfaces*/) {
        m_driveOfBlock.erase(path);
        m_calls.erase(path);
      });
}

UDisksService::~UDisksService() = default;

void UDisksService::setMountedCallback(MountedCallback callback) { m_mountedCallback = std::move(callback); }

void UDisksService::mount(const std::string& blockPath, const std::string& name, const std::string& drivePath) {
  m_driveOfBlock[blockPath] = drivePath;
  auto& proxy = m_calls[blockPath];
  if (proxy == nullptr) {
    proxy = sdbus::createProxy(m_bus.connection(), kBusName, sdbus::ObjectPath{blockPath});
  }
  const std::weak_ptr<int> alive = m_lifetimeToken;
  proxy->callMethodAsync("Mount")
      .onInterface(kFilesystem)
      .withArguments(Properties{})
      .uponReplyInvoke([this, alive, blockPath, name](std::optional<sdbus::Error> err, std::string mountPoint) {
        if (alive.expired()) {
          return;
        }
        if (err.has_value()) {
          kLog.warn("mount {} failed: {}", blockPath, err->what());
          return;
        }
        kLog.info("mounted {} at {}", name, mountPoint);
        if (m_mountedCallback) {
          m_mountedCallback({.blockPath = blockPath, .label = name, .mountPoint = mountPoint});
        }
      });
}

void UDisksService::eject(const std::string& blockPath) {
  const auto drive = m_driveOfBlock.find(blockPath);
  const std::string drivePath = drive != m_driveOfBlock.end() ? drive->second : std::string{};
  auto& proxy = m_calls[blockPath];
  if (proxy == nullptr) {
    proxy = sdbus::createProxy(m_bus.connection(), kBusName, sdbus::ObjectPath{blockPath});
  }
  const std::weak_ptr<int> alive = m_lifetimeToken;
  proxy->callMethodAsync("Unmount")
      .onInterface(kFilesystem)
      .withArguments(Properties{})
      .uponReplyInvoke([this, alive, blockPath, drivePath](std::optional<sdbus::Error> err) {
        if (alive.expired()) {
          return;
        }
        if (err.has_value()) {
          kLog.warn("unmount {} failed: {}", blockPath, err->what());
          return;
        }
        if (!drivePath.empty() && drivePath != "/") {
          powerOff(drivePath);
        }
      });
}

void UDisksService::powerOff(const std::string& drivePath) {
  auto& proxy = m_calls[drivePath];
  if (proxy == nullptr) {
    proxy = sdbus::createProxy(m_bus.connection(), kBusName, sdbus::ObjectPath{drivePath});
  }
  const std::weak_ptr<int> alive = m_lifetimeToken;
  proxy->callMethodAsync("PowerOff")
      .onInterface(kDrive)
      .withArguments(Properties{})
      .uponReplyInvoke([this, alive, drivePath](std::optional<sdbus::Error> err) {
        if (alive.expired()) {
          return;
        }
        if (err.has_value()) {
          kLog.warn("power off {} failed: {}", drivePath, err->what());
        }
      });
}

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Automounts removable filesystems through org.freedesktop.UDisks2 as they appear, honouring the block device's
// HintAuto / HintIgnore / HintSystem, and ejects them on request. Filesystems present at startup are left alone.
class UDisksService {
public:
  struct Mounted {
    std::string blockPath;
    std::string label;
    std::string mountPoint;
  };
  using MountedCallback = std::function<void(const Mounted&)>;

  explicit UDisksService(SystemBus& bus);
  ~UDisksService();

  void setMountedCallback(MountedCallback callback);
  // Unmounts the filesystem, then powers its drive off so it is safe to unplug.
  void eject(const std::string& blockPath);

private:
  void mount(const std::string& blockPath, const std::string& label, const std::string& drivePath);
  void powerOff(const std::string& drivePath);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_root;
  // One proxy per object, reused across calls and dropped when UDisks removes the object; a proxy must not be
  // destroyed from inside its own reply callback.
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_calls;
  std::map<std::string, std::string> m_driveOfBlock;
  MountedCallback m_mountedCallback;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};

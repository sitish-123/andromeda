#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Lid, power-button, sleep-mode and hibernate settings. Reads logind and the kernel; writes go through the
// root helper andromeda-power-config (pkexec, polkit org.andromeda.power.configure).
struct PowerConfigState {
  // logind Handle* values: suspend, hibernate, suspend-then-hibernate, lock, ignore, poweroff, ...
  std::string lidBattery;
  std::string lidPower;
  std::string lidDocked;
  std::string powerKey;
  bool hasLid = false;
  // Modes the kernel offers (/sys/power/mem_sleep) and the active one.
  std::vector<std::string> sleepModes;
  std::string sleepMode;
  // Our drop-in's HibernateDelaySec, or "off".
  std::string hibernateDelay = "off";
  bool canHibernate = false;
  bool canSuspendThenHibernate = false;
  bool lockdown = false;
  std::uint64_t ramBytes = 0;
  // Swap on disk only; zram cannot hold a hibernation image.
  std::uint64_t diskSwapBytes = 0;
  // resume= is set, or a systemd initramfs on UEFI can resume through the HibernateLocation EFI variable.
  bool resumeConfigured = false;
  bool helperInstalled = false;
};

class PowerConfigService {
public:
  using ChangeCallback = std::function<void()>;
  using ResultCallback = std::function<void(bool ok)>;

  explicit PowerConfigService(SystemBus& bus);
  ~PowerConfigService();

  void setChangeCallback(ChangeCallback callback);
  void refresh();
  // key/value as andromeda-power-config accepts them. Refreshes after the helper exits.
  void set(const std::string& key, const std::string& value, ResultCallback done = {});

  [[nodiscard]] const PowerConfigState& state() const noexcept { return m_state; }

private:
  void readSystem();
  void readLogind();
  void notify();

  std::unique_ptr<sdbus::IProxy> m_manager;
  PowerConfigState m_state;
  ChangeCallback m_changeCallback;
  std::shared_ptr<int> m_aliveToken = std::make_shared<int>(0);
};

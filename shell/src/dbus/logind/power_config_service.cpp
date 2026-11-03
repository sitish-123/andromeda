#include "dbus/logind/power_config_service.h"

#include "core/log.h"
#include "core/process/process.h"
#include "dbus/system_bus.h"

#include <fstream>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <sstream>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("power-config");

  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kHelperPath = "/usr/lib/noctalia/andromeda-power-config";
  constexpr auto kSleepDropIn = "/etc/systemd/sleep.conf.d/60-andromeda.conf";

  std::string readFile(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  // SW_LID is bit 0 of an input device's switch bitmap (linux/input-event-codes.h).
  bool hasLidSwitch() {
    std::istringstream in(readFile("/proc/bus/input/devices"));
    for (std::string line; std::getline(in, line);) {
      if (!line.starts_with("B: SW=")) {
        continue;
      }
      const std::string words = line.substr(6);
      const std::string last = words.substr(words.find_last_of(' ') + 1);
      if (!last.empty() && (std::stoull(last, nullptr, 16) & 1U) != 0) {
        return true;
      }
    }
    return false;
  }

  std::uint64_t meminfoBytes(const std::string& key) {
    std::istringstream in(readFile("/proc/meminfo"));
    for (std::string name, unit; in >> name;) {
      std::uint64_t kb = 0;
      in >> kb >> unit;
      if (name == key + ":") {
        return kb * 1024;
      }
    }
    return 0;
  }

  std::uint64_t diskSwapBytes() {
    std::istringstream in(readFile("/proc/swaps"));
    std::string line;
    std::getline(in, line);
    std::uint64_t total = 0;
    for (std::string name, type; in >> name >> type;) {
      std::uint64_t kb = 0;
      in >> kb;
      std::getline(in, line);
      if (!name.starts_with("/dev/zram")) {
        total += kb * 1024;
      }
    }
    return total;
  }

  // systemd-hibernate-resume-generator(8): a systemd initramfs resumes from the HibernateLocation EFI variable.
  bool systemdInitramfs() {
    std::istringstream in(readFile("/etc/mkinitcpio.conf"));
    for (std::string line; std::getline(in, line);) {
      if (line.starts_with("HOOKS=")) {
        return line.contains(" systemd ") || line.contains("(systemd ");
      }
    }
    return ::access("/usr/bin/dracut", X_OK) == 0;
  }

  bool resumeConfigured() {
    std::string resume = readFile("/sys/power/resume");
    while (!resume.empty() && resume.back() == '\n') {
      resume.pop_back();
    }
    if (!resume.empty() && resume != "0:0") {
      return true;
    }
    return ::access("/sys/firmware/efi/efivars", F_OK) == 0 && systemdInitramfs();
  }
} // namespace

PowerConfigService::PowerConfigService(SystemBus& bus) {
  try {
    m_manager = sdbus::createProxy(bus.connection(), kLogindBusName, kLogindObjectPath);
  } catch (const sdbus::Error& e) {
    kLog.warn("logind unavailable: {}", e.what());
  }
  refresh();
}

PowerConfigService::~PowerConfigService() = default;

void PowerConfigService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void PowerConfigService::refresh() {
  readSystem();
  notify();
  readLogind();
}

void PowerConfigService::readSystem() {
  m_state.hasLid = hasLidSwitch();
  m_state.sleepModes.clear();
  m_state.sleepMode.clear();
  std::istringstream modes(readFile("/sys/power/mem_sleep"));
  for (std::string mode; modes >> mode;) {
    if (mode.starts_with('[')) {
      mode = mode.substr(1, mode.size() - 2);
      m_state.sleepMode = mode;
    }
    m_state.sleepModes.push_back(mode);
  }
  m_state.hibernateDelay = "off";
  std::istringstream dropIn(readFile(kSleepDropIn));
  for (std::string line; std::getline(dropIn, line);) {
    if (line.starts_with("HibernateDelaySec=")) {
      m_state.hibernateDelay = line.substr(18);
    }
  }
  const std::string lockdown = readFile("/sys/kernel/security/lockdown");
  m_state.lockdown = !lockdown.empty() && !lockdown.contains("[none]");
  m_state.ramBytes = meminfoBytes("MemTotal");
  m_state.diskSwapBytes = diskSwapBytes();
  m_state.resumeConfigured = resumeConfigured();
  m_state.helperInstalled = ::access(kHelperPath, X_OK) == 0;
}

void PowerConfigService::readLogind() {
  if (m_manager == nullptr) {
    return;
  }
  const std::weak_ptr<int> alive = m_aliveToken;
  const auto readString = [this, alive](const char* property, std::string PowerConfigState::* field) {
    m_manager->getPropertyAsync(property).onInterface(kManagerInterface).uponReplyInvoke(
        [this, alive, property, field](std::optional<sdbus::Error> e, const sdbus::Variant& value) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            kLog.warn("logind {} read failed: {}", property, e->what());
            return;
          }
          m_state.*field = value.get<std::string>();
          notify();
        }
    );
  };
  readString("HandleLidSwitch", &PowerConfigState::lidBattery);
  readString("HandleLidSwitchExternalPower", &PowerConfigState::lidPower);
  readString("HandleLidSwitchDocked", &PowerConfigState::lidDocked);
  readString("HandlePowerKey", &PowerConfigState::powerKey);

  const auto readCan = [this, alive](const char* method, bool PowerConfigState::* field) {
    m_manager->callMethodAsync(method).onInterface(kManagerInterface).uponReplyInvoke(
        [this, alive, method, field](std::optional<sdbus::Error> e, const std::string& answer) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            kLog.warn("logind {} failed: {}", method, e->what());
            return;
          }
          m_state.*field = answer == "yes";
          notify();
        }
    );
  };
  readCan("CanHibernate", &PowerConfigState::canHibernate);
  readCan("CanSuspendThenHibernate", &PowerConfigState::canSuspendThenHibernate);
}

void PowerConfigService::set(const std::string& key, const std::string& value, ResultCallback done) {
  const std::weak_ptr<int> alive = m_aliveToken;
  // pkexec, not run0: the polkit action's exec.path annotation applies to pkexec only.
  const bool started = process::runAsync(
      std::vector<std::string>{"pkexec", kHelperPath, key, value},
      process::RunCallbacks{
          .onExit =
              [this, alive, key, value, done = std::move(done)](process::RunResult result) {
                if (alive.expired()) {
                  return;
                }
                if (!result) {
                  kLog.warn("power config {}={} failed (exit {}): {}", key, value, result.exitCode, result.err);
                }
                refresh();
                if (done) {
                  done(static_cast<bool>(result));
                }
              },
      }
  );
  if (!started) {
    kLog.warn("power config: pkexec did not start");
    if (done) {
      done(false);
    }
  }
}

void PowerConfigService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

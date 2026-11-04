#include "dbus/logind/power_config_service.h"

#include "core/log.h"
#include "core/process/process.h"
#include "dbus/system_bus.h"
#include "util/file_utils.h"

#include <ctime>
#include <filesystem>
#include <format>
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
  constexpr auto kSystemHelperPath = "/usr/lib/noctalia/andromeda-power-system";
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

  std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) {
      s.pop_back();
    }
    return s;
  }

  // A resume from hibernation keeps the boot ID; a cold boot gets a new one.
  std::string bootId() { return trimmed(readFile("/proc/sys/kernel/random/boot_id")); }

  std::string trialPath() { return FileUtils::stateDir() + "/hibernate-trial"; }

  // What a passed trial depends on: kernel, command line and disk swap. Any change asks for a new trial.
  std::string resumeFingerprint() {
    std::string swaps;
    std::istringstream in(readFile("/proc/swaps"));
    std::string line;
    std::getline(in, line);
    for (std::string name, type, size; in >> name >> type >> size;) {
      std::getline(in, line);
      if (!name.starts_with("/dev/zram")) {
        swaps += name + ":" + size + ";";
      }
    }
    const std::string all =
        readFile("/proc/sys/kernel/osrelease") + readFile("/proc/cmdline") + swaps;
    return std::format("{:016x}", std::hash<std::string>{}(all));
  }

  bool isSystemKey(const std::string& key) {
    return key == "swapfile" || key == "resume-apply" || key == "resume-rollback" || key == "initramfs-systemd";
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
  // "pending <boot id> <epoch>" from another boot: the trial hibernate never resumed. From this boot (the shell
  // restarted around it): the kernel log decides.
  std::istringstream saved(trimmed(readFile(trialPath().c_str())));
  std::string word, boot;
  if (saved >> word >> boot && word == "pending") {
    // Older files have no start time; 0 searches the whole boot.
    if (!(saved >> m_trialSince)) {
      m_trialSince = 0;
    }
    if (boot != bootId()) {
      kLog.warn("hibernate trial failed: the machine booted fresh instead of resuming");
      writeTrial("failed");
    } else {
      finishTrial();
    }
  }
  if (m_manager != nullptr) {
    m_manager->uponSignal("PrepareForSleep").onInterface(kManagerInterface).call([this](bool sleeping) {
      if (!sleeping && m_state.trial == "pending") {
        finishTrial();
      }
    });
  }
  refresh();
}

PowerConfigService::~PowerConfigService() = default;

void PowerConfigService::writeTrial(const std::string& value) {
  std::error_code ec;
  std::filesystem::create_directories(FileUtils::stateDir(), ec);
  std::string line = value;
  if (value == "pending") {
    line += " " + bootId() + " " + std::to_string(m_trialSince);
  } else if (value == "ok") {
    line += " " + resumeFingerprint();
  }
  std::ofstream(trialPath()) << line << '\n';
  m_state.trial = value;
}

bool PowerConfigService::hibernateVerified() const {
  return m_state.canHibernate && m_state.trial == "ok" && m_trialFingerprint == resumeFingerprint();
}

void PowerConfigService::startTrial() {
  if (m_manager == nullptr || !m_state.canHibernate) {
    return;
  }
  // pm_debug_messages makes the restored kernel log "Hibernation image restored successfully"; trial-check resets it.
  set("trial-debug", "on", [this](bool ok) {
    if (!ok) {
      return;
    }
    m_trialSince = std::time(nullptr);
    writeTrial("pending");
    notify();
    const std::weak_ptr<int> alive = m_aliveToken;
    m_manager->callMethodAsync("Hibernate").onInterface(kManagerInterface).withArguments(false).uponReplyInvoke(
        [this, alive](std::optional<sdbus::Error> e) {
          if (alive.expired() || !e.has_value()) {
            return;
          }
          kLog.warn("hibernate trial refused: {}", e->what());
          writeTrial("");
          notify();
        }
    );
  });
}

// Passed only when the restored kernel logged that message after the trial began. A lid suspend or an aborted
// hibernate also ends in PrepareForSleep(false), but without that message. The helper reads the kernel log as root
// and retries while journald catches up; exit 1 = no restore, any other failure (a cancelled prompt) stays pending.
void PowerConfigService::finishTrial() {
  set("trial-check", std::to_string(m_trialSince), [this](bool ok) {
    if (ok || m_state.lastExit == 1) {
      kLog.info("hibernate trial: {}", ok ? "resumed from the image" : "no restore in the kernel log");
      writeTrial(ok ? "ok" : "failed");
      notify();
    }
  });
}

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
  const bool uefi = ::access("/sys/firmware/efi/efivars", F_OK) == 0;
  m_state.canSwitchInitramfs = uefi && ::access("/usr/bin/mkinitcpio", X_OK) == 0 && !systemdInitramfs();
  // The helper leaves this after a new initramfs; /run is empty again after a reboot.
  m_state.restartNeeded = ::access("/run/andromeda-power/restart", F_OK) == 0;
  m_state.helperInstalled = ::access(kHelperPath, X_OK) == 0 && ::access(kSystemHelperPath, X_OK) == 0;
  std::istringstream saved(trimmed(readFile(trialPath().c_str())));
  m_state.trial.clear();
  m_trialFingerprint.clear();
  saved >> m_state.trial >> m_trialFingerprint;
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
  const auto shared = std::make_shared<ResultCallback>(std::move(done));
  // pkexec, not run0: the polkit action's exec.path annotation applies to pkexec only.
  const bool started = process::runAsync(
      std::vector<std::string>{"pkexec", isSystemKey(key) ? kSystemHelperPath : kHelperPath, key, value},
      process::RunCallbacks{
          .onExit =
              [this, alive, key, value, shared](const process::RunResult& result) {
                if (alive.expired()) {
                  return;
                }
                m_state.lastExit = result.exitCode;
                m_state.lastError.clear();
                m_state.lastGuide.clear();
                std::istringstream err(result ? std::string{} : result.err);
                for (std::string line; std::getline(err, line);) {
                  if (line.starts_with("guide: https://")) {
                    m_state.lastGuide = trimmed(line.substr(7));
                  } else if (m_state.lastError.empty()) {
                    m_state.lastError = trimmed(line);
                  }
                }
                if (!result) {
                  kLog.warn("power config {}={} failed (exit {}): {}", key, value, result.exitCode, result.err);
                }
                // Swap or boot entries changed: a passed trial no longer proves resume.
                if (result && isSystemKey(key)) {
                  writeTrial("");
                }
                refresh();
                if (*shared) {
                  (*shared)(static_cast<bool>(result));
                }
              },
      }
  );
  if (!started) {
    kLog.warn("power config: pkexec did not start");
    if (*shared) {
      (*shared)(false);
    }
  }
}

void PowerConfigService::reboot() {
  if (m_manager == nullptr) {
    return;
  }
  // interactive = true: polkit may ask when other sessions are open.
  m_manager->callMethodAsync("Reboot").onInterface(kManagerInterface).withArguments(true).uponReplyInvoke(
      [](std::optional<sdbus::Error> e) {
        if (e.has_value()) {
          kLog.warn("reboot refused: {}", e->what());
        }
      }
  );
}

void PowerConfigService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

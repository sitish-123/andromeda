#include "shell/session/session_guard.h"

#include "core/log.h"
#include "dbus/andromeda/software_update_service.h"
#include "dbus/system_bus.h"
#include "ipc/ipc_service.h"

#include <algorithm>
#include <chrono>
#include <unistd.h>

namespace {

  constexpr Logger kLog("session-guard");
  constexpr auto kLogin = "org.andromeda.System1.Login";
  // Below this uid are system accounts (the greeter among them), never "other users".
  constexpr std::uint32_t kFirstUserUid = 1000;

  using Inhibitor = sdbus::Struct<std::string, std::string, std::string, std::string, std::uint32_t, std::uint32_t>;
  using Session = sdbus::Struct<std::string, std::uint32_t, std::string, std::string, sdbus::ObjectPath>;

} // namespace

void SessionGuard::initialize(SystemBus* bus, IpcService* ipc) {
  m_ipc = ipc;
  if (bus == nullptr) {
    return;
  }
  try {
    m_logind = sdbus::createProxy(
        bus->connection(), sdbus::ServiceName{"org.andromeda.System1"},
        sdbus::ObjectPath{"/org/andromeda/System1/Login"}
    );
  } catch (const sdbus::Error& e) {
    kLog.warn("logind unavailable: {}", e.getMessage());
    return;
  }
  std::weak_ptr<int> alive = m_alive;
  m_logind->callMethodAsync("CanPower")
      .onInterface(kLogin)
      .withArguments(std::string("reboot-to-firmware-setup"))
      .uponReplyInvoke(
      [this, alive](std::optional<sdbus::Error> e, const std::string& answer) {
        if (!alive.expired() && !e.has_value()) {
          m_firmwareSetup = answer == "yes" || answer == "challenge";
        }
      }
  );
}

void SessionGuard::watch(bool on) {
  m_watchers = std::max(0, m_watchers + (on ? 1 : -1));
  if (on) {
    refresh();
  }
  syncTimer();
}

void SessionGuard::syncTimer() {
  const bool needed = m_watchers > 0 || !m_queued.empty();
  if (needed && !m_poll.active()) {
    m_poll.startRepeating(std::chrono::seconds(2), [this] { refresh(); });
  } else if (!needed) {
    m_poll.stop();
  }
}

void SessionGuard::refresh() {
  if (m_logind == nullptr) {
    return;
  }
  std::weak_ptr<int> alive = m_alive;
  m_logind->callMethodAsync("ListInhibitors").onInterface(kLogin).uponReplyInvoke(
      [this, alive](std::optional<sdbus::Error> e, const std::vector<Inhibitor>& list) {
        if (alive.expired() || e.has_value()) {
          return;
        }
        std::vector<Blocker> blockers;
        for (const Inhibitor& entry : list) {
          const std::string& what = entry.get<0>();
          const std::string& mode = entry.get<3>();
          // Delay inhibitors only hold the action a moment and never show (DESIGN.md).
          if (mode != "block" || what.find("shutdown") == std::string::npos
              || entry.get<5>() == static_cast<std::uint32_t>(getpid())) {
            continue;
          }
          blockers.push_back({.who = entry.get<1>(), .why = entry.get<2>()});
        }
        if (blockers != m_blockers) {
          m_blockers = std::move(blockers);
          notify();
        }
        if (!m_queued.empty() && m_blockers.empty()) {
          runQueued();
        }
      }
  );
  m_logind->callMethodAsync("ListSessions").onInterface(kLogin).uponReplyInvoke(
      [this, alive](std::optional<sdbus::Error> e, const std::vector<Session>& list) {
        if (alive.expired() || e.has_value()) {
          return;
        }
        std::vector<std::string> users;
        for (const Session& session : list) {
          const std::uint32_t uid = session.get<1>();
          if (uid < kFirstUserUid || uid == getuid() || std::ranges::contains(users, session.get<2>())) {
            continue;
          }
          users.push_back(session.get<2>());
        }
        if (users != m_otherUsers) {
          m_otherUsers = std::move(users);
          notify();
        }
      }
  );
}

void SessionGuard::queue(const std::string& action) {
  m_queued = action;
  m_updates = nullptr;
  notify();
  refresh();
  syncTimer();
}

void SessionGuard::updateAndRestart(SoftwareUpdateService& updates) {
  m_updates = &updates;
  m_queued = "reboot";
  updates.upgrade();
  notify();
}

void SessionGuard::cancel() {
  if (m_queued.empty()) {
    return;
  }
  m_queued.clear();
  m_updates = nullptr;
  syncTimer();
  notify();
}

void SessionGuard::onUpdateChanged() {
  if (m_updates == nullptr) {
    return;
  }
  switch (m_updates->runState()) {
  case SoftwareUpdateService::RunState::Done:
    m_updates = nullptr;
    // The restart still waits behind a block inhibitor, if one appeared meanwhile.
    refresh();
    syncTimer();
    break;
  case SoftwareUpdateService::RunState::Failed:
  case SoftwareUpdateService::RunState::NeedsChoice:
    // Settings › Software Update shows the failure or the question; the restart does not go on by itself.
    cancel();
    break;
  default:
    break;
  }
  notify();
}

void SessionGuard::runQueued() {
  if (m_updates != nullptr) {
    return;
  }
  const std::string action = std::move(m_queued);
  m_queued.clear();
  syncTimer();
  notify();
  if (m_ipc != nullptr) {
    if (const std::string reply = m_ipc->execute("power-overlay end " + action); reply.starts_with("error")) {
      kLog.warn("queued {} dropped: {}", action, reply);
    }
  }
}

void SessionGuard::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

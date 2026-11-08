#include "shell/desktop/desktop_file_ops.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <cstdint>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("desktop-icons");
  const sdbus::ServiceName kName{"org.andromeda.FileOps1"};
  const sdbus::ObjectPath kPath{"/org/andromeda/FileOps1"};
  constexpr auto kInterface = "org.andromeda.FileOps1";
  constexpr auto kJobInterface = "org.andromeda.FileOps1.Job";
  constexpr auto kProperties = "org.freedesktop.DBus.Properties";
  constexpr std::size_t kUndoDepth = 50;

  using Bytes = std::vector<std::uint8_t>;

  Bytes bytes(const std::filesystem::path& path) {
    const std::string& s = path.native();
    return {s.begin(), s.end()};
  }

  bool finished(const std::string& state) { return state == "done" || state == "failed" || state == "cancelled"; }
} // namespace

DesktopFileOps::DesktopFileOps(SessionBus* bus) : m_bus(bus) {
  if (m_bus != nullptr) {
    m_manager = sdbus::createProxy(m_bus->connection(), kName, kPath);
  }
}

DesktopFileOps::~DesktopFileOps() = default;

bool DesktopFileOps::transfer(bool move, const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& destination) {
  if (m_manager == nullptr || sources.empty()) {
    return false;
  }
  std::vector<Bytes> list;
  for (const auto& source : sources) {
    list.push_back(bytes(source));
  }
  try {
    m_manager->callMethodAsync(move ? "Move" : "Copy")
        .onInterface(kInterface)
        .withArguments(list, bytes(destination))
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), move, count = sources.size(),
                          destination](std::optional<sdbus::Error> error, sdbus::ObjectPath job) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("FileOps1 {} failed: {}", move ? "Move" : "Copy", error->getMessage());
            return;
          }
          kLog.debug("desktop {} {} items into {} as {}", move ? "moves" : "copies", count, destination.string(),
                     std::string(job));
          track(job, true);
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("FileOps1 is not reachable: {}", e.getMessage());
    return false;
  }
  return true;
}

void DesktopFileOps::track(const std::string& job, bool remember) {
  if (!remember) {
    m_noUndo.insert(job);
  }
  auto proxy = sdbus::createProxy(m_bus->connection(), kName, sdbus::ObjectPath{job});
  const auto onState = [this, job](const std::string& state, std::optional<bool> undoable) {
    if (state == "conflict") {
      // No conflict sheet on the desktop: keep both, as Finder names a dropped clash "name 2".
      if (const auto it = m_jobs.find(job); it != m_jobs.end()) {
        it->second->callMethodAsync("ResolveConflict")
            .onInterface(kJobInterface)
            .withArguments(std::string("keep_both"), true)
            .uponReplyInvoke([](std::optional<sdbus::Error>) {});
      }
    } else if (finished(state)) {
      finish(job, undoable.value_or(false));
    }
  };
  proxy->uponSignal("PropertiesChanged")
      .onInterface(kProperties)
      .call([onState](const std::string& iface, const std::map<std::string, sdbus::Variant>& changed,
                      const std::vector<std::string>&) {
        if (iface != kJobInterface) {
          return;
        }
        if (const auto it = changed.find("State"); it != changed.end()) {
          std::optional<bool> undoable;
          if (const auto u = changed.find("Undoable"); u != changed.end()) {
            undoable = u->second.get<bool>();
          }
          onState(it->second.get<std::string>(), undoable);
        }
      });
  auto* raw = proxy.get();
  m_jobs[job] = std::move(proxy);
  // The job may have asked or finished before the signal was wired: read its state once.
  raw->callMethodAsync("GetAll")
      .onInterface(kProperties)
      .withArguments(std::string(kJobInterface))
      .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), job, onState](
                           std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> all) {
        if (alive.expired() || !m_jobs.contains(job)) {
          return;
        }
        if (error.has_value()) {
          m_jobs.erase(job); // gone already (daemon restart): nothing to follow
          m_noUndo.erase(job);
          return;
        }
        const auto state = all.find("State");
        const auto undoable = all.find("Undoable");
        if (state != all.end()) {
          onState(state->second.get<std::string>(),
                  undoable != all.end() ? std::optional(undoable->second.get<bool>()) : std::nullopt);
        }
      });
}

void DesktopFileOps::finish(const std::string& job, bool undoable) {
  // The signal and the first GetAll can both report the end.
  if (!m_jobs.contains(job) || !m_finished.insert(job).second) {
    return;
  }
  // The daemon signals State before Undoable: read Undoable once more when it was not in the same signal.
  const auto settle = [this, job](bool canUndo) {
    kLog.debug("desktop job {} finished, undoable {}", job, canUndo);
    if (canUndo && !m_noUndo.contains(job)) {
      m_undo.push_back(job);
      if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
      }
    }
    // Not from inside this proxy's own callback.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), job] {
      if (!alive.expired()) {
        m_jobs.erase(job);
        m_finished.erase(job);
        m_noUndo.erase(job);
      }
    });
  };
  if (undoable) {
    settle(true);
    return;
  }
  m_jobs[job]
      ->getPropertyAsync("Undoable")
      .onInterface(kJobInterface)
      .uponReplyInvoke([alive = std::weak_ptr<bool>(m_alive), settle](std::optional<sdbus::Error> error,
                                                                       sdbus::Variant value) {
        if (!alive.expired()) {
          settle(!error.has_value() && value.get<bool>());
        }
      });
}

bool DesktopFileOps::undo() {
  if (m_manager == nullptr || m_undo.empty()) {
    return false;
  }
  const std::string job = m_undo.back();
  m_undo.pop_back();
  try {
    m_manager->callMethodAsync("Undo")
        .onInterface(kInterface)
        .withArguments(sdbus::ObjectPath{job})
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), job](std::optional<sdbus::Error> error,
                                                                             sdbus::ObjectPath undoJob) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            // The daemon quits after 10 min idle and forgets its jobs, and Files may have undone this one: try the
            // next older entry.
            kLog.debug("undo of {} not possible: {}", job, error->getMessage());
            undo();
            return;
          }
          kLog.debug("desktop undo {} as {}", job, std::string(undoJob));
          // Followed so a clash during the undo keeps both instead of waiting for an answer; not undoable again.
          track(std::string(undoJob), false);
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("undo of {} failed: {}", job, e.getMessage());
    return false;
  }
  return true;
}

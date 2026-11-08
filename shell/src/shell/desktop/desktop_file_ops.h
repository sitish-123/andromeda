#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// Copies and moves of the desktop as org.andromeda.FileOps1 jobs, so they have undo like Files (FILES-UI-SPEC §20).
// The shell has no conflict sheet: a name clash keeps both. The daemon is D-Bus activated.
class DesktopFileOps {
public:
  explicit DesktopFileOps(SessionBus* bus);
  ~DesktopFileOps();
  DesktopFileOps(const DesktopFileOps&) = delete;
  DesktopFileOps& operator=(const DesktopFileOps&) = delete;

  // One job for the batch; false when the bus is missing or the call cannot be sent.
  bool transfer(bool move, const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destination);
  // Undoes the newest finished job that can be undone; false when there is none.
  bool undo();

private:
  void track(const std::string& job, bool remember); // remember: push it on the undo stack when it ends
  void finish(const std::string& job, bool undoable);

  SessionBus* m_bus = nullptr;
  std::unique_ptr<sdbus::IProxy> m_manager;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_jobs; // running jobs
  std::set<std::string> m_finished;
  std::set<std::string> m_noUndo; // undo jobs: followed, never pushed
  std::vector<std::string> m_undo;                              // newest last
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>
#include <vector>

class SessionBus;

// Async client of org.andromeda.FileOps1 (system/data/org.andromeda.FileOps1.xml): the per-user file operation
// daemon. Every call returns at once; replies and job updates arrive on the app loop through the session bus.
class FileOpsClient {
public:
  struct Job {
    std::string path; // D-Bus object path
    std::string kind; // copy, move, trash, delete, restore, undo
    std::string state; // running, conflict, done, failed, cancelled
    std::uint64_t totalFiles = 0;
    std::uint64_t doneFiles = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t doneBytes = 0;
    std::filesystem::path current;
    std::string error;
    std::optional<std::pair<std::filesystem::path, std::filesystem::path>> conflict; // source, existing target
    bool undoable = false;

    [[nodiscard]] bool finished() const noexcept { return state == "done" || state == "failed" || state == "cancelled"; }
  };
  enum class Resolution : std::uint8_t { Replace, Skip, KeepBoth };
  // jobPath empty on error.
  using Started = std::function<void(const std::string& jobPath, const std::string& error)>;
  using JobChanged = std::function<void(const Job& job)>;

  explicit FileOpsClient(SessionBus& bus);
  ~FileOpsClient();

  FileOpsClient(const FileOpsClient&) = delete;
  FileOpsClient& operator=(const FileOpsClient&) = delete;

  void copy(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destDir, Started done = {});
  void move(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destDir, Started done = {});
  void trash(const std::vector<std::filesystem::path>& sources, Started done = {});
  void remove(const std::vector<std::filesystem::path>& sources, Started done = {});
  // Items are (trash directory, name in it), as listed from the trash's info/ files.
  void restore(const std::vector<std::pair<std::filesystem::path, std::string>>& items, Started done = {});
  void undo(const std::string& jobPath, Started done = {});
  void cancel(const std::string& jobPath);
  void resolveConflict(const std::string& jobPath, Resolution resolution, bool applyToAll);

  // Every job of the daemon, from any app.
  [[nodiscard]] const std::map<std::string, Job>& jobs() const noexcept { return m_jobs; }
  [[nodiscard]] const Job* job(const std::string& path) const;
  void setJobChangedCallback(JobChanged callback) { m_onJobChanged = std::move(callback); }
  // A job that left the daemon's list (it keeps finished jobs for undo until it drops them).
  void setJobRemovedCallback(std::function<void(const std::string& path)> callback) {
    m_onJobRemoved = std::move(callback);
  }

private:
  void start(const char* method, std::function<void(sdbus::AsyncMethodInvoker&)> args, Started done);
  void syncJobs(const std::vector<sdbus::ObjectPath>& paths);
  void track(const std::string& path);
  void apply(Job& job, const std::map<std::string, sdbus::Variant>& props);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_manager;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_jobProxies;
  std::map<std::string, Job> m_jobs;
  JobChanged m_onJobChanged;
  std::function<void(const std::string&)> m_onJobRemoved;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

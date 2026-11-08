#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
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
    std::string state; // running, paused, conflict, done, failed, cancelled
    std::uint64_t totalFiles = 0;
    std::uint64_t doneFiles = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t doneBytes = 0;
    std::filesystem::path current;
    std::filesystem::path destination; // copy, move, link, extract; empty for other jobs
    std::string error;
    std::optional<std::pair<std::filesystem::path, std::filesystem::path>> conflict; // source, existing target
    // Conflict k of m: the question asked now (1-based, 0 before the first); m = 0 when unknown.
    std::uint64_t conflictIndex = 0;
    std::uint64_t conflictTotal = 0;
    // A folder onto a folder: what a merge does with its files (FILES-UI-SPEC §14).
    struct FolderClash {
      std::uint64_t newFiles = 0;
      std::uint64_t sameFiles = 0;      // equal size and modification time
      std::uint64_t differentFiles = 0;
      std::vector<std::pair<std::filesystem::path, std::string>> different; // up to 20: path inside, hint
    };
    std::optional<FolderClash> folderClash;
    bool undoable = false;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moves; // finished: what moved, from -> to
    std::string result; // a checksum job's hex digest

    [[nodiscard]] bool finished() const noexcept { return state == "done" || state == "failed" || state == "cancelled"; }
  };
  enum class Resolution : std::uint8_t { Replace, Skip, KeepBoth, Merge }; // Merge: a folder onto a folder
  // jobPath empty on error.
  using Started = std::function<void(const std::string& jobPath, const std::string& error)>;
  using JobChanged = std::function<void(const Job& job)>;
  struct ArchiveEntry {
    std::filesystem::path path; // relative inside the archive
    std::string kind;           // file, dir, link
    std::int64_t size = 0;
    std::uint64_t mtime = 0; // seconds since 1970
    bool isDir = false;
  };
  // error empty on success; passwordNeeded: a 7z whose names are encrypted.
  using ArchiveListed =
      std::function<void(std::vector<ArchiveEntry> entries, const std::string& error, bool passwordNeeded)>;

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
  void rename(const std::filesystem::path& path, const std::string& name, Started done = {});
  void renameMany(const std::vector<std::pair<std::filesystem::path, std::string>>& items, Started done = {});
  void newFolder(const std::filesystem::path& destDir, const std::string& name, Started done = {});
  void link(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destDir, Started done = {});
  void listArchive(const std::filesystem::path& archive, ArchiveListed done);
  // innerPaths empty = all. A job error ending in "password needed" or "wrong password" asks for the password.
  void extract(const std::filesystem::path& archive, const std::vector<std::filesystem::path>& innerPaths,
               const std::filesystem::path& destDir, const std::string& password, Started done = {});
  // format: zip, 7z, tar.gz, tar.xz, tar.zst; a password only for zip and 7z.
  // Bits `mode` where `mask` is set; folders open where they read. recursive: also everything inside folders.
  void setPermissions(const std::vector<std::filesystem::path>& targets, std::uint32_t mode, std::uint32_t mask,
                      bool recursive, Started done = {});
  // mode "set" (exactly these), "add", "remove" or "rename" (tags = {old, new}); tags in user.xdg.tags, no comma in a tag.
  void setTags(const std::vector<std::filesystem::path>& targets, const std::vector<std::string>& tags,
               const std::string& mode, Started done = {});
  // algorithm "sha256" or "md5". finished gets the hex digest, or "" and the error (also "cancelled").
  void checksum(const std::filesystem::path& path, const std::string& algorithm, Started started,
                std::function<void(const std::string& hex, const std::string& error)> finished);
  // Storage scan of one file system as a job; finished gets its JSON summary (FileOps1.xml StorageScan).
  void storageScan(const std::filesystem::path& root, bool fresh, Started started,
                   std::function<void(const std::string& json, const std::string& error)> finished);
  struct SizedPath {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    bool isDir = false;
  };
  using FolderSizes = std::function<void(std::vector<SizedPath> items, const std::string& error)>;
  // Folders (sizes from the last scan) and files in `folder`, largest first.
  void storageFolder(const std::filesystem::path& folder, FolderSizes done);
  void compress(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& archive,
                const std::string& format, const std::string& password, std::uint64_t volumeSize, Started done = {});
  void cancel(const std::string& jobPath);
  // Only a running job pauses (state "paused" at once) and only a paused one resumes.
  void pause(const std::string& jobPath);
  void resume(const std::string& jobPath);
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
  // A job whose Result the caller wants once it ends ("" and the error, or "cancelled", when it does not finish).
  void startWithResult(const char* method, std::function<void(sdbus::AsyncMethodInvoker&)> args, Started started,
                       std::function<void(const std::string& result, const std::string& error)> finished);
  void syncJobs(const std::vector<sdbus::ObjectPath>& paths);
  void track(const std::string& path);
  void apply(Job& job, const std::map<std::string, sdbus::Variant>& props, bool notifyChange);
  void refresh(const std::string& path);
  void notify(const Job& job);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_manager;
  std::set<std::string> m_settling; // finished jobs whose GetAll is in flight
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_jobProxies;
  std::map<std::string, Job> m_jobs;
  JobChanged m_onJobChanged;
  std::function<void(const std::string&)> m_onJobRemoved;
  // Job path -> who waits for its end (checksum results), called once it has settled.
  std::map<std::string, std::function<void(const Job&)>> m_finishWaiters;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

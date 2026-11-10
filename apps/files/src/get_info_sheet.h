#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class FileOpsClient;
class Label;
class Node;

// Get Info sheet (mockup files-v2-live.html §11): General (kind, size counted live on a worker, where, dates, Open
// With), Permissions in plain words (written through FileOps1.SetPermissions, with undo), and SHA-256 / MD5 on demand
// (FileOps1.Checksum, cancellable). Several items: one summary with shared values or "Mixed"; a change applies to all.
class GetInfoSheet {
public:
  GetInfoSheet(std::vector<std::filesystem::path> items, FileOpsClient* ops, std::function<void()> close,
               std::function<void()> rebuild, std::function<void()> invalidate);
  ~GetInfoSheet();
  GetInfoSheet(const GetInfoSheet&) = delete;
  GetInfoSheet& operator=(const GetInfoSheet&) = delete;

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  // Restore copies a previous version back into the file's folder; the host runs it as a job (conflict sheet).
  void setRestore(std::function<void(const std::filesystem::path& version, const std::filesystem::path& folder)> restore) {
    m_restore = std::move(restore);
  }
  void setOpen(std::function<void(const std::filesystem::path& version)> open) { m_open = std::move(open); }

  // Previous Versions (FB-INFO): the file in btrfs snapshots (snapper layout <dir>/.snapshots/<n>/snapshot/<rel>,
  // looked up in each ancestor on the same device), newest first, only versions that differ from the file.
  struct Version {
    std::filesystem::path path;
    std::int64_t modifiedNs = 0;
    std::uint64_t size = 0;
  };
  [[nodiscard]] static std::vector<Version> previousVersions(const std::filesystem::path& file);

private:
  struct Item {
    std::filesystem::path path;
    std::uint32_t mode = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    bool isDir = false;
    bool isRegular = false;
    std::uint64_t size = 0;
    std::int64_t createdNs = 0; // 0 when the file system has no birth time
    std::int64_t modifiedNs = 0;
    std::int64_t openedNs = 0;
  };
  struct Sum {
    enum class State : std::uint8_t { Idle, Running, Done, Failed };
    State state = State::Idle;
    std::string job;
    std::string hex;
    std::string error;
  };

  void load();
  void startCount();
  void updateSize();
  [[nodiscard]] std::string sizeText() const;
  void setAccess(int cls, int access);
  void setRun(bool allow);
  void runChecksum(Sum& sum, const char* algorithm);
  [[nodiscard]] std::unique_ptr<Node> buildGeneral(float s);
  [[nodiscard]] std::unique_ptr<Node> buildPermissions(float s);
  [[nodiscard]] std::unique_ptr<Node> buildChecksums(float s);
  [[nodiscard]] std::unique_ptr<Node> buildVersions(float s);
  void later(std::function<void()> fn);
  // A mode change the daemon could not make (items of another user): the root helper through pkexec.
  struct AdminChange {
    std::vector<std::filesystem::path> paths;
    std::uint32_t mode = 0;
    std::uint32_t mask = 0;
    bool recursive = false;
  };
  void failed(const std::string& error, AdminChange change);
  void changeAsAdmin();

  std::vector<Item> m_items;
  FileOpsClient* m_ops;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  std::function<void()> m_invalidate;
  bool m_recursive = false;
  std::string m_error;
  std::optional<AdminChange> m_admin; // set: the error offers "Change as Administrator…"
  std::function<void(const std::filesystem::path&, const std::filesystem::path&)> m_restore;
  std::function<void(const std::filesystem::path&)> m_open;
  std::vector<Version> m_versions;
  Sum m_sha;
  Sum m_md5;
  std::string m_compare;

  // Folder walk, updated from the worker.
  // The folder walk's tally. The walk runs detached (a hung mount must not block closing the sheet) and owns a share.
  struct Count {
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> items{0};
    std::atomic<bool> done{false};
    std::atomic<bool> posted{false};
    std::atomic<bool> stop{false};
  };
  std::shared_ptr<Count> m_count = std::make_shared<Count>();
  Label* m_size = nullptr;
  Label* m_compareResult = nullptr;
  void updateCompare();
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

#pragma once

#include "core/files/directory_watcher.h"
#include "core/files/file_info.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class FileSortKey : std::uint8_t { Name, Size, Modified, Kind };

struct FileSort {
  FileSortKey key = FileSortKey::Name;
  bool descending = false;
  bool foldersFirst = true;
};

// The entries of one directory, listed off the main thread and kept current by inotify. One model per directory
// per process (FileModel::open), shared by every window and the picker; views keep their own sort and filter
// through FileModelView.
class FileModel : public std::enable_shared_from_this<FileModel> {
public:
  enum class Status : std::uint8_t { Loading, Ready, Missing, Denied, Error };

  struct Change {
    enum class Type : std::uint8_t { Reset, Inserted, Removed, Updated, Renamed };
    Type type = Type::Reset;
    std::string name;    // Inserted, Removed, Updated, Renamed (old name)
    std::string newName; // Renamed
  };
  using Listener = std::function<void(const Change& change)>;

  // The shared model for dir (lexically normalised). The watcher must outlive every model.
  [[nodiscard]] static std::shared_ptr<FileModel> open(const std::filesystem::path& dir, DirectoryWatcher& watcher);
  ~FileModel();

  FileModel(const FileModel&) = delete;
  FileModel& operator=(const FileModel&) = delete;

  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_dir; }
  [[nodiscard]] Status status() const noexcept { return m_status; }
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }
  // Unordered; FileModelView sorts. An update replaces the pointer, so a view's old row stays valid until it rebuilds.
  [[nodiscard]] const std::vector<std::shared_ptr<const FileInfo>>& entries() const noexcept { return m_entries; }
  [[nodiscard]] const FileInfo* find(std::string_view name) const;

  // Returns a token; the listener stays while the token lives.
  [[nodiscard]] std::shared_ptr<void> subscribe(Listener listener);
  // Lists the directory again (after a watch overflow, or on F5).
  void reload();

private:
  FileModel(std::filesystem::path dir, DirectoryWatcher& watcher);
  void startWatch();
  void onWatch(DirectoryWatcher::Event event, const std::string& name, const std::string& newName);
  void upsert(const std::string& name, bool inserted);
  void erase(const std::string& name);
  void notify(const Change& change);

  std::filesystem::path m_dir;
  DirectoryWatcher& m_watcher;
  DirectoryWatcher::WatchId m_watch = -1;
  Status m_status = Status::Loading;
  std::string m_error;
  std::vector<std::shared_ptr<const FileInfo>> m_entries;
  std::vector<std::weak_ptr<Listener>> m_listeners;
  std::uint64_t m_generation = 0; // a listing result for an older reload() is dropped
  struct QueuedEvent {
    DirectoryWatcher::Event event;
    std::string name;
    std::string newName;
  };
  bool m_listing = false;
  std::vector<QueuedEvent> m_queued; // watch events while a listing runs, replayed on its result
};

// A sorted, filtered row list over a FileModel, for one view.
class FileModelView {
public:
  explicit FileModelView(std::shared_ptr<FileModel> model);

  void setSort(FileSort sort);
  void setShowHidden(bool show);
  // Case-insensitive substring on the display name; empty shows all.
  void setFilter(std::string text);
  // Extra row test (picker file-type filters); null shows all.
  void setPredicate(std::function<bool(const FileInfo&)> predicate);
  // Called after the rows changed (any model change or setting).
  void setChangedCallback(std::function<void()> callback) { m_onChanged = std::move(callback); }

  [[nodiscard]] const std::vector<std::shared_ptr<const FileInfo>>& rows() const noexcept { return m_rows; }
  [[nodiscard]] FileModel& model() noexcept { return *m_model; }
  [[nodiscard]] const FileSort& sort() const noexcept { return m_sort; }

  // True when a should come before b under sort (exposed for tests and the column view).
  [[nodiscard]] static bool before(const FileInfo& a, const FileInfo& b, const FileSort& sort);

private:
  void rebuild();

  std::shared_ptr<FileModel> m_model;
  std::shared_ptr<void> m_subscription;
  FileSort m_sort;
  bool m_showHidden = false;
  std::string m_filter;
  std::function<bool(const FileInfo&)> m_predicate;
  std::vector<std::shared_ptr<const FileInfo>> m_rows;
  std::function<void()> m_onChanged;
};

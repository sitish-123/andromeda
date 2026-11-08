#include "core/files/file_model.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <fcntl.h>
#include <glib.h>
#include <map>
#include <mutex>
#include <numeric>
#include <sys/syscall.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace {
  constexpr Logger kLog("file-model");
  // Folders up to this size get their stat before the first result: a few ms, and no sizes that pop in.
  constexpr std::size_t kInlineStatLimit = 1000;
  constexpr std::size_t kStatBatch = 2048;
  constexpr std::size_t kMaxStatWorkers = 4;
  constexpr std::size_t kDirentBuffer = 256 * 1024;

  bool nameBefore(const FileInfo& a, const FileInfo& b) {
    const int cmp = a.collateKey.compare(b.collateKey);
    return cmp != 0 ? cmp < 0 : a.name < b.name;
  }

  std::map<std::filesystem::path, std::weak_ptr<FileModel>>& registry() {
    static std::map<std::filesystem::path, std::weak_ptr<FileModel>> models;
    return models;
  }

  std::string folded(std::string_view text) {
    gchar* f = g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
    std::string out = f != nullptr ? f : "";
    g_free(f);
    return out;
  }
} // namespace

// Phase 2 of a listing, shared by its worker threads and the model.
struct FileModel::StatJob {
  explicit StatJob(int fd, std::vector<std::shared_ptr<const FileInfo>> entries)
      : dirFd(fd), base(std::move(entries)), claimed(std::make_unique<std::atomic<bool>[]>(base.size())) {}
  ~StatJob() { ::close(dirFd); }
  StatJob(const StatJob&) = delete;
  StatJob& operator=(const StatJob&) = delete;

  const int dirFd;
  const std::vector<std::shared_ptr<const FileInfo>> base; // name order, as first published
  std::unique_ptr<std::atomic<bool>[]> claimed;
  std::atomic<std::size_t> next{0};
  std::atomic<bool> cancelled{false};
  std::weak_ptr<FileModel> model;

  std::mutex mutex; // guards the members below
  std::deque<std::size_t> priority;
  std::vector<std::pair<std::size_t, std::shared_ptr<const FileInfo>>> batch;
  std::size_t workersLeft = 0;
  bool finished = false;
  bool drainPosted = false;

  // With mutex held.
  void postDrain(const std::shared_ptr<StatJob>& self) {
    if (drainPosted) {
      return;
    }
    drainPosted = true;
    DeferredCall::callLater([self] {
      if (auto owner = self->model.lock()) {
        owner->applyStats(*self);
      }
    });
  }

  static void run(const std::shared_ptr<StatJob>& job) {
    while (!job->cancelled.load(std::memory_order_relaxed)) {
      std::size_t index = 0;
      bool urgent = false;
      {
        std::scoped_lock lock(job->mutex);
        if (!job->priority.empty()) {
          index = job->priority.front();
          job->priority.pop_front();
          urgent = true;
        }
      }
      if (!urgent) {
        index = job->next.fetch_add(1, std::memory_order_relaxed);
        if (index >= job->base.size()) {
          break;
        }
      }
      if (job->claimed[index].exchange(true, std::memory_order_relaxed)) {
        continue;
      }
      FileInfo info = *job->base[index];
      if (!fillStat(job->dirFd, info)) {
        continue; // gone: inotify reports the delete
      }
      std::scoped_lock lock(job->mutex);
      job->batch.emplace_back(index, std::make_shared<const FileInfo>(std::move(info)));
      if (urgent || job->batch.size() >= kStatBatch) {
        job->postDrain(job);
      }
    }
    std::scoped_lock lock(job->mutex);
    if (--job->workersLeft == 0) {
      job->finished = true;
      job->postDrain(job);
    }
  }
};

namespace {
  struct Listing {
    FileModel::Status status = FileModel::Status::Ready;
    std::string error;
    std::vector<std::shared_ptr<const FileInfo>> entries;
    int statFd = -1; // open when some entries still need their stat
  };

  // Phase 1, on a worker thread (a slow or hung mount must not stall the UI): names and d_type from getdents64,
  // sorted by collate key.
  Listing listNames(const std::filesystem::path& dir) {
    Listing out;
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      out.status = errno == ENOENT || errno == ENOTDIR ? FileModel::Status::Missing
          : errno == EACCES || errno == EPERM          ? FileModel::Status::Denied
                                                       : FileModel::Status::Error;
      out.error = std::strerror(errno);
      return out;
    }
    std::vector<FileInfo> infos;
    std::vector<char> buffer(kDirentBuffer);
    while (true) {
      const long n = ::syscall(SYS_getdents64, fd, buffer.data(), buffer.size());
      if (n < 0) {
        out.status = FileModel::Status::Error;
        out.error = std::strerror(errno);
        ::close(fd);
        return out;
      }
      if (n == 0) {
        break;
      }
      // struct linux_dirent64: u64 ino, s64 off, u16 reclen, u8 type, then the NUL-terminated name.
      for (long at = 0; at < n;) {
        std::uint16_t reclen = 0;
        std::memcpy(&reclen, buffer.data() + at + 16, sizeof(reclen));
        const auto type = static_cast<unsigned char>(buffer[static_cast<std::size_t>(at) + 18]);
        const std::string_view name(buffer.data() + at + 19);
        at += reclen;
        if (name == "." || name == "..") {
          continue;
        }
        if (auto info = fileFromDirent(fd, name, type)) {
          infos.push_back(std::move(*info));
        }
      }
    }
    bool pending = false;
    for (FileInfo& info : infos) {
      if (info.pendingStat && infos.size() <= kInlineStatLimit) {
        (void)fillStat(fd, info);
      }
      pending = pending || info.pendingStat;
    }
    std::vector<std::uint32_t> order(infos.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::sort(order, [&](std::uint32_t x, std::uint32_t y) { return nameBefore(infos[x], infos[y]); });
    out.entries.reserve(infos.size());
    for (const std::uint32_t i : order) {
      out.entries.push_back(std::make_shared<const FileInfo>(std::move(infos[i])));
    }
    if (pending) {
      out.statFd = fd;
    } else {
      ::close(fd);
    }
    return out;
  }
} // namespace

std::shared_ptr<FileModel> FileModel::open(const std::filesystem::path& dir, DirectoryWatcher& watcher) {
  std::filesystem::path key = dir.lexically_normal();
  if (key.has_filename() == false && key != key.root_path()) {
    key = key.parent_path();
  }
  auto& models = registry();
  if (auto it = models.find(key); it != models.end()) {
    if (auto model = it->second.lock()) {
      return model;
    }
  }
  std::shared_ptr<FileModel> model(new FileModel(key, watcher));
  models[key] = model;
  model->startWatch();
  model->reload();
  return model;
}

FileModel::FileModel(std::filesystem::path dir, DirectoryWatcher& watcher) : m_dir(std::move(dir)), m_watcher(watcher) {}

FileModel::~FileModel() {
  if (m_statJob != nullptr) {
    m_statJob->cancelled = true;
  }
  m_watcher.unwatch(m_watch);
  auto& models = registry();
  if (auto it = models.find(m_dir); it != models.end() && it->second.expired()) {
    models.erase(it);
  }
}

void FileModel::startWatch() {
  std::weak_ptr<FileModel> weak = weak_from_this();
  m_watch = m_watcher.watch(m_dir, [weak](DirectoryWatcher::Event event, const std::string& name, const std::string& to) {
    if (auto self = weak.lock()) {
      self->onWatch(event, name, to);
    }
  });
}

void FileModel::reload() {
  const std::uint64_t generation = ++m_generation;
  m_listing = true;
  m_pending.clear();
  if (m_statJob != nullptr) {
    m_statJob->cancelled = true;
    m_statJob.reset();
  }
  // The watch starts before the read, so no change between the two is lost.
  if (m_watch < 0 && m_status != Status::Loading) {
    startWatch();
  }
  std::weak_ptr<FileModel> weak = weak_from_this();
  std::thread([weak, generation, dir = m_dir] {
    auto listing = std::make_shared<Listing>(listNames(dir));
    std::shared_ptr<StatJob> job;
    if (listing->statFd >= 0) {
      job = std::make_shared<StatJob>(listing->statFd, listing->entries);
      job->model = weak;
    }
    DeferredCall::callLater([weak, generation, listing, job] {
      auto self = weak.lock();
      if (self == nullptr || generation != self->m_generation) {
        if (job != nullptr) {
          job->cancelled = true;
        }
        return;
      }
      self->m_status = listing->status;
      self->m_error = std::move(listing->error);
      self->m_entries = std::move(listing->entries);
      self->m_statJob = job;
      self->m_listing = false;
      self->notify({.type = Change::Type::Reset});
      // A change that raced the listing may be in it already; a flush stats each name again, so it is idempotent.
      self->flushWatch();
    });
    if (job == nullptr) {
      return;
    }
    const std::size_t workers = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kMaxStatWorkers);
    job->workersLeft = workers;
    for (std::size_t i = 1; i < workers; ++i) {
      std::thread([job] { StatJob::run(job); }).detach();
    }
    StatJob::run(job);
  }).detach();
}

void FileModel::prioritizeStat(std::span<const std::shared_ptr<const FileInfo>> rows) {
  if (m_statJob == nullptr) {
    return;
  }
  StatJob& job = *m_statJob;
  std::scoped_lock lock(job.mutex);
  job.priority.clear();
  for (const auto& row : rows) {
    if (row == nullptr || !row->pendingStat) {
      continue;
    }
    const auto it = std::ranges::lower_bound(job.base, *row, nameBefore, [](const auto& p) -> const FileInfo& {
      return *p;
    });
    if (it != job.base.end() && it->get() == row.get()) {
      job.priority.push_back(static_cast<std::size_t>(it - job.base.begin()));
    }
  }
}

void FileModel::applyStats(StatJob& job) {
  if (&job != m_statJob.get()) {
    return;
  }
  std::vector<std::pair<std::size_t, std::shared_ptr<const FileInfo>>> batch;
  bool finished = false;
  {
    std::scoped_lock lock(job.mutex);
    batch = std::exchange(job.batch, {});
    finished = job.finished;
    job.drainPosted = false;
  }
  bool changed = false;
  for (auto& [index, info] : batch) {
    if (index < m_entries.size() && m_entries[index] == job.base[index]) {
      m_entries[index] = std::move(info);
      changed = true;
      continue;
    }
    // Watch events moved the row: find it by name, and keep a newer stat from upsert.
    const auto it = std::ranges::find_if(m_entries, [&](const auto& e) { return e->name == info->name; });
    if (it != m_entries.end() && (*it)->pendingStat) {
      *it = std::move(info);
      changed = true;
    }
  }
  if (finished) {
    m_statJob.reset();
  }
  if (changed || finished) {
    notify({.type = Change::Type::Updated});
  }
}

void FileModel::insertSorted(std::shared_ptr<const FileInfo> info) {
  const auto it = std::ranges::upper_bound(m_entries, *info, nameBefore, [](const auto& p) -> const FileInfo& {
    return *p;
  });
  m_entries.insert(it, std::move(info));
}

const FileInfo* FileModel::find(std::string_view name) const {
  const auto it = std::ranges::find_if(m_entries, [&](const auto& e) { return e->name == name; });
  return it == m_entries.end() ? nullptr : it->get();
}

void FileModel::onWatch(DirectoryWatcher::Event event, const std::string& name, const std::string& newName) {
  switch (event) {
  case DirectoryWatcher::Event::Created:
  case DirectoryWatcher::Event::Changed:
  case DirectoryWatcher::Event::Deleted:
  case DirectoryWatcher::Event::Renamed:
    m_pending.insert(name);
    if (!newName.empty()) {
      m_pending.insert(newName);
    }
    // A fixed window from the first event, not reset by later ones: a steady stream still shows every 50 ms.
    if (!m_listing && !m_flush.active()) {
      m_flush.start(std::chrono::milliseconds(50), [this] { flushWatch(); });
    }
    break;
  case DirectoryWatcher::Event::Gone:
    m_flush.stop();
    m_pending.clear();
    m_watcher.unwatch(m_watch);
    m_watch = -1;
    m_entries.clear();
    m_status = Status::Missing;
    notify({.type = Change::Type::Reset});
    break;
  case DirectoryWatcher::Event::Overflow:
    kLog.info("watch overflow in {}; listing again", m_dir.string());
    m_flush.stop();
    reload();
    break;
  }
}

void FileModel::flushWatch() {
  m_flush.stop();
  if (m_listing || m_pending.empty()) {
    return;
  }
  const std::set<std::string> names = std::exchange(m_pending, {});
  std::unordered_map<std::string, std::shared_ptr<const FileInfo>> old;
  std::erase_if(m_entries, [&](const auto& e) {
    if (!names.contains(e->name)) {
      return false;
    }
    old.emplace(e->name, e);
    return true;
  });
  bool changed = false;
  std::vector<std::string> modified;
  std::vector<std::shared_ptr<const FileInfo>> fresh;
  for (const std::string& name : names) {
    auto info = statFile(m_dir / name);
    const auto was = old.find(name);
    if (!info) {
      changed = changed || was != old.end();
      continue;
    }
    // A Changed for an unchanged stat (IN_CLOSE_WRITE after IN_MODIFY) is no change.
    if (was != old.end() && was->second->size == info->size && was->second->mtimeNs == info->mtimeNs &&
        was->second->mode == info->mode) {
      fresh.push_back(was->second);
      continue;
    }
    changed = true;
    if (was != old.end()) {
      modified.push_back(name);
    }
    fresh.push_back(std::make_shared<const FileInfo>(std::move(*info)));
  }
  const auto key = [](const auto& p) -> const FileInfo& { return *p; };
  std::ranges::sort(fresh, nameBefore, key);
  std::vector<std::shared_ptr<const FileInfo>> merged;
  merged.reserve(m_entries.size() + fresh.size());
  std::ranges::merge(m_entries, fresh, std::back_inserter(merged), nameBefore, key, key);
  m_entries = std::move(merged);
  if (changed) {
    notify({.type = Change::Type::Updated, .modified = std::move(modified)});
  }
}

std::shared_ptr<void> FileModel::subscribe(Listener listener) {
  auto token = std::make_shared<Listener>(std::move(listener));
  std::erase_if(m_listeners, [](const auto& w) { return w.expired(); });
  m_listeners.push_back(token);
  return token;
}

void FileModel::notify(const Change& change) {
  const auto listeners = m_listeners;
  for (const auto& weak : listeners) {
    if (auto listener = weak.lock(); listener && *listener) {
      (*listener)(change);
    }
  }
}

FileModelView::FileModelView(std::shared_ptr<FileModel> model) : m_model(std::move(model)) {
  m_subscription = m_model->subscribe([this](const FileModel::Change&) { rebuild(); });
  rebuild();
}

void FileModelView::setSort(FileSort sort) {
  m_sort = sort;
  rebuild();
}

void FileModelView::setShowHidden(bool show) {
  m_showHidden = show;
  rebuild();
}

void FileModelView::setFilter(std::string text) {
  m_filter = folded(text);
  rebuild();
}

void FileModelView::setPredicate(std::function<bool(const FileInfo&)> predicate) {
  m_predicate = std::move(predicate);
  rebuild();
}

bool FileModelView::before(const FileInfo& a, const FileInfo& b, const FileSort& sort) {
  if (sort.foldersFirst && a.isDir != b.isDir) {
    return a.isDir;
  }
  int cmp = 0;
  switch (sort.key) {
  case FileSortKey::Name:
    break;
  case FileSortKey::Size:
    cmp = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
    break;
  case FileSortKey::Modified:
    cmp = a.mtimeNs < b.mtimeNs ? -1 : a.mtimeNs > b.mtimeNs ? 1 : 0;
    break;
  case FileSortKey::Kind:
    cmp = a.contentType.compare(b.contentType);
    break;
  case FileSortKey::Added:
    cmp = a.addedNs < b.addedNs ? -1 : a.addedNs > b.addedNs ? 1 : 0;
    break;
  }
  // Ties fall back to the name, then the raw bytes, so the order is total and stable across rebuilds.
  if (cmp == 0) {
    cmp = a.collateKey.compare(b.collateKey);
  }
  if (cmp == 0) {
    cmp = a.name.compare(b.name);
  }
  return sort.descending ? cmp > 0 : cmp < 0;
}

void FileModelView::rebuild() {
  m_rows.clear();
  for (const auto& entry : m_model->entries()) {
    if (entry->hidden && !m_showHidden) {
      continue;
    }
    if (!m_filter.empty() && folded(entry->displayName).find(m_filter) == std::string::npos) {
      continue;
    }
    if (m_predicate && !m_predicate(*entry)) {
      continue;
    }
    m_rows.push_back(entry);
  }
  if (m_sort.key == FileSortKey::Name) {
    // The model keeps name order, so a name sort is a reverse and a stable partition: O(n), no compares.
    if (m_sort.descending) {
      std::ranges::reverse(m_rows);
    }
    if (m_sort.foldersFirst) {
      std::ranges::stable_partition(m_rows, [](const auto& row) { return row->isDir; });
    }
  } else {
    std::ranges::sort(m_rows, [this](const auto& a, const auto& b) { return before(*a, *b, m_sort); });
  }
  if (m_onChanged) {
    m_onChanged();
  }
}

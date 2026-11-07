#include "core/files/file_model.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <map>
#include <thread>
#include <utility>
#include <unistd.h>

namespace {
  constexpr Logger kLog("file-model");

  struct Listing {
    FileModel::Status status = FileModel::Status::Ready;
    std::string error;
    std::vector<std::shared_ptr<const FileInfo>> entries;
  };

  // Runs on a worker thread: a slow or hung mount must not stall the UI.
  Listing listDirectory(const std::filesystem::path& dir) {
    Listing out;
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      out.status = errno == ENOENT || errno == ENOTDIR ? FileModel::Status::Missing
          : errno == EACCES || errno == EPERM          ? FileModel::Status::Denied
                                                       : FileModel::Status::Error;
      out.error = std::strerror(errno);
      return out;
    }
    DIR* d = ::fdopendir(fd);
    if (d == nullptr) {
      ::close(fd);
      out.status = FileModel::Status::Error;
      out.error = std::strerror(errno);
      return out;
    }
    while (const dirent* ent = ::readdir(d)) {
      const std::string_view name = ent->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      if (auto info = statFile(fd, name)) {
        out.entries.push_back(std::make_shared<const FileInfo>(std::move(*info)));
      }
    }
    ::closedir(d);
    return out;
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
  m_queued.clear();
  if (m_watch < 0 && m_status != Status::Loading) {
    startWatch();
  }
  std::weak_ptr<FileModel> weak = weak_from_this();
  std::thread([weak, generation, dir = m_dir] {
    auto listing = std::make_shared<Listing>(listDirectory(dir));
    DeferredCall::callLater([weak, generation, listing] {
      auto self = weak.lock();
      if (self == nullptr || generation != self->m_generation) {
        return;
      }
      self->m_status = listing->status;
      self->m_error = std::move(listing->error);
      self->m_entries = std::move(listing->entries);
      self->m_listing = false;
      self->notify({.type = Change::Type::Reset});
      // A change that raced the listing may be in it already; upsert and erase are idempotent.
      for (const QueuedEvent& e : std::exchange(self->m_queued, {})) {
        self->onWatch(e.event, e.name, e.newName);
      }
    });
  }).detach();
}

const FileInfo* FileModel::find(std::string_view name) const {
  const auto it = std::ranges::find_if(m_entries, [&](const auto& e) { return e->name == name; });
  return it == m_entries.end() ? nullptr : it->get();
}

void FileModel::upsert(const std::string& name, bool inserted) {
  auto info = statFile(m_dir / name);
  if (!info) {
    erase(name);
    return;
  }
  auto it = std::ranges::find_if(m_entries, [&](const auto& e) { return e->name == name; });
  if (it == m_entries.end()) {
    m_entries.push_back(std::make_shared<const FileInfo>(std::move(*info)));
    notify({.type = Change::Type::Inserted, .name = name});
    return;
  }
  // A Changed for an unchanged stat (IN_CLOSE_WRITE after IN_MODIFY) is no change.
  if (!inserted && (*it)->size == info->size && (*it)->mtimeNs == info->mtimeNs && (*it)->mode == info->mode) {
    return;
  }
  *it = std::make_shared<const FileInfo>(std::move(*info));
  notify({.type = Change::Type::Updated, .name = name});
}

void FileModel::erase(const std::string& name) {
  if (std::erase_if(m_entries, [&](const auto& e) { return e->name == name; }) > 0) {
    notify({.type = Change::Type::Removed, .name = name});
  }
}

void FileModel::onWatch(DirectoryWatcher::Event event, const std::string& name, const std::string& newName) {
  if (m_listing && event != DirectoryWatcher::Event::Gone && event != DirectoryWatcher::Event::Overflow) {
    m_queued.push_back({event, name, newName});
    return;
  }
  switch (event) {
  case DirectoryWatcher::Event::Created:
    upsert(name, true);
    break;
  case DirectoryWatcher::Event::Changed:
    upsert(name, false);
    break;
  case DirectoryWatcher::Event::Deleted:
    erase(name);
    break;
  case DirectoryWatcher::Event::Renamed: {
    auto info = statFile(m_dir / newName);
    std::erase_if(m_entries, [&](const auto& e) { return e->name == name || e->name == newName; });
    if (info) {
      m_entries.push_back(std::make_shared<const FileInfo>(std::move(*info)));
      notify({.type = Change::Type::Renamed, .name = name, .newName = newName});
    } else {
      notify({.type = Change::Type::Removed, .name = name});
    }
    break;
  }
  case DirectoryWatcher::Event::Gone:
    m_watcher.unwatch(m_watch);
    m_watch = -1;
    m_entries.clear();
    m_status = Status::Missing;
    notify({.type = Change::Type::Reset});
    break;
  case DirectoryWatcher::Event::Overflow:
    kLog.info("watch overflow in {}; listing again", m_dir.string());
    reload();
    break;
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
  std::ranges::sort(m_rows, [this](const auto& a, const auto& b) { return before(*a, *b, m_sort); });
  if (m_onChanged) {
    m_onChanged();
  }
}

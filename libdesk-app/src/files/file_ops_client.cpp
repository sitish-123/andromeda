#include "files/file_ops_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <algorithm>
#include <set>

namespace {
  constexpr Logger kLog("file-ops");
  const sdbus::ServiceName kName{"org.andromeda.FileOps1"};
  const sdbus::ObjectPath kPath{"/org/andromeda/FileOps1"};
  constexpr auto kInterface = "org.andromeda.FileOps1";
  constexpr auto kJobInterface = "org.andromeda.FileOps1.Job";
  constexpr auto kProperties = "org.freedesktop.DBus.Properties";

  using Bytes = std::vector<std::uint8_t>;

  Bytes bytes(const std::filesystem::path& path) {
    const std::string& s = path.native();
    return {s.begin(), s.end()};
  }

  std::vector<Bytes> bytesList(const std::vector<std::filesystem::path>& paths) {
    std::vector<Bytes> out;
    out.reserve(paths.size());
    for (const auto& p : paths) {
      out.push_back(bytes(p));
    }
    return out;
  }

  std::filesystem::path pathFrom(const Bytes& b) { return std::string(b.begin(), b.end()); }

  const char* resolutionName(FileOpsClient::Resolution r) {
    switch (r) {
    case FileOpsClient::Resolution::Replace:
      return "replace";
    case FileOpsClient::Resolution::Skip:
      return "skip";
    case FileOpsClient::Resolution::KeepBoth:
      return "keep_both";
    case FileOpsClient::Resolution::Merge:
      return "merge";
    }
    return "skip";
  }
} // namespace

FileOpsClient::FileOpsClient(SessionBus& bus) : m_bus(bus) {
  m_manager = sdbus::createProxy(m_bus.connection(), kName, kPath);
  m_manager->uponSignal("PropertiesChanged")
      .onInterface(kProperties)
      .call([this](
                const std::string& iface, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (iface != kInterface) {
          return;
        }
        if (const auto it = changed.find("Jobs"); it != changed.end()) {
          syncJobs(it->second.get<std::vector<sdbus::ObjectPath>>());
        }
      });
  // The daemon is D-Bus activated: this read starts it when it is not running.
  std::weak_ptr<bool> alive = m_alive;
  m_manager->getPropertyAsync("Jobs").onInterface(kInterface).uponReplyInvoke(
      [this, alive](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (alive.expired()) {
          return;
        }
        if (error.has_value()) {
          kLog.warn("FileOps1 is not reachable: {}", error->getMessage());
          return;
        }
        syncJobs(value.get<std::vector<sdbus::ObjectPath>>());
      }
  );
}

FileOpsClient::~FileOpsClient() = default;

const FileOpsClient::Job* FileOpsClient::job(const std::string& path) const {
  const auto it = m_jobs.find(path);
  return it == m_jobs.end() ? nullptr : &it->second;
}

void FileOpsClient::start(const char* method, std::function<void(sdbus::AsyncMethodInvoker&)> args, Started done) {
  std::weak_ptr<bool> alive = m_alive;
  try {
    auto call = m_manager->callMethodAsync(method);
    call.onInterface(kInterface);
    args(call);
    call.uponReplyInvoke([this, alive, done = std::move(done), method](
                             std::optional<sdbus::Error> error, sdbus::ObjectPath job
                         ) {
      if (alive.expired()) {
        return;
      }
      if (error.has_value()) {
        kLog.warn("{} failed: {}", method, error->getMessage());
        if (done) {
          done({}, error->getMessage());
        }
        return;
      }
      track(job);
      if (done) {
        done(job, {});
      }
    });
  } catch (const sdbus::Error& e) {
    if (done) {
      done({}, e.getMessage());
    }
  }
}

void FileOpsClient::copy(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destDir, Started done) {
  start("Copy", [&](auto& c) { c.withArguments(bytesList(sources), bytes(destDir)); }, std::move(done));
}

void FileOpsClient::move(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destDir, Started done) {
  start("Move", [&](auto& c) { c.withArguments(bytesList(sources), bytes(destDir)); }, std::move(done));
}

void FileOpsClient::rename(const std::filesystem::path& path, const std::string& name, Started done) {
  start("Rename", [&](auto& c) { c.withArguments(bytes(path), name); }, std::move(done));
}

void FileOpsClient::renameMany(const std::vector<std::pair<std::filesystem::path, std::string>>& items, Started done) {
  std::vector<sdbus::Struct<Bytes, std::string>> args;
  for (const auto& [path, name] : items) {
    args.emplace_back(bytes(path), name);
  }
  start("RenameMany", [&](auto& c) { c.withArguments(args); }, std::move(done));
}

void FileOpsClient::newFolder(const std::filesystem::path& destDir, const std::string& name, Started done) {
  start("NewFolder", [&](auto& c) { c.withArguments(bytes(destDir), name); }, std::move(done));
}

void FileOpsClient::link(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destDir, Started done) {
  start("Link", [&](auto& c) { c.withArguments(bytesList(sources), bytes(destDir)); }, std::move(done));
}

void FileOpsClient::listArchive(const std::filesystem::path& archive, ArchiveListed done) {
  std::weak_ptr<bool> alive = m_alive;
  try {
    m_manager->callMethodAsync("ListArchive")
        .onInterface(kInterface)
        .withArguments(bytes(archive))
        .uponReplyInvoke([alive, done = std::move(done)](
                             std::optional<sdbus::Error> error,
                             std::vector<sdbus::Struct<Bytes, std::string, std::int64_t, std::uint64_t, bool>> raw
                         ) {
          if (alive.expired() || !done) {
            return;
          }
          if (error.has_value()) {
            done({}, error->getMessage(), error->getName() == "org.andromeda.FileOps1.Error.PasswordNeeded");
            return;
          }
          std::vector<ArchiveEntry> entries;
          entries.reserve(raw.size());
          for (const auto& [path, kind, size, mtime, isDir] : raw) {
            entries.push_back({.path = pathFrom(path), .kind = kind, .size = size, .mtime = mtime, .isDir = isDir});
          }
          done(std::move(entries), {}, false);
        });
  } catch (const sdbus::Error& e) {
    if (done) {
      done({}, e.getMessage(), false);
    }
  }
}

void FileOpsClient::extract(const std::filesystem::path& archive, const std::vector<std::filesystem::path>& innerPaths,
                            const std::filesystem::path& destDir, const std::string& password, Started done) {
  start("Extract", [&](auto& c) { c.withArguments(bytes(archive), bytesList(innerPaths), bytes(destDir), password); },
        std::move(done));
}

void FileOpsClient::compress(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& archive,
                             const std::string& format, const std::string& password, std::uint64_t volumeSize, Started done) {
  start("Compress", [&](auto& c) { c.withArguments(bytesList(sources), bytes(archive), format, password, volumeSize); },
        std::move(done));
}

void FileOpsClient::setPermissions(const std::vector<std::filesystem::path>& targets, std::uint32_t mode,
                                   std::uint32_t mask, bool recursive, Started done) {
  start("SetPermissions", [&](auto& c) { c.withArguments(bytesList(targets), mode, mask, recursive); }, std::move(done));
}

void FileOpsClient::setTags(const std::vector<std::filesystem::path>& targets, const std::vector<std::string>& tags,
                            const std::string& mode, Started done) {
  start("SetTags", [&](auto& c) { c.withArguments(bytesList(targets), tags, mode); }, std::move(done));
}

void FileOpsClient::checksum(const std::filesystem::path& path, const std::string& algorithm, Started started,
                             std::function<void(const std::string& hex, const std::string& error)> finished) {
  startWithResult("Checksum", [&](auto& c) { c.withArguments(bytes(path), algorithm); }, std::move(started),
                  std::move(finished));
}

void FileOpsClient::storageScan(const std::filesystem::path& root, bool fresh, Started started,
                                std::function<void(const std::string& json, const std::string& error)> finished) {
  startWithResult("StorageScan", [&](auto& c) { c.withArguments(bytes(root), fresh); }, std::move(started),
                  std::move(finished));
}

void FileOpsClient::storageFolder(const std::filesystem::path& folder, FolderSizes done) {
  std::weak_ptr<bool> alive = m_alive;
  try {
    m_manager->callMethodAsync("StorageFolder")
        .onInterface(kInterface)
        .withArguments(bytes(folder))
        .uponReplyInvoke([alive, done = std::move(done)](std::optional<sdbus::Error> error,
                                                         std::vector<sdbus::Struct<Bytes, std::uint64_t, bool>> raw) {
          if (alive.expired() || !done) {
            return;
          }
          if (error.has_value()) {
            done({}, error->getMessage());
            return;
          }
          std::vector<SizedPath> items;
          items.reserve(raw.size());
          for (const auto& [path, size, isDir] : raw) {
            items.push_back({.path = pathFrom(path), .bytes = size, .isDir = isDir});
          }
          done(std::move(items), {});
        });
  } catch (const sdbus::Error& e) {
    if (done) {
      done({}, e.getMessage());
    }
  }
}

void FileOpsClient::startWithResult(const char* method, std::function<void(sdbus::AsyncMethodInvoker&)> args,
                                    Started started,
                                    std::function<void(const std::string& result, const std::string& error)> finished) {
  start(
      method, std::move(args),
      [this, started = std::move(started), finished = std::move(finished)](const std::string& job, const std::string& error) {
        if (started) {
          started(job, error);
        }
        if (job.empty()) {
          if (finished) {
            finished({}, error);
          }
          return;
        }
        m_finishWaiters[job] = [finished](const Job& j) {
          if (finished) {
            finished(j.state == "done" ? j.result : std::string(), j.state == "done" ? std::string() : j.error.empty() ? j.state : j.error);
          }
        };
        // The job may have settled before this reply came back.
        if (const Job* j = this->job(job); j != nullptr && j->finished() && !m_settling.contains(job)) {
          notify(*j);
        }
      });
}

void FileOpsClient::trash(const std::vector<std::filesystem::path>& sources, Started done) {
  start("Trash", [&](auto& c) { c.withArguments(bytesList(sources)); }, std::move(done));
}

void FileOpsClient::remove(const std::vector<std::filesystem::path>& sources, Started done) {
  start("Delete", [&](auto& c) { c.withArguments(bytesList(sources)); }, std::move(done));
}

void FileOpsClient::restore(const std::vector<std::pair<std::filesystem::path, std::string>>& items, Started done) {
  std::vector<sdbus::Struct<Bytes, std::string>> args;
  for (const auto& [dir, name] : items) {
    args.emplace_back(bytes(dir), name);
  }
  start("Restore", [&](auto& c) { c.withArguments(args); }, std::move(done));
}

void FileOpsClient::undo(const std::string& jobPath, Started done) {
  start("Undo", [&](auto& c) { c.withArguments(sdbus::ObjectPath{jobPath}); }, std::move(done));
}

void FileOpsClient::cancel(const std::string& jobPath) {
  if (const auto it = m_jobProxies.find(jobPath); it != m_jobProxies.end()) {
    it->second->callMethodAsync("Cancel").onInterface(kJobInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
  }
}

void FileOpsClient::pause(const std::string& jobPath) {
  if (const auto it = m_jobProxies.find(jobPath); it != m_jobProxies.end()) {
    it->second->callMethodAsync("Pause").onInterface(kJobInterface).uponReplyInvoke([jobPath](std::optional<sdbus::Error> error) {
      if (error.has_value()) {
        kLog.warn("Pause on {} failed: {}", jobPath, error->getMessage());
      }
    });
  }
}

void FileOpsClient::resume(const std::string& jobPath) {
  if (const auto it = m_jobProxies.find(jobPath); it != m_jobProxies.end()) {
    it->second->callMethodAsync("Resume").onInterface(kJobInterface).uponReplyInvoke([jobPath](std::optional<sdbus::Error> error) {
      if (error.has_value()) {
        kLog.warn("Resume on {} failed: {}", jobPath, error->getMessage());
      }
    });
  }
}

void FileOpsClient::resolveConflict(const std::string& jobPath, Resolution resolution, bool applyToAll) {
  if (const auto it = m_jobProxies.find(jobPath); it != m_jobProxies.end()) {
    it->second->callMethodAsync("ResolveConflict")
        .onInterface(kJobInterface)
        .withArguments(std::string(resolutionName(resolution)), applyToAll)
        .uponReplyInvoke([jobPath](std::optional<sdbus::Error> error) {
          if (error.has_value()) {
            kLog.warn("ResolveConflict on {} failed: {}", jobPath, error->getMessage());
          }
        });
  }
}

void FileOpsClient::syncJobs(const std::vector<sdbus::ObjectPath>& paths) {
  const std::set<std::string> live(paths.begin(), paths.end());
  for (auto it = m_jobs.begin(); it != m_jobs.end();) {
    if (live.contains(it->first)) {
      ++it;
      continue;
    }
    const std::string gone = it->first;
    m_jobProxies.erase(gone);
    m_settling.erase(gone);
    it = m_jobs.erase(it);
    if (m_onJobRemoved) {
      m_onJobRemoved(gone);
    }
  }
  for (const std::string& path : live) {
    track(path);
  }
}

void FileOpsClient::track(const std::string& path) {
  if (m_jobProxies.contains(path)) {
    return;
  }
  auto proxy = sdbus::createProxy(m_bus.connection(), kName, sdbus::ObjectPath{path});
  proxy->uponSignal("PropertiesChanged")
      .onInterface(kProperties)
      .call([this, path](
                const std::string& iface, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (iface != kJobInterface) {
          return;
        }
        Job& job = m_jobs[path];
        const bool wasFinished = job.finished();
        apply(job, changed, false);
        if (!wasFinished && job.finished()) {
          // The daemon signals State before Undoable, Error and the final counts, one signal each: hold every change
          // until a GetAll has read them all.
          m_settling.insert(path);
          refresh(path);
        } else if (!m_settling.contains(path)) {
          notify(job);
        }
      });
  m_jobs[path].path = path;
  m_jobProxies.emplace(path, std::move(proxy));
  refresh(path);
}

void FileOpsClient::refresh(const std::string& path) {
  const auto it = m_jobProxies.find(path);
  if (it == m_jobProxies.end()) {
    return;
  }
  std::weak_ptr<bool> alive = m_alive;
  it->second->callMethodAsync("GetAll")
      .onInterface(kProperties)
      .withArguments(std::string(kJobInterface))
      .uponReplyInvoke([this, alive, path](std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> all) {
        if (alive.expired() || !m_jobProxies.contains(path)) {
          return;
        }
        m_settling.erase(path);
        if (error.has_value()) {
          kLog.warn("job {} properties: {}", path, error->getMessage());
          notify(m_jobs[path]);
          return;
        }
        apply(m_jobs[path], all, true);
      });
}

void FileOpsClient::notify(const Job& job) {
  if (job.finished() && !m_settling.contains(job.path)) {
    if (auto it = m_finishWaiters.find(job.path); it != m_finishWaiters.end()) {
      auto waiter = std::move(it->second);
      m_finishWaiters.erase(it);
      waiter(job);
    }
  }
  if (m_onJobChanged) {
    m_onJobChanged(job);
  }
}

void FileOpsClient::apply(Job& job, const std::map<std::string, sdbus::Variant>& props, bool notifyChange) {
  const auto read = [&]<typename T>(const char* name, T& out) {
    if (const auto it = props.find(name); it != props.end()) {
      try {
        out = it->second.get<T>();
      } catch (const sdbus::Error&) {
      }
    }
  };
  read("Kind", job.kind);
  read("State", job.state);
  read("TotalFiles", job.totalFiles);
  read("DoneFiles", job.doneFiles);
  read("TotalBytes", job.totalBytes);
  read("DoneBytes", job.doneBytes);
  read("Error", job.error);
  read("Undoable", job.undoable);
  read("Result", job.result);
  if (const auto it = props.find("Moves"); it != props.end()) {
    job.moves.clear();
    for (const auto& pair : it->second.get<std::vector<sdbus::Struct<Bytes, Bytes>>>()) {
      const auto& [from, to] = pair;
      job.moves.emplace_back(pathFrom(from), pathFrom(to));
    }
  }
  if (const auto it = props.find("Current"); it != props.end()) {
    job.current = pathFrom(it->second.get<Bytes>());
  }
  if (const auto it = props.find("Destination"); it != props.end()) {
    job.destination = pathFrom(it->second.get<Bytes>());
  }
  if (const auto it = props.find("Conflict"); it != props.end()) {
    const auto pair = it->second.get<sdbus::Struct<Bytes, Bytes>>();
    const auto& [source, target] = pair;
    job.conflict = source.empty() ? std::nullopt
                                  : std::optional{std::pair{pathFrom(source), pathFrom(target)}};
  }
  if (const auto it = props.find("ConflictProgress"); it != props.end()) {
    try {
      const auto [index, total] = it->second.get<sdbus::Struct<std::uint64_t, std::uint64_t>>();
      job.conflictIndex = index;
      job.conflictTotal = total;
    } catch (const sdbus::Error&) {
    }
  }
  if (const auto it = props.find("FolderClash"); it != props.end()) {
    try {
      const auto clash =
          it->second.get<sdbus::Struct<std::uint64_t, std::uint64_t, std::uint64_t, std::vector<sdbus::Struct<Bytes, std::string>>>>();
      const auto& [fresh, same, different, names] = clash;
      job.folderClash.reset();
      if (fresh + same + different > 0) {
        Job::FolderClash out{.newFiles = fresh, .sameFiles = same, .differentFiles = different, .different = {}};
        for (const auto& [path, hint] : names) {
          out.different.emplace_back(pathFrom(path), hint);
        }
        job.folderClash = std::move(out);
      }
    } catch (const sdbus::Error&) {
    }
  }
  if (notifyChange) {
    notify(job);
  }
}

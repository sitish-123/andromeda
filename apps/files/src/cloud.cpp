#include "cloud.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <csignal>
#include <fstream>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-cloud");

  struct Result {
    int status = -1;
    std::string out;
    std::string err;
  };
  using Finished = std::function<void(Result)>;

  // Runs argv; done(exit status, stdout, stderr) on the main loop. Returns the pid (0 when it did not start).
  int run(const std::vector<std::string>& argv, Finished done, const std::string& input = {}) {
    std::vector<const char*> args;
    for (const std::string& a : argv) {
      args.push_back(a.c_str());
    }
    args.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* proc = g_subprocess_newv(
        args.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);
    if (proc == nullptr) {
      Result r{.status = -1, .out = {}, .err = error != nullptr ? error->message : "could not start"};
      g_clear_error(&error);
      done(std::move(r));
      return 0;
    }
    const char* id = g_subprocess_get_identifier(proc);
    const int pid = id != nullptr ? std::atoi(id) : 0;
    auto* cb = new Finished(std::move(done));
    g_subprocess_communicate_utf8_async(
        proc, input.c_str(), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Finished> fn(static_cast<Finished*>(data));
          auto* p = G_SUBPROCESS(source);
          gchar* out = nullptr;
          gchar* err = nullptr;
          g_subprocess_communicate_utf8_finish(p, result, &out, &err, nullptr);
          Result r{.status = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1,
                   .out = out != nullptr ? out : "", .err = err != nullptr ? err : ""};
          g_free(out);
          g_free(err);
          g_object_unref(p);
          (*fn)(std::move(r));
        },
        cb);
    return pid;
  }

  // The last non-empty stderr line without rclone's "2026/11/08 10:00:00 ERROR : " prefix.
  std::string lastLine(const std::string& text) {
    std::string line;
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t nl = text.find('\n', start);
      if (nl == std::string::npos) {
        nl = text.size();
      }
      if (nl > start) {
        line = text.substr(start, nl - start);
      }
      start = nl + 1;
    }
    if (const auto colon = line.find(" : "); colon != std::string::npos) {
      line = line.substr(colon + 3);
    }
    return line;
  }

  fs::path configFile() {
    if (const char* env = g_getenv("RCLONE_CONFIG"); env != nullptr && *env != '\0') {
      return env;
    }
    return fs::path(g_get_user_config_dir()) / "rclone" / "rclone.conf";
  }

  fs::path cacheDir() { return fs::path(g_get_user_cache_dir()) / "andromeda" / "rclone"; }

  fs::path socketFor(const std::string& remote) {
    return fs::path(g_get_user_runtime_dir()) / ("andromeda-rclone-" + remote + ".sock");
  }

  fs::path pinsFile() { return fs::path(g_get_user_state_dir()) / "andromeda" / "files" / "cloud.json"; }

  bool isMount(const fs::path& path) {
    GUnixMountEntry* entry = g_unix_mount_entry_at(path.c_str(), nullptr);
    if (entry == nullptr) {
      return false;
    }
    g_unix_mount_entry_free(entry);
    return true;
  }

  void startGuard(const fs::path& mount) {
    std::error_code ec;
    const std::string self = fs::read_symlink("/proc/self/exe", ec).string();
    const char* argv[] = {self.c_str(), "--vault-guard", mount.c_str(), nullptr};
    GError* error = nullptr;
    if (!g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr, static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL),
                       nullptr, nullptr, nullptr, &error)) {
      kLog.warn("cloud guard did not start: {}", error->message);
      g_clear_error(&error);
    }
  }

  // Regular files at or under each path (symlinks not followed).
  std::vector<fs::path> filesUnder(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& p : paths) {
      if (fs::is_directory(fs::symlink_status(p, ec))) {
        for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
          if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
            out.push_back(it->path());
          }
        }
      } else if (fs::is_regular_file(fs::symlink_status(p, ec))) {
        out.push_back(p);
      }
    }
    return out;
  }
} // namespace

Cloud::Cloud() {
  reload();
  loadPins();
}

Cloud::~Cloud() { cancelAdd(); }

bool Cloud::available() {
  gchar* path = g_find_program_in_path("rclone");
  const bool found = path != nullptr;
  g_free(path);
  return found;
}

const std::vector<Cloud::Provider>& Cloud::providers() {
  static const std::vector<Provider> kProviders{
      {"drive", "Google Drive", true},      {"onedrive", "OneDrive", true},
      {"dropbox", "Dropbox", true},          {"box", "Box", true},
      {"iclouddrive", "iCloud Drive", false}, {"webdav", "Nextcloud", false},
      {"protondrive", "Proton Drive", false}, {"googlephotos", "Google Photos", true},
  };
  return kProviders;
}

void Cloud::reload() {
  m_remotes.clear();
  std::ifstream in(configFile());
  std::string line;
  Remote current;
  const auto flush = [&] {
    if (!current.name.empty()) {
      m_remotes.push_back(current);
    }
    current = {};
  };
  while (std::getline(in, line)) {
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#' || line[first] == ';') {
      continue;
    }
    if (line[first] == '[') {
      flush();
      const auto close = line.find(']', first);
      current.name = close != std::string::npos ? line.substr(first + 1, close - first - 1) : std::string();
      continue;
    }
    const auto eq = line.find('=');
    if (eq != std::string::npos && !current.name.empty()) {
      std::string key = line.substr(first, eq - first);
      key.erase(key.find_last_not_of(" \t") + 1);
      if (key == "type") {
        std::string value = line.substr(eq + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        current.type = value;
      }
    }
  }
  flush();
  kLog.debug("cloud remotes {}", m_remotes.size());
}

fs::path Cloud::mountPoint(const std::string& remote) { return fs::path(g_get_home_dir()) / "Cloud" / remote; }

bool Cloud::mounted(const std::string& remote) { return isMount(mountPoint(remote)); }

std::string Cloud::remoteOf(const fs::path& path) const {
  const fs::path base = fs::path(g_get_home_dir()) / "Cloud";
  const auto rel = path.lexically_relative(base);
  if (rel.empty() || rel.begin() == rel.end() || *rel.begin() == "..") {
    return {};
  }
  const std::string name = rel.begin()->string();
  return std::ranges::any_of(m_remotes, [&](const Remote& r) { return r.name == name; }) ? name : std::string();
}

void Cloud::mount(const std::string& remote, Done done) {
  const fs::path mp = mountPoint(remote);
  if (isMount(mp)) {
    done({});
    return;
  }
  std::error_code ec;
  fs::create_directories(mp, ec);
  fs::create_directories(cacheDir(), ec);
  fs::remove(socketFor(remote), ec);
  kLog.info("cloud mount {}", remote);
  // --daemon: rclone forks once the mount is up, so the exit status is the mount result.
  run({"rclone", "mount", remote + ":", mp.string(), "--daemon", "--vfs-cache-mode", "full", "--vfs-cache-max-age",
       "8760h", "--cache-dir", cacheDir().string(), "--rc", "--rc-addr", "unix://" + socketFor(remote).string(),
       "--rc-no-auth", "--volname", remote},
      [remote, mp, done = std::move(done)](Result r) {
        if (r.status == 0 && isMount(mp)) {
          kLog.info("cloud mounted {} at {}", remote, mp.string());
          startGuard(mp);
          done({});
        } else {
          kLog.warn("cloud mount {} failed ({}): {}", remote, r.status, r.err);
          done(r.err.empty() ? std::string("The account could not be opened.") : lastLine(r.err));
        }
      });
}

void Cloud::unmount(const std::string& remote, Done done) {
  run({"fusermount3", "-u", mountPoint(remote).string()}, [remote, done = std::move(done)](Result r) {
    if (r.status == 0) {
      kLog.info("cloud unmounted {}", remote);
      done({});
    } else {
      done(r.err.find("busy") != std::string::npos ? std::string("An app still uses files in this account.")
                                                   : lastLine(r.err));
    }
  });
}

fs::path Cloud::cacheFile(const std::string& remote, const fs::path& rel) const {
  return cacheDir() / "vfs" / remote / rel;
}

Cloud::State Cloud::state(const fs::path& path) const {
  const std::string remote = remoteOf(path);
  if (remote.empty()) {
    return State::None;
  }
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return State::None;
  }
  const fs::path rel = path.lexically_relative(mountPoint(remote));
  if (auto it = m_queued.find(remote); it != m_queued.end() && it->second.contains(rel.string())) {
    return State::Syncing;
  }
  struct stat cached{};
  // The cache data file is sparse: every block present means the whole file is on this device.
  if (::stat(cacheFile(remote, rel).c_str(), &cached) == 0 &&
      static_cast<std::int64_t>(cached.st_blocks) * 512 >= static_cast<std::int64_t>(st.st_size)) {
    return State::OnDevice;
  }
  return State::CloudOnly;
}

void Cloud::refreshQueue(const std::string& remote, std::function<void()> changed) {
  run({"rclone", "rc", "--unix-socket", socketFor(remote).string(), "vfs/queue"},
      [this, remote, changed = std::move(changed), alive = std::weak_ptr<bool>(m_alive)](Result r) {
        if (alive.expired() || r.status != 0) {
          return;
        }
        std::set<std::string> queued;
        const auto doc = nlohmann::json::parse(r.out, nullptr, false);
        for (const auto& item : doc.is_object() ? doc.value("queue", nlohmann::json::array()) : nlohmann::json::array()) {
          if (item.is_object() && item.contains("name") && item["name"].is_string()) {
            queued.insert(item["name"].get<std::string>());
          }
        }
        if (queued != m_queued[remote]) {
          kLog.debug("cloud {} queue {}", remote, queued.size());
          m_queued[remote] = std::move(queued);
          if (changed) {
            changed();
          }
        }
      });
}

void Cloud::keep(const std::vector<fs::path>& paths, std::function<void(std::size_t, const std::string&)> done) {
  for (const fs::path& p : paths) {
    m_pins.insert(p.string());
  }
  savePins();
  std::thread([paths, done = std::move(done), alive = std::weak_ptr<bool>(m_alive)] {
    std::size_t count = 0;
    std::string error;
    std::vector<char> buf(1 << 20);
    for (const fs::path& file : filesUnder(paths)) {
      std::ifstream in(file, std::ios::binary);
      while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
      }
      if (in.bad()) {
        error = file.filename().string() + " could not be downloaded.";
        break;
      }
      ++count;
    }
    DeferredCall::callLater([count, error, done, alive] {
      if (!alive.expired()) {
        kLog.info("cloud kept {} files", count);
        done(count, error);
      }
    });
  }).detach();
}

void Cloud::freeUp(const std::vector<fs::path>& paths, std::function<void(std::size_t)> done) {
  std::size_t count = 0;
  std::set<std::pair<std::string, std::string>> folders; // remote, folder inside it
  for (const fs::path& p : paths) {
    m_pins.erase(p.string());
  }
  savePins();
  for (const fs::path& file : filesUnder(paths)) {
    const std::string remote = remoteOf(file);
    if (remote.empty() || state(file) == State::Syncing) {
      continue;
    }
    const fs::path rel = file.lexically_relative(mountPoint(remote));
    std::error_code ec;
    const fs::path data = cacheFile(remote, rel);
    if (fs::remove(data, ec)) {
      fs::remove(cacheDir() / "vfsMeta" / remote / rel, ec);
      ++count;
      folders.emplace(remote, rel.parent_path().string());
    }
  }
  for (const auto& [remote, folder] : folders) {
    run({"rclone", "rc", "--unix-socket", socketFor(remote).string(), "vfs/forget", "dir=" + folder}, [](Result) {});
  }
  kLog.info("cloud freed {} files", count);
  done(count);
}

void Cloud::shareLink(const fs::path& path, std::function<void(const std::string&, const std::string&)> done) {
  const std::string remote = remoteOf(path);
  if (remote.empty()) {
    done({}, "This item is not in a cloud account.");
    return;
  }
  const std::string rel = path.lexically_relative(mountPoint(remote)).string();
  run({"rclone", "link", remote + ":" + rel}, [done = std::move(done)](Result r) {
    std::string url = r.out;
    url.erase(url.find_last_not_of(" \n\r") + 1);
    if (r.status == 0 && !url.empty()) {
      done(url, {});
    } else {
      kLog.debug("cloud link failed: {}", r.err);
      done({}, r.err.find("not supported") != std::string::npos || r.err.find("doesn't support") != std::string::npos
                   ? std::string("This account has no public links.")
                   : lastLine(r.err));
    }
  });
}

std::string Cloud::nameError(const std::string& name) const {
  if (name.empty()) {
    return "Give the account a name.";
  }
  if (name.find_first_of("/:[]") != std::string::npos || name.starts_with("-") || name.starts_with(" ")) {
    return "A name cannot contain “/”, “:” or brackets.";
  }
  if (std::ranges::any_of(m_remotes, [&](const Remote& r) { return r.name == name; })) {
    return "An account with this name exists.";
  }
  return {};
}

void Cloud::addAccount(const std::string& name, const std::string& type, const std::map<std::string, std::string>& fields,
                       Ask ask, Done done) {
  if (std::string error = nameError(name); !error.empty()) {
    done(error);
    return;
  }
  const auto provider = std::ranges::find_if(providers(), [&](const Provider& p) { return std::string_view(p.type) == type; });
  if (provider == providers().end()) {
    done("This provider is not supported.");
    return;
  }
  std::vector<std::string> argv{"rclone", "config", "create", name, type};
  if (type == "webdav") {
    argv.push_back("vendor=nextcloud");
  }
  for (const auto& [key, value] : fields) {
    argv.push_back(key + "=" + value);
  }
  if (!provider->oauth) {
    argv.push_back("--obscure"); // pass= is plain text: rclone obscures it in the config
  }
  m_adding = name;
  kLog.info("cloud add {} ({})", name, type);
  auto finish = std::make_shared<std::function<void(Result)>>();
  *finish = [this, name, ask, done, finish, alive = std::weak_ptr<bool>(m_alive)](Result r) {
    if (alive.expired()) {
      return;
    }
    if (m_adding != name) {
      return; // cancelled
    }
    // --non-interactive answers with the next question as JSON while rclone needs more (iCloud two-factor code).
    const auto doc = nlohmann::json::parse(r.out, nullptr, false);
    if (r.status == 0 && doc.is_object() && doc.contains("State") && doc["State"].is_string() &&
        !doc["State"].get<std::string>().empty()) {
      const std::string state = doc["State"].get<std::string>();
      const std::string question = doc.contains("Option") && doc["Option"].is_object()
                                       ? doc["Option"].value("Help", std::string("rclone needs one more answer."))
                                       : std::string("rclone needs one more answer.");
      ask(question, [this, name, state, finish](const std::string* text) {
        if (text == nullptr) {
          cancelAdd();
          return;
        }
        m_addPid = run({"rclone", "config", "update", name, "--continue", "--state", state, "--result", *text,
                        "--non-interactive"},
                       *finish);
      });
      return;
    }
    m_adding.clear();
    m_addPid = 0;
    reload();
    if (r.status == 0 && std::ranges::any_of(m_remotes, [&](const Remote& rem) { return rem.name == name; })) {
      kLog.info("cloud added {}", name);
      done({});
    } else {
      kLog.warn("cloud add {} failed ({}): {}", name, r.status, r.err);
      run({"rclone", "config", "delete", name}, [](Result) {});
      done(r.err.empty() ? std::string("The account could not be added.") : lastLine(r.err));
    }
  };
  if (!provider->oauth) {
    argv.push_back("--non-interactive");
  }
  m_addPid = run(argv, *finish);
}

void Cloud::cancelAdd() {
  if (m_adding.empty()) {
    return;
  }
  kLog.info("cloud add {} cancelled", m_adding);
  if (m_addPid > 0) {
    ::kill(m_addPid, SIGTERM);
  }
  run({"rclone", "config", "delete", m_adding}, [](Result) {});
  m_adding.clear();
  m_addPid = 0;
}

void Cloud::removeAccount(const std::string& name, Done done) {
  const auto remove = [this, name, done, alive = std::weak_ptr<bool>(m_alive)] {
    run({"rclone", "config", "delete", name}, [this, name, done, alive](Result r) {
      if (alive.expired()) {
        return;
      }
      reload();
      kLog.info("cloud removed {}", name);
      done(r.status == 0 ? std::string() : lastLine(r.err));
    });
  };
  if (mounted(name)) {
    unmount(name, [remove, done](const std::string& error) {
      if (error.empty()) {
        remove();
      } else {
        done(error);
      }
    });
  } else {
    remove();
  }
}

void Cloud::loadPins() {
  std::ifstream in(pinsFile());
  const auto doc = nlohmann::json::parse(in, nullptr, false);
  if (doc.is_object() && doc.contains("pinned") && doc["pinned"].is_array()) {
    for (const auto& p : doc["pinned"]) {
      if (p.is_string()) {
        m_pins.insert(p.get<std::string>());
      }
    }
  }
}

void Cloud::savePins() const {
  std::error_code ec;
  fs::create_directories(pinsFile().parent_path(), ec);
  std::ofstream(pinsFile()) << nlohmann::json{{"pinned", m_pins}}.dump();
}

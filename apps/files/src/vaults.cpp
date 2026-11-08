#include "vaults.h"

#include "core/log.h"

#include <algorithm>
#include <fstream>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixmounts.h>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  // The gocryptfs process of this user that serves `mount` (its last argument).
  pid_t fuseDaemon(const fs::path& mount) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
      const std::string pid = entry.path().filename().string();
      struct stat st {};
      if (pid.find_first_not_of("0123456789") != std::string::npos || ::stat(entry.path().c_str(), &st) != 0 ||
          st.st_uid != ::getuid()) {
        continue;
      }
      std::ifstream in(entry.path() / "cmdline", std::ios::binary);
      std::vector<std::string> argv;
      for (std::string arg; std::getline(in, arg, '\0');) {
        argv.push_back(std::move(arg));
      }
      if (argv.size() >= 2 && fs::path(argv.front()).filename() == "gocryptfs" && fs::path(argv.back()) == mount) {
        return static_cast<pid_t>(std::stol(pid));
      }
    }
    return -1;
  }

  int fusermount(const fs::path& mount, bool lazy) {
    std::vector<const char*> argv{"fusermount3", "-u"};
    if (lazy) {
      argv.push_back("-z");
    }
    argv.push_back(mount.c_str());
    argv.push_back(nullptr);
    gint status = -1;
    g_spawn_sync(nullptr, const_cast<gchar**>(argv.data()), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                 nullptr, &status, nullptr);
    return status;
  }

  constexpr Logger kLog("files-vault");
  constexpr const char* kIdle = "10m";
  constexpr int kWrongPassword = 12; // gocryptfs exit code

  // Runs argv with input on stdin; done(exit status, stderr). GLib keeps the call alive until it ends.
  void run(std::vector<std::string> argv, std::string input, std::function<void(int, std::string)> done) {
    std::vector<const char*> args;
    for (const std::string& a : argv) {
      args.push_back(a.c_str());
    }
    args.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* proc = g_subprocess_newv(args.data(),
                                          static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                                                        G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                                        G_SUBPROCESS_FLAGS_STDERR_PIPE),
                                          &error);
    if (proc == nullptr) {
      std::string message = error != nullptr ? error->message : "could not start";
      g_clear_error(&error);
      done(-1, std::move(message));
      return;
    }
    auto* cb = new std::function<void(int, std::string)>(std::move(done));
    g_subprocess_communicate_utf8_async(
        proc, input.c_str(), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::function<void(int, std::string)>> fn(
              static_cast<std::function<void(int, std::string)>*>(data));
          auto* p = G_SUBPROCESS(source);
          gchar* err = nullptr;
          g_subprocess_communicate_utf8_finish(p, result, nullptr, &err, nullptr);
          const int status = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
          std::string text = err != nullptr ? err : "";
          g_free(err);
          g_object_unref(p);
          (*fn)(status, std::move(text));
        },
        cb);
  }

  std::string firstLine(const std::string& text) {
    const auto nl = text.find('\n');
    return text.substr(0, nl);
  }

  bool mounted(const fs::path& mount) {
    GUnixMountEntry* entry = g_unix_mount_entry_at(mount.c_str(), nullptr);
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
    if (!g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr,
                       static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL), nullptr, nullptr, nullptr, &error)) {
      kLog.warn("vault guard did not start: {}", error->message);
      g_clear_error(&error);
    }
  }
} // namespace

Vaults::Vaults(std::string file) : m_file(std::move(file)) {
  std::ifstream in(storePath());
  const nlohmann::json list = nlohmann::json::parse(in, nullptr, false);
  if (!list.is_array()) {
    return;
  }
  for (const auto& v : list) {
    if (v.is_object() && v.value("name", "") != "") {
      m_vaults.push_back({.name = v.value("name", ""), .cipher = v.value("cipher", ""), .mount = v.value("mount", "")});
    }
  }
}

fs::path Vaults::storePath() const { return fs::path(g_get_user_data_dir()) / "andromeda" / "files" / m_file; }

void Vaults::save() const {
  nlohmann::json list = nlohmann::json::array();
  for (const Vault& v : m_vaults) {
    list.push_back({{"name", v.name}, {"cipher", v.cipher.string()}, {"mount", v.mount.string()}});
  }
  const fs::path file = storePath();
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::ofstream(file) << list.dump(2);
}

const Vaults::Vault* Vaults::find(const std::string& name) const {
  const auto it = std::ranges::find(m_vaults, name, &Vault::name);
  return it != m_vaults.end() ? &*it : nullptr;
}

bool Vaults::unlocked(const Vault& vault) { return mounted(vault.mount); }

bool Vaults::available() {
  gchar* path = g_find_program_in_path("gocryptfs");
  const bool found = path != nullptr;
  g_free(path);
  return found;
}

std::string Vaults::nameError(const std::string& name) const {
  if (name.empty()) {
    return "Give the vault a name.";
  }
  if (name.find('/') != std::string::npos || name == "." || name == "..") {
    return "A name cannot contain “/”.";
  }
  if (find(name) != nullptr) {
    return "A vault with this name exists.";
  }
  return {};
}

void Vaults::create(const std::string& name, const std::string& password, Done done) {
  if (std::string error = nameError(name); !error.empty()) {
    done(error);
    return;
  }
  Vault vault{.name = name,
              .cipher = fs::path(g_get_user_data_dir()) / "andromeda" / "vaults" / name,
              .mount = fs::path(g_get_home_dir()) / "Vaults" / name};
  std::error_code ec;
  fs::create_directories(vault.cipher, ec);
  if (ec || !fs::is_empty(vault.cipher, ec)) {
    done("The vault folder could not be made.");
    return;
  }
  run({"gocryptfs", "-init", "-q", vault.cipher.string()}, password + "\n",
      [this, vault, password, done = std::move(done)](int status, const std::string& err) {
        if (status != 0) {
          kLog.warn("vault init {} failed: {}", vault.name, err);
          std::error_code e;
          fs::remove_all(vault.cipher, e);
          done(err.empty() ? std::string("The vault could not be made.") : firstLine(err));
          return;
        }
        kLog.debug("vault created {}", vault.name);
        m_vaults.push_back(vault);
        save();
        unlock(vault, password, done);
      });
}

void Vaults::unlock(const Vault& vault, const std::string& password, Done done) {
  if (mounted(vault.mount)) {
    done({});
    return;
  }
  std::error_code ec;
  fs::create_directories(vault.mount, ec);
  // gocryptfs forks into the background once the mount is up, so the exit status is the unlock result.
  run({"gocryptfs", "-q", "-idle", kIdle, vault.cipher.string(), vault.mount.string()}, password + "\n",
      [vault, done = std::move(done)](int status, const std::string& err) {
        if (status == 0) {
          kLog.debug("vault unlocked {}", vault.name);
          startGuard(vault.mount);
          done({});
        } else if (status == kWrongPassword) {
          kLog.debug("vault wrong password {}", vault.name);
          done("The password is not correct.");
        } else {
          kLog.warn("vault unlock {} failed ({}): {}", vault.name, status, err);
          done(err.empty() ? std::string("The vault could not be opened.") : firstLine(err));
        }
      });
}

void Vaults::lock(const Vault& vault, Done done) {
  run({"fusermount3", "-u", vault.mount.string()}, {}, [vault, done = std::move(done)](int status, const std::string& err) {
    if (status == 0) {
      kLog.debug("vault locked {}", vault.name);
      done({});
    } else {
      kLog.warn("vault lock {} failed: {}", vault.name, err);
      done(err.find("busy") != std::string::npos ? std::string("An app still uses files in the vault.")
                                                 : firstLine(err));
    }
  });
}

int Vaults::guard(const fs::path& mount) {
  // logind waits for a delay inhibitor to close before sleep, so the vault locks while the system is still up.
  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (bus == nullptr) {
    kLog.warn("vault guard: no system bus: {}", error->message);
    g_clear_error(&error);
    return 1;
  }
  struct State {
    fs::path mount;
    GMainLoop* loop;
    GDBusConnection* bus;
    int inhibit = -1;
  } state{.mount = mount, .loop = g_main_loop_new(nullptr, FALSE), .bus = bus};
  const auto takeInhibitor = [](State& st) {
    GUnixFDList* fds = nullptr;
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
        st.bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
        g_variant_new("(ssss)", "sleep", "Files", "Lock vaults", "delay"), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, nullptr);
    if (reply != nullptr && fds != nullptr) {
      st.inhibit = g_unix_fd_list_get(fds, 0, nullptr);
    }
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    g_clear_object(&fds);
  };
  takeInhibitor(state);
  const guint sub = g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager", "PrepareForSleep", "/org/freedesktop/login1",
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer data) {
        auto& st = *static_cast<State*>(data);
        gboolean start = FALSE;
        g_variant_get(params, "(b)", &start);
        if (!start) {
          return;
        }
        // A busy vault: a lazy unmount alone keeps open files readable, so the decrypting process goes first and
        // every open handle fails from then on.
        if (fusermount(st.mount, false) != 0) {
          if (const pid_t daemon = fuseDaemon(st.mount); daemon > 0) {
            ::kill(daemon, SIGKILL);
          }
          fusermount(st.mount, true);
        }
        kLog.info("vault guard: locked {} before sleep", st.mount.string());
        if (st.inhibit >= 0) {
          ::close(st.inhibit);
          st.inhibit = -1;
        }
        g_main_loop_quit(st.loop);
      },
      &state, nullptr);
  // Locked some other way (Files, idle timeout, fusermount): nothing left to guard.
  GUnixMountMonitor* monitor = g_unix_mount_monitor_get();
  g_signal_connect(monitor, "mounts-changed", G_CALLBACK(+[](GUnixMountMonitor*, gpointer data) {
                     auto& st = *static_cast<State*>(data);
                     if (!mounted(st.mount)) {
                       g_main_loop_quit(st.loop);
                     }
                   }),
                   &state);
  kLog.info("vault guard: watching {}", mount.string());
  if (mounted(mount)) {
    g_main_loop_run(state.loop);
  }
  g_dbus_connection_signal_unsubscribe(bus, sub);
  g_object_unref(monitor);
  if (state.inhibit >= 0) {
    ::close(state.inhibit);
  }
  g_main_loop_unref(state.loop);
  g_object_unref(bus);
  return 0;
}

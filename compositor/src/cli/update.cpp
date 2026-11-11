#include "cli/update.h"

#include <cstdio>
#include <cstdlib>
#include <print>

#if ANDROMEDA_HAS_SD_BUS
#include <systemd/sd-bus.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <csignal>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#endif

namespace andromeda {

#if ANDROMEDA_HAS_SD_BUS
  namespace {
    constexpr const char* kUpdateName = "org.andromeda.Update1";
    constexpr const char* kUpdatePath = "/org/andromeda/Update1";
    constexpr const char* kHelperName = "org.andromeda.UpdateHelper1";
    constexpr const char* kHelperPath = "/org/andromeda/UpdateHelper1";
    // Each source of a check has its own 180 s bound in the service.
    constexpr auto kCheckTimeout = std::chrono::minutes(15);
    // The Upgrade reply waits for the polkit prompt.
    constexpr auto kAuthTimeout = std::chrono::minutes(5);
    // The groups UpdateHelper1.Upgrade installs; apps (Flatpak), firmware and held packages it does not.
    constexpr std::array<std::string_view, 3> kHelperGroups{"andromeda", "system", "snaps"};

    struct Bus {
      sd_bus* bus = nullptr;
      Bus() = default;
      Bus(const Bus&) = delete;
      Bus& operator=(const Bus&) = delete;
      ~Bus() { sd_bus_flush_close_unref(bus); }
    };

    struct Error {
      sd_bus_error error = SD_BUS_ERROR_NULL;
      Error() = default;
      Error(const Error&) = delete;
      Error& operator=(const Error&) = delete;
      ~Error() { sd_bus_error_free(&error); }
      [[nodiscard]] std::string text(int r) const {
        if (error.message != nullptr) {
          return error.message;
        }
        return std::string(std::strerror(-r));
      }
    };

    struct Message {
      sd_bus_message* m = nullptr;
      Message() = default;
      Message(const Message&) = delete;
      Message& operator=(const Message&) = delete;
      ~Message() { sd_bus_message_unref(m); }
    };

    struct Row {
      std::string group;
      std::string name;
      std::string current;
      std::string next;
    };

    // sd_bus_open_user reads DBUS_SESSION_BUS_ADDRESS with secure_getenv, which returns nothing in a secure-exec
    // process (the packaged cap_sys_nice binary); main() restored it into environ for getenv.
    int openUserBus(sd_bus** bus) {
      const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
      if (getauxval(AT_SECURE) == 0 || address == nullptr) {
        return sd_bus_open_user(bus);
      }
      sd_bus* opened = nullptr;
      int r = sd_bus_new(&opened);
      if (r >= 0) {
        r = sd_bus_set_address(opened, address);
      }
      if (r >= 0) {
        r = sd_bus_set_bus_client(opened, 1);
      }
      if (r >= 0) {
        r = sd_bus_start(opened);
      }
      if (r < 0) {
        sd_bus_unref(opened);
        return r;
      }
      *bus = opened;
      return 0;
    }

    // Runs `bus` until `done()` or the deadline; false on timeout or a broken connection.
    template <typename Done>
    bool runUntil(sd_bus* bus, std::chrono::steady_clock::time_point deadline, Done done) {
      while (!done()) {
        const int r = sd_bus_process(bus, nullptr);
        if (r < 0) {
          return false;
        }
        if (r > 0) {
          continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        if (sd_bus_wait(bus, static_cast<std::uint64_t>(left.count())) < 0) {
          return false;
        }
      }
      return true;
    }

    int fail(std::string_view message) {
      std::println(stderr, "andromeda update: {}", message);
      return EXIT_FAILURE;
    }

    bool checking(sd_bus* bus, std::string& error) {
      Error e;
      int value = 0;
      const int r = sd_bus_get_property_trivial(bus, kUpdateName, kUpdatePath, kUpdateName, "Checking", &e.error, 'b', &value);
      if (r < 0) {
        error = e.text(r);
        return false;
      }
      return value != 0;
    }

    // Check, then wait for `Checking` to turn false. Returns the rows, or an error text.
    // `sourceErrors`: one line per source that failed; the other sources still report rows.
    bool check(sd_bus* bus, std::vector<Row>& rows, std::vector<std::string>& sourceErrors, std::string& error) {
      bool changed = false;
      sd_bus_slot* slot = nullptr;
      int r = sd_bus_match_signal(
          bus, &slot, kUpdateName, kUpdatePath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
          [](sd_bus_message*, void* data, sd_bus_error*) {
            *static_cast<bool*>(data) = true;
            return 0;
          },
          &changed
      );
      if (r < 0) {
        error = std::strerror(-r);
        return false;
      }
      {
        Error e;
        r = sd_bus_call_method(bus, kUpdateName, kUpdatePath, kUpdateName, "Check", &e.error, nullptr, "");
        if (r < 0) {
          sd_bus_slot_unref(slot);
          error = e.text(r);
          return false;
        }
      }
      bool busy = checking(bus, error);
      const bool finished = error.empty() && runUntil(bus, std::chrono::steady_clock::now() + kCheckTimeout, [&] {
        if (busy && changed) {
          changed = false;
          busy = checking(bus, error);
        }
        return !busy || !error.empty();
      });
      sd_bus_slot_unref(slot);
      if (!error.empty()) {
        return false;
      }
      if (!finished) {
        error = "the check did not finish";
        return false;
      }

      Error e;
      Message reply;
      r = sd_bus_get_property(bus, kUpdateName, kUpdatePath, kUpdateName, "Updates", &e.error, &reply.m, "a(sssst)");
      if (r >= 0) {
        r = sd_bus_message_enter_container(reply.m, 'a', "(sssst)");
      }
      while (r >= 0) {
        const char* group = nullptr;
        const char* name = nullptr;
        const char* current = nullptr;
        const char* next = nullptr;
        std::uint64_t bytes = 0;
        r = sd_bus_message_read(reply.m, "(sssst)", &group, &name, &current, &next, &bytes);
        if (r <= 0) {
          break;
        }
        rows.push_back({group, name, current, next});
      }
      if (r < 0) {
        error = e.text(r);
        return false;
      }

      Error errorsError;
      char** errors = nullptr;
      r = sd_bus_get_property_strv(bus, kUpdateName, kUpdatePath, kUpdateName, "Errors", &errorsError.error, &errors);
      if (r < 0) {
        error = "cannot read the check errors: " + errorsError.text(r);
        return false;
      }
      if (errors != nullptr) {
        for (char** line = errors; *line != nullptr; ++line) {
          sourceErrors.emplace_back(*line);
          std::free(*line);
        }
        std::free(errors);
      }
      return true;
    }

    struct Install {
      bool finished = false;
      bool ok = false;
      bool helperGone = false;
      // The unique name that answered Upgrade; an earlier helper's idle exit names another owner.
      std::string helperOwner;
      std::string message;
      std::vector<std::string> questions;
      std::string lastStage;
      std::string lastPackage;
    };

    int onProgress(sd_bus_message* m, void* data, sd_bus_error*) {
      auto* install = static_cast<Install*>(data);
      const char* stage = nullptr;
      int percent = 0;
      const char* package = nullptr;
      if (sd_bus_message_read(m, "sis", &stage, &percent, &package) < 0) {
        return 0;
      }
      if (install->lastStage == stage && install->lastPackage == package) {
        return 0;
      }
      install->lastStage = stage;
      install->lastPackage = package;
      if (*package != '\0') {
        std::println("{} {} {}%", stage, package, std::clamp(percent, 0, 100));
      } else {
        std::println("{} {}%", stage, std::clamp(percent, 0, 100));
      }
      std::fflush(stdout);
      return 0;
    }

    int onFinished(sd_bus_message* m, void* data, sd_bus_error*) {
      auto* install = static_cast<Install*>(data);
      int ok = 0;
      const char* message = nullptr;
      if (sd_bus_message_read(m, "bs", &ok, &message) < 0) {
        return 0;
      }
      install->ok = ok != 0;
      install->message = message;
      if (sd_bus_message_skip(m, "as") >= 0 && sd_bus_message_enter_container(m, 'a', "s") >= 0) {
        const char* question = nullptr;
        while (sd_bus_message_read(m, "s", &question) > 0) {
          install->questions.emplace_back(question);
        }
      }
      install->finished = true;
      return 0;
    }

    int onOwnerChanged(sd_bus_message* m, void* data, sd_bus_error*) {
      const char* name = nullptr;
      const char* oldOwner = nullptr;
      const char* newOwner = nullptr;
      auto* install = static_cast<Install*>(data);
      if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) >= 0 && *newOwner == '\0'
          && install->helperOwner == oldOwner) {
        install->helperGone = true;
      }
      return 0;
    }

    // A polkit text agent on the terminal for the Upgrade prompt, where no shell runs one (tty, ssh). It registers
    // for this connection's bus name, the subject the helper checks; --fallback leaves an existing agent in charge.
    class TtyAgent {
    public:
      explicit TtyAgent(sd_bus* bus) {
        const char* unique = nullptr;
        if (bus == nullptr || sd_bus_get_unique_name(bus, &unique) < 0) {
          return;
        }
        int ready[2];
        if (pipe2(ready, O_CLOEXEC) < 0) {
          return;
        }
        constexpr int kNotifyFd = 3;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, ready[1], kNotifyFd);
        std::string name = std::string("--system-bus-name=") + unique;
        std::string notify = "--notify-fd=" + std::to_string(kNotifyFd);
        std::array<char*, 5> argv{
            const_cast<char*>("pkttyagent"), name.data(), const_cast<char*>("--fallback"), notify.data(), nullptr
        };
        if (posix_spawnp(&m_pid, "pkttyagent", &actions, nullptr, argv.data(), environ) != 0) {
          m_pid = 0;
        }
        posix_spawn_file_actions_destroy(&actions);
        close(ready[1]);
        if (m_pid > 0) {
          // EOF: the agent registered and closed the fd, or it exited.
          pollfd fd{.fd = ready[0], .events = POLLIN, .revents = 0};
          char byte = 0;
          while (poll(&fd, 1, kReadyTimeoutMs) > 0 && read(ready[0], &byte, 1) > 0) {
          }
        }
        close(ready[0]);
      }
      TtyAgent(const TtyAgent&) = delete;
      TtyAgent& operator=(const TtyAgent&) = delete;
      ~TtyAgent() {
        if (m_pid <= 0) {
          return;
        }
        kill(m_pid, SIGTERM);
        while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      }

    private:
      static constexpr int kReadyTimeoutMs = 5000;
      pid_t m_pid = 0;
    };

    int install() {
      Bus system;
      int r = sd_bus_open_system(&system.bus);
      if (r < 0) {
        return fail(std::string("cannot connect to the system bus: ") + std::strerror(-r));
      }
      sd_bus_set_allow_interactive_authorization(system.bus, 1);
      Install state;
      // Matches go in before the call: Progress can follow the reply at once.
      if (sd_bus_match_signal(system.bus, nullptr, kHelperName, kHelperPath, kHelperName, "Progress", onProgress, &state) < 0
          || sd_bus_match_signal(system.bus, nullptr, kHelperName, kHelperPath, kHelperName, "Finished", onFinished, &state) < 0
          || sd_bus_add_match(
                 system.bus, nullptr,
                 "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.andromeda.UpdateHelper1'",
                 onOwnerChanged, &state
             ) < 0) {
        return fail("cannot watch the update service");
      }
      Message call;
      r = sd_bus_message_new_method_call(system.bus, &call.m, kHelperName, kHelperPath, kHelperName, "Upgrade");
      if (r >= 0) {
        r = sd_bus_message_append(call.m, "b", 0);
      }
      Error e;
      Message reply;
      const bool terminal = isatty(STDIN_FILENO) == 1;
      if (r >= 0) {
        const TtyAgent agent(terminal ? system.bus : nullptr);
        const auto timeout = std::chrono::duration_cast<std::chrono::microseconds>(kAuthTimeout);
        r = sd_bus_call(system.bus, call.m, static_cast<std::uint64_t>(timeout.count()), &e.error, &reply.m);
      }
      if (r < 0) {
        if (sd_bus_error_has_name(&e.error, SD_BUS_ERROR_ACCESS_DENIED)) {
          return fail(
              "authorization refused: " + e.text(r)
              + (terminal ? "" : " (with no polkit agent, run from a terminal or a desktop session)")
          );
        }
        return fail(e.text(r));
      }
      if (const char* sender = sd_bus_message_get_sender(reply.m); sender != nullptr) {
        state.helperOwner = sender;
      }
      // A transaction has no upper bound (downloads); only the helper's exit ends the wait without Finished.
      runUntil(system.bus, std::chrono::steady_clock::time_point::max(), [&] {
        return state.finished || state.helperGone;
      });
      if (!state.finished) {
        return fail("the update service stopped");
      }
      if (!state.ok) {
        std::println(stderr, "andromeda update: {}", state.message);
        if (!state.questions.empty()) {
          std::println(stderr, "The update needs your answer; open Settings > Software Update:");
          for (const auto& question : state.questions) {
            std::println(stderr, "  {}", question);
          }
        }
        return EXIT_FAILURE;
      }
      std::println("{}", state.message);
      return EXIT_SUCCESS;
    }

    // The Flatpak update the Software Update page runs (SoftwareUpdateService::upgradeApps): user scope, no root.
    // Each output line prints as "apps <line>"; on failure the last line is the error, as on the page.
    int updateApps() {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC) < 0) {
        return fail(std::string("flatpak could not start: ") + std::strerror(errno));
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
      std::array<char*, 5> argv{
          const_cast<char*>("flatpak"), const_cast<char*>("update"), const_cast<char*>("--noninteractive"),
          const_cast<char*>("-y"), nullptr
      };
      pid_t pid = 0;
      const int spawned = posix_spawnp(&pid, "flatpak", &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      close(fds[1]);
      if (spawned != 0) {
        close(fds[0]);
        return fail("flatpak could not start");
      }
      std::string pending;
      std::string last;
      const auto emit = [&last](std::string_view line) {
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }
        // Progress redraws with '\r'; keep the newest state of the line.
        if (const auto cr = line.rfind('\r'); cr != std::string_view::npos) {
          line.remove_prefix(cr + 1);
        }
        if (line.empty()) {
          return;
        }
        last = line;
        std::println("apps {}", line);
        std::fflush(stdout);
      };
      std::array<char, 4096> buffer{};
      for (;;) {
        const ssize_t n = read(fds[0], buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(n));
        for (std::size_t nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n')) {
          emit(std::string_view(pending).substr(0, nl));
          pending.erase(0, nl + 1);
        }
      }
      emit(pending);
      close(fds[0]);
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return EXIT_SUCCESS;
      }
      return fail(last.empty() ? std::string("flatpak failed") : "flatpak failed: " + last);
    }
  } // namespace

  int runUpdate() {
    Bus session;
    if (const int r = openUserBus(&session.bus); r < 0) {
      return fail(std::string("cannot connect to the session bus: ") + std::strerror(-r));
    }
    std::println("Checking for updates…");
    std::fflush(stdout);
    std::vector<Row> rows;
    std::vector<std::string> sourceErrors;
    if (std::string error; !check(session.bus, rows, sourceErrors, error)) {
      return fail("check failed: " + error);
    }
    // A failed source exits 1, but what the other sources found still installs, as on the Settings page.
    for (const auto& line : sourceErrors) {
      std::println(stderr, "andromeda update: check failed: {}", line);
    }
    const int checkResult = sourceErrors.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    if (rows.empty()) {
      if (checkResult == EXIT_SUCCESS) {
        std::println("System is up to date");
      }
      return checkResult;
    }
    bool installable = false;
    bool apps = false;
    bool other = false;
    for (const auto& row : rows) {
      const bool ours = std::ranges::find(kHelperGroups, row.group) != kHelperGroups.end();
      installable = installable || ours;
      apps = apps || row.group == "apps";
      other = other || (!ours && row.group != "apps");
      std::println("  {} {} -> {}{}", row.name, row.current, row.next, ours ? "" : std::format(" ({})", row.group));
    }
    std::fflush(stdout);
    // Settings runs both regardless of the other's result.
    const int system = installable ? install() : EXIT_SUCCESS;
    const int appsResult = apps ? updateApps() : EXIT_SUCCESS;
    if (other) {
      std::println("Firmware and held packages update from Settings > Software Update.");
    }
    return checkResult == EXIT_SUCCESS && system == EXIT_SUCCESS && appsResult == EXIT_SUCCESS ? EXIT_SUCCESS
                                                                                                : EXIT_FAILURE;
  }
#else
  int runUpdate() {
    std::println(stderr, "andromeda update: this build has no D-Bus support (libsystemd)");
    return EXIT_FAILURE;
  }
#endif

} // namespace andromeda

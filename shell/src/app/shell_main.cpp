#include "app/application.h"
#include "app/single_instance_lock.h"
#include "auth/pam_authenticator.h"
#include "app/lock_app.h"
#include "shell/lockscreen/lock_helper.h"
#include "cli/completions.h"
#include "cli/help.h"
#include "cli/schema_root.h"
#include "compositors/compositor_detect.h"
#include "config/cli.h"
#include "config/legacy_home_move.h"
#include "core/build_info.h"
#include "core/log.h"
#include "core/process/process_fds.h"
#include "ipc/cli.h"
#include "launcher/dmenu_cli.h"
#include "scripting/plugin_lint.h"
#include "theme/dev_tokens.h"

#include <librsvg/rsvg.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <sched.h>
#include <string>
#include <string_view>
#include <unistd.h>


namespace {

  // The shell draws the bar and panels; under a 32-job build its main thread waited 0.47 ms per wake-up even at nice
  // -12. SCHED_RR priority 1 (one below the compositor) removes that wait. RESET_ON_FORK keeps worker threads and
  // launched apps at normal priority.
  void raiseMainThreadPriority() {
    sched_param param{};
    param.sched_priority = 1;
    if (sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &param) != 0) {
      logWarn("real-time scheduling unavailable ({}); the shell may stall under CPU load", std::strerror(errno));
    }
  }

  // librsvg filters run on rayon's global pool, which otherwise keeps one idle thread per CPU for the process lifetime.
  // Rayon reads RAYON_NUM_THREADS once, at the first filter render, so one 1 px render here sizes the pool and the
  // variable is removed again before any app inherits it. Must run after the daemon fork: threads do not survive it.
  void capRsvgThreadPool() {
    if (std::getenv("RAYON_NUM_THREADS") != nullptr) {
      return;
    }
    setenv("RAYON_NUM_THREADS", "1", 1);
    constexpr std::string_view kSvg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1">)"
                                      R"(<filter id="f"><feGaussianBlur stdDeviation="1"/></filter>)"
                                      R"svg(<rect width="1" height="1" filter="url(#f)"/></svg>)svg";
    if (RsvgHandle* handle =
            rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(kSvg.data()), kSvg.size(), nullptr);
        handle != nullptr) {
      // A 1x1 target skips the parallel blur path and leaves the pool unstarted.
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
      cairo_t* cr = cairo_create(surface);
      const RsvgRectangle viewport{0, 0, 2, 2};
      (void)rsvg_handle_render_document(handle, cr, &viewport, nullptr);
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
      g_object_unref(handle);
    }
    unsetenv("RAYON_NUM_THREADS");
  }

  enum class SpawnResult { Parent, Error };

  constexpr const char* kDaemonPipeEnv = "ANDROMEDA_DAEMON_PIPE_FD";
  int g_daemonPipe = -1;

  void closeFd(int& fd) {
    if (fd == -1) {
      return;
    }
    (void)::close(fd);
    fd = -1;
  }

  bool writeAll(int fd, const void* data, std::size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
      const ssize_t written = ::write(fd, bytes, size);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (written == 0) {
        return false;
      }
      bytes += written;
      size -= static_cast<std::size_t>(written);
    }
    return true;
  }

  bool readAll(int fd, void* data, std::size_t size) {
    char* bytes = static_cast<char*>(data);
    while (size > 0) {
      const ssize_t received = ::read(fd, bytes, size);
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (received == 0) {
        return false;
      }
      bytes += received;
      size -= static_cast<std::size_t>(received);
    }
    return true;
  }

  bool redirectStdioToNull() {
    int fd = ::open("/dev/null", O_RDWR);
    if (fd == -1) {
      std::perror("open(\"/dev/null\")");
      return false;
    }

    bool ok = true;
    if (::dup2(fd, STDIN_FILENO) == -1) {
      std::perror("dup2(stdin)");
      ok = false;
    }
    if (::dup2(fd, STDOUT_FILENO) == -1) {
      std::perror("dup2(stdout)");
      ok = false;
    }
    if (::dup2(fd, STDERR_FILENO) == -1) {
      std::perror("dup2(stderr)");
      ok = false;
    }

    if (fd > STDERR_FILENO) {
      (void)::close(fd);
    }
    return ok;
  }

  void completeDaemonStartup(int code) {
    if (g_daemonPipe == -1) {
      return;
    }

    const int result = code;
    if (!writeAll(g_daemonPipe, &result, sizeof(result))) {
      std::println(stderr, "error: failed to notify daemon parent: {}", std::strerror(errno));
    }
    closeFd(g_daemonPipe);
    if (code == 0 && !redirectStdioToNull()) {
      std::println(stderr, "error: failed to redirect daemon stdio");
    }
  }

  int runTopLevelFlag(const char* flag) {
    if (std::strcmp(flag, "--version") == 0 || std::strcmp(flag, "-v") == 0) {
      const std::string version = noctalia::build_info::displayVersion();
      std::println("noctalia {}", version);
      return 0;
    }
    if (std::strcmp(flag, "--help") == 0 || std::strcmp(flag, "-h") == 0) {
      std::print("{}", noctalia::cli::renderHelp(noctalia::cli::kRootCmd, "noctalia"));
      return 0;
    }
    return -1;
  }

  bool takeDaemonPipeFromEnv() {
    const char* value = std::getenv(kDaemonPipeEnv);
    if (value == nullptr || value[0] == '\0') {
      return false;
    }

    errno = 0;
    char* end = nullptr;
    const long fd = std::strtol(value, &end, 10);
    (void)::unsetenv(kDaemonPipeEnv);
    if (errno != 0 || end == value || *end != '\0' || fd < 0) {
      std::println(stderr, "error: invalid {} value: {}", kDaemonPipeEnv, value);
      return false;
    }

    g_daemonPipe = static_cast<int>(fd);
    return true;
  }

  SpawnResult daemonize(pid_t* outPid, int* parentPipe, char* const argv[]) {
    auto pipeFds = std::array<int, 2>{-1, -1};
    if (::pipe(pipeFds.data()) == -1) {
      std::perror("pipe");
      return SpawnResult::Error;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
      std::perror("fork");
      closeFd(pipeFds[0]);
      closeFd(pipeFds[1]);
      return SpawnResult::Error;
    }

    if (pid > 0) {
      if (outPid)
        *outPid = pid;
      if (parentPipe)
        *parentPipe = pipeFds[0];
      closeFd(pipeFds[1]);
      return SpawnResult::Parent;
    }

    closeFd(pipeFds[0]);
    // Match v4's early daemon boundary, but exec before shell startup so GLib,
    // D-Bus, and polkit start from a normal process image rather than a raw
    // post-fork child.
    if (::setsid() == -1) {
      std::perror("setsid");
      const int daemonResult = 1;
      (void)writeAll(pipeFds[1], &daemonResult, sizeof(daemonResult));
      closeFd(pipeFds[1]);
      _exit(1);
    }

    const std::string pipeFd = std::to_string(pipeFds[1]);
    if (::setenv(kDaemonPipeEnv, pipeFd.c_str(), 1) == -1) {
      std::perror("setenv");
      const int daemonResult = 1;
      (void)writeAll(pipeFds[1], &daemonResult, sizeof(daemonResult));
      closeFd(pipeFds[1]);
      _exit(1);
    }

    ::execvp(argv[0], argv);
    std::perror("execvp");
    const int daemonResult = 1;
    (void)writeAll(pipeFds[1], &daemonResult, sizeof(daemonResult));
    closeFd(pipeFds[1]);
    _exit(1);
  }

  int runShell() {
    if (compositors::detect() != compositors::CompositorKind::Andromeda) {
      logError(
          "noctalia runs only on the Andromeda compositor (ANDROMEDA_SOCKET is unset, desktop \"{}\")",
          compositors::envHint()
      );
      completeDaemonStartup(1);
      return 1;
    }

    // Raise the soft fd limit before any Wayland/EGL init. The NVIDIA EGL/Wayland
    // driver leaks internal sync_file fences slowly across a session; the default
    // 1024 soft cap can be exhausted in a long-running session, after which the
    // Wayland connection fails fatally.
    ProcessFds::raiseOpenFileLimit();

    // Claim the single-instance lock before any shell/Wayland init so the answer
    // is settled before bars or surfaces are created. Held for the process lifetime.
    SingleInstanceLock instanceLock;
    if (!instanceLock.tryAcquire()) {
      std::println(stderr, "error: noctalia is already running");
      completeDaemonStartup(1);
      _exit(1);
    }
    capRsvgThreadPool();
    raiseMainThreadPriority();
    try {
      noctalia::theme::dev_tokens::applyFromEnv();
      Application app;
      app.run([] { completeDaemonStartup(0); });
      if (Application::s_displayLost) {
        return Application::kDisplayLostExitCode;
      }
    } catch (const std::exception& e) {
      logError("fatal: {}", e.what());
      completeDaemonStartup(1);
      return 1;
    }
    return 0;
  }

} // namespace

// The shell behind the `noctalia` launcher (src/main.cpp), which has already set the allocator and the locale.
extern "C" __attribute__((visibility("default"))) int noctalia_shell_main_v1(int argc, char* argv[]) {
  // Undo the launcher's jemalloc preload in the environment: apps the shell starts must not inherit it. The check on
  // LD_PRELOAD itself keeps a stale inherited ANDROMEDA_JEMALLOC from deleting the user's own preload.
#ifdef NOCTALIA_JEMALLOC_PATH
  const char* preload = std::getenv("LD_PRELOAD");
  if ((std::getenv("ANDROMEDA_JEMALLOC") ? std::getenv("ANDROMEDA_JEMALLOC") : std::getenv("NOCTALIA_JEMALLOC")) != nullptr && preload != nullptr
      && std::string_view(preload).starts_with(NOCTALIA_JEMALLOC_PATH)) {
    const auto restore = [](const char* name, const char* saved) {
      if (const char* orig = std::getenv(saved); orig != nullptr) {
        setenv(name, orig, 1);
      } else {
        unsetenv(name);
      }
      unsetenv(saved);
    };
    restore("LD_PRELOAD", "ANDROMEDA_LD_PRELOAD_ORIG");
    restore("MALLOC_CONF", "ANDROMEDA_MALLOC_CONF_ORIG");
  }
#endif
  unsetenv("ANDROMEDA_JEMALLOC");

  const bool isDaemonChild = takeDaemonPipeFromEnv();
  bool shouldDaemonize = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--daemon") == 0
        || std::strcmp(argv[i], "--daemonize") == 0
        || std::strcmp(argv[i], "-d") == 0) {
      shouldDaemonize = true;
      for (int j = i; j < argc - 1; ++j) {
        argv[j] = argv[j + 1];
      }
      --argc;
      argv[argc] = nullptr;
      break;
    }
  }

  // The shell, the lock process and the config CLI read the config roots; `msg` and the helpers do not.
  if (argc < 2 || std::strcmp(argv[1], "lock") == 0 || std::strcmp(argv[1], "config") == 0) {
    noctalia::config::moveLegacyHomes();
  }

  if (argc >= 2) {
    if (std::strcmp(argv[1], "completions") == 0)
      return noctalia::cli::runCompletionsCli(argc, argv);
    if (std::strcmp(argv[1], "msg") == 0)
      return noctalia::ipc::runCli(argc, argv);
    // Internal mode for PAM authentication after re-exec.
    if (std::strcmp(argv[1], "pam-helper") == 0)
      return PamAuthenticator::runHelperMode(argc, argv);
    if (std::strcmp(argv[1], "lock-helper") == 0)
      return runLockHelper(argc, argv);
    if (std::strcmp(argv[1], "lock") == 0)
      return runLockMode(argc, argv);
    if (std::strcmp(argv[1], "config") == 0)
      return noctalia::config::runCli(argc, argv);
    if (std::strcmp(argv[1], "dmenu") == 0)
      return noctalia::launcher::runDmenuCli(argc, argv);
    if (std::strcmp(argv[1], "plugins") == 0)
      return noctalia::plugins::runCli(argc, argv);
  }

  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-') {
      const int rc = runTopLevelFlag(argv[i]);
      if (rc >= 0)
        return rc;

      std::println(stderr, "error: unknown option: {}", argv[i]);
      return 1;
    }
  }

  if (argc >= 2) {
    std::println(stderr, "error: unknown command: {}", argv[1]);
    std::println(stderr, "Run 'noctalia --help' for usage.");
    return 1;
  }

  {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0' && ::chdir(home) != 0) {
      std::println(stderr, "warning: failed to chdir to HOME ({}): {}", home, std::strerror(errno));
    }
  }

  if (shouldDaemonize && !isDaemonChild) {
    pid_t pid = -1;
    int parentPipe = -1;
    SpawnResult result = daemonize(&pid, &parentPipe, argv);

    if (result == SpawnResult::Error) {
      return 1;
    }
    if (result == SpawnResult::Parent) {
      int daemonResult = 1;
      const bool receivedResult = readAll(parentPipe, &daemonResult, sizeof(daemonResult));
      closeFd(parentPipe);
      if (!receivedResult) {
        std::println(stderr, "error: failed to wait for daemon startup");
        return 1;
      }
      if (daemonResult == 0) {
        std::println("noctalia started [pid: {}]", pid);
      }
      return daemonResult;
    }
  }

  return runShell();
}

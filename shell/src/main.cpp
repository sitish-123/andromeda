#include "greeter/greeter_host.h"

#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <print>
#include <string>
#include <system_error>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

#ifdef NOCTALIA_USE_JEMALLOC
  // The long-running shell runs on jemalloc (lower fragmentation over days). Greeter mode and CLI calls stay on glibc
  // malloc and save jemalloc's ~8 MB of fixed metadata. jemalloc must replace malloc before the first allocation, so a
  // shell run re-execs once with it preloaded; shell_main.cpp removes the preload so launched apps never inherit it.
  constexpr const char* kJemallocConf =
      "background_thread:true,narenas:2,dirty_decay_ms:1000,muzzy_decay_ms:5000,lg_tcache_max:12";

  bool isShellRun(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "-d") != 0 && std::strcmp(argv[i], "--daemon") != 0
          && std::strcmp(argv[i], "--daemonize") != 0) {
        return false;
      }
    }
    return true;
  }

  void execWithJemalloc(char* argv[]) {
    if (std::getenv("NOCTALIA_JEMALLOC") != nullptr || access(NOCTALIA_JEMALLOC_PATH, R_OK) != 0) {
      return;
    }
    std::string preload = NOCTALIA_JEMALLOC_PATH;
    if (const char* own = std::getenv("LD_PRELOAD"); own != nullptr && own[0] != '\0') {
      setenv("NOCTALIA_LD_PRELOAD_ORIG", own, 1);
      preload += ':';
      preload += own;
    }
    setenv("LD_PRELOAD", preload.c_str(), 1);
    // The user's own MALLOC_CONF keys come last, so they override single keys as with the old compiled-in malloc_conf.
    std::string conf = kJemallocConf;
    if (const char* own = std::getenv("MALLOC_CONF"); own != nullptr) {
      setenv("NOCTALIA_MALLOC_CONF_ORIG", own, 1);
      conf += ',';
      conf += own;
    }
    setenv("MALLOC_CONF", conf.c_str(), 1);
    setenv("NOCTALIA_JEMALLOC", "1", 1);
    // The real path, not /proc/self/exe: the kernel names the process after the file, and `pgrep noctalia` must work.
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    execv(ec ? "/proc/self/exe" : self.c_str(), argv);
    std::println(stderr, "warning: cannot re-exec with jemalloc ({}); running on glibc malloc", std::strerror(errno));
  }
#endif

  constexpr const char* kShellModule = "noctalia-shell.so";
  constexpr const char* kShellEntry = "noctalia_shell_main_v1";

  using ShellMain = int (*)(int, char*[]);

  // The shell and its library tree live in noctalia-shell.so, so greeter mode maps neither. The build directory copy
  // wins over the installed one, as for noctalia-calc.so.
  ShellMain loadShell() {
    std::error_code ec;
    const auto exeDir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    for (const auto& path :
         {exeDir / kShellModule,
          std::filesystem::path(NOCTALIA_INSTALL_PREFIX) / NOCTALIA_INSTALL_LIBDIR / "noctalia" / kShellModule}) {
      if (!std::filesystem::exists(path, ec)) {
        continue;
      }
      void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (module == nullptr) {
        std::println(stderr, "error: cannot load {}", dlerror());
        return nullptr;
      }
      void* symbol = dlsym(module, kShellEntry);
      if (symbol == nullptr) {
        std::println(stderr, "error: {} has no {}; it is from another noctalia version", path.string(), kShellEntry);
        return nullptr;
      }
      ShellMain entry = nullptr;
      std::memcpy(&entry, &symbol, sizeof(entry));
      return entry;
    }
    std::println(stderr, "error: {} is not installed next to noctalia or in the noctalia libdir", kShellModule);
    return nullptr;
  }

} // namespace

int main(int argc, char* argv[]) {
#ifdef NOCTALIA_USE_JEMALLOC
  if (isShellRun(argc, argv)) {
    execWithJemalloc(argv);
  }
#endif
#ifdef __GLIBC__
  mallopt(M_ARENA_MAX, 2);
#endif

  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  if (argc >= 2 && std::strcmp(argv[1], "greeter") == 0) {
    return runGreeterHost(argc, argv);
  }
  const ShellMain shell = loadShell();
  return shell != nullptr ? shell(argc, argv) : 1;
}

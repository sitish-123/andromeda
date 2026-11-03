#include "greeter/greeter_host.h"

#include <clocale>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <print>
#include <system_error>

#ifdef __GLIBC__
#ifndef NOCTALIA_USE_JEMALLOC
#include <malloc.h>
#endif
#endif

#ifdef NOCTALIA_USE_JEMALLOC
// jemalloc reads this before its first allocation; the background thread runs
// decay even while the shell is idle.
const char* malloc_conf = "background_thread:true,narenas:2,dirty_decay_ms:1000,muzzy_decay_ms:5000,lg_tcache_max:12";
#endif

namespace {

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
      void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (module == nullptr) {
        continue;
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
    std::println(stderr, "error: cannot load {}: {}", kShellModule, dlerror());
    return nullptr;
  }

} // namespace

int main(int argc, char* argv[]) {
#if defined(__GLIBC__) && !defined(NOCTALIA_USE_JEMALLOC)
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

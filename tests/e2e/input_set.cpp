// Drives InputControl against a live compositor: sets one input setting and waits for it to come back as the
// effective value. Prints one line per step and exits non-zero on the first mismatch.
#include "wayland/input_control.h"
#include "desktop-unstable-v1-client-protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <wayland-client.h>

namespace {

  struct Global {
    std::uint32_t name = 0;
    std::uint32_t version = 0;
  };

  void onGlobal(void* data, wl_registry*, std::uint32_t name, const char* interface, std::uint32_t version) {
    if (std::strcmp(interface, "dsk_input_manager_v1") == 0) {
      *static_cast<Global*>(data) = {name, version};
    }
  }
  void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
  const wl_registry_listener kRegistry = {.global = onGlobal, .global_remove = onGlobalRemove};

  wl_display* g_display = nullptr;
  int g_failures = 0;

  void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
  }

  template <typename Pred> bool dispatchUntil(Pred done) {
    for (int i = 0; i < 200 && !done(); ++i) {
      if (wl_display_roundtrip(g_display) < 0) {
        return false;
      }
    }
    return done();
  }

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::puts("usage: input_set <key> <value>");
    return 1;
  }
  const std::string key = argv[1];
  const std::string value = argv[2];

  g_display = wl_display_connect(nullptr);
  if (g_display == nullptr) {
    std::puts("FAIL connect");
    return 1;
  }
  Global global;
  wl_registry* registry = wl_display_get_registry(g_display);
  wl_registry_add_listener(registry, &kRegistry, &global);
  wl_display_roundtrip(g_display);
  check(global.name != 0, "compositor advertises dsk_input_manager_v1");
  if (global.name == 0) {
    return 1;
  }

  InputControl input(registry, global.name, global.version, {});
  check(dispatchUntil([&] { return input.ready(); }), "initial state received");

  input.set(key, value);
  check(dispatchUntil([&] { return input.value(key) == value || !input.lastFailure().empty(); }), "set " + key + "=" + value);
  check(input.lastFailure().empty(), "no failure (" + input.lastFailure() + ")");
  check(input.value(key) == value, "effective value is " + value);

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

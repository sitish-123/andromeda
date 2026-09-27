// Drives SettingsControl against a live compositor: sets one setting and waits for the effective value, which is the
// value itself unless a third argument names what should apply instead (clearing a key falls back to config.toml).
// Prints one line per step and exits non-zero on any mismatch.
#include "wayland/settings_control.h"
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
    if (std::strcmp(interface, "dsk_settings_manager_v1") == 0) {
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
  if (argc != 3 && argc != 4) {
    std::puts("usage: settings_set <key> <value> [expected]");
    return 1;
  }
  const std::string key = argv[1];
  const std::string value = argv[2];
  const std::string expected = argc == 4 ? argv[3] : value;

  g_display = wl_display_connect(nullptr);
  if (g_display == nullptr) {
    std::puts("FAIL connect");
    return 1;
  }
  Global global;
  wl_registry* registry = wl_display_get_registry(g_display);
  wl_registry_add_listener(registry, &kRegistry, &global);
  wl_display_roundtrip(g_display);
  check(global.name != 0, "compositor advertises dsk_settings_manager_v1");
  if (global.name == 0) {
    return 1;
  }

  SettingsControl settings(registry, global.name, global.version, {});
  check(dispatchUntil([&] { return settings.ready(); }), "initial state received");

  settings.set(key, value);
  check(
      dispatchUntil([&] { return settings.value(key) == expected || !settings.lastFailure().empty(); }),
      "set " + key + "=" + value
  );
  check(settings.lastFailure().empty(), "no failure (" + settings.lastFailure() + ")");
  check(settings.value(key) == expected, "effective value is " + expected);
  check(settings.customized(key) == !value.empty(), value.empty() ? "no longer customized" : "reported as customized");

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

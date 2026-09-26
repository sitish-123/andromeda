#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>
#include <wayland-client.h>

namespace {
  struct State {
    std::string_view wanted;
    bool found = false;
    uint32_t name = 0;
    uint32_t version = 0;
  };

  void handleGlobal(void* data, wl_registry*, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (state->wanted == interface) {
      state->found = true;
      state->name = name;
      state->version = version;
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };

  wl_registry* queryGlobal(wl_display* display, State& state) {
    state.found = false;
    state.name = 0;
    state.version = 0;
    wl_registry* registry = wl_display_get_registry(display);
    if (registry == nullptr) {
      return nullptr;
    }
    wl_registry_add_listener(registry, &kRegistryListener, &state);
    if (wl_display_roundtrip(display) < 0) {
      wl_registry_destroy(registry);
      return nullptr;
    }
    return registry;
  }

  int checkResult(const State& state, bool expected, bool checkVersion, uint32_t expectedVersion) {
    if (state.found != expected) {
      std::println(
          stderr, "global-client: {} was {}, expected {}", state.wanted, state.found ? "present" : "absent",
          expected ? "present" : "absent"
      );
      return 1;
    }
    if (state.found && checkVersion && state.version != expectedVersion) {
      std::println(
          stderr, "global-client: {} version was {}, expected {}", state.wanted, state.version, expectedVersion
      );
      return 1;
    }
    return 0;
  }
} // namespace

int main(int argc, char** argv) {
  const bool recheck = argc >= 2 && std::strcmp(argv[1], "recheck") == 0;
  const bool bind = argc >= 2 && std::strcmp(argv[1], "bind") == 0;
  const bool waitForSignal = recheck || bind;
  const int offset = waitForSignal ? 1 : 0;
  if ((argc != 3 + offset && argc != 4 + offset)
      || (std::strcmp(argv[2 + offset], "present") != 0 && std::strcmp(argv[2 + offset], "absent") != 0)) {
    std::println(stderr, "usage: global-client [recheck|bind] INTERFACE present|absent [VERSION]");
    return 2;
  }
  char* versionEnd = nullptr;
  const bool checkVersion = argc == 4 + offset;
  const char* versionArgument = checkVersion ? argv[3 + offset] : nullptr;
  const unsigned long expectedVersion = checkVersion ? std::strtoul(versionArgument, &versionEnd, 10) : 0;
  if (checkVersion && (*versionArgument == '\0' || *versionEnd != '\0' || expectedVersion > UINT32_MAX)) {
    std::println(stderr, "global-client: invalid version {}", versionArgument);
    return 2;
  }

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "global-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }

  State state{.wanted = argv[1 + offset]};
  wl_registry* registry = queryGlobal(display, state);
  if (registry == nullptr) {
    std::println(stderr, "global-client: registry roundtrip failed");
    wl_display_disconnect(display);
    return 2;
  }

  const bool expected = std::strcmp(argv[2 + offset], "present") == 0;
  if (const int result = checkResult(state, expected, checkVersion, expectedVersion); result != 0) {
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return result;
  }

  if (bind && (!state.found || state.wanted != zxdg_decoration_manager_v1_interface.name)) {
    std::println(stderr, "global-client: bind mode only supports a present zxdg_decoration_manager_v1");
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 2;
  }

  if (waitForSignal) {
    if (recheck) {
      wl_registry_destroy(registry);
      registry = nullptr;
    }
    std::println("ready");
    std::fflush(stdout);
    if (std::getchar() == EOF) {
      std::println(stderr, "global-client: control signal was not received");
      wl_display_disconnect(display);
      return 2;
    }
    if (recheck) {
      registry = queryGlobal(display, state);
      if (registry == nullptr) {
        std::println(stderr, "global-client: registry recheck roundtrip failed");
        wl_display_disconnect(display);
        return 2;
      }
      if (const int result = checkResult(state, expected, checkVersion, expectedVersion); result != 0) {
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return result;
      }
    } else {
      auto* manager = static_cast<zxdg_decoration_manager_v1*>(
          wl_registry_bind(registry, state.name, &zxdg_decoration_manager_v1_interface, 1)
      );
      if (manager == nullptr) {
        std::println(stderr, "global-client: decoration manager bind failed");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 2;
      }
      zxdg_decoration_manager_v1_destroy(manager);
      if (wl_display_roundtrip(display) < 0) {
        std::println(stderr, "global-client: decoration manager bind roundtrip failed");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
      }
    }
  }

  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return 0;
}

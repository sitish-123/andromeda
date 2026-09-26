// Maps an idle-inhibiting xdg toplevel and reports when the inhibited idle
// notification fires. The harness moves the window to an inactive workspace.

#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wmBase = nullptr;
    zwp_idle_inhibit_manager_v1* inhibitManager = nullptr;
    ext_idle_notifier_v1* idleNotifier = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zwp_idle_inhibitor_v1* inhibitor = nullptr;
    ext_idle_notification_v1* notification = nullptr;
    wl_buffer* buffer = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
    bool mapped = false;
  };

  void idleIdled(void*, ext_idle_notification_v1*) {
    std::println("idled");
    std::fflush(stdout);
  }
  void idleResumed(void*, ext_idle_notification_v1*) {}
  constexpr ext_idle_notification_v1_listener kIdleListener = {
      .idled = idleIdled,
      .resumed = idleResumed,
  };

  void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  void xdgConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (state.mapped) {
      return;
    }
    state.mapped = true;
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, 64, 64);
    wl_surface_commit(state.surface);
    std::println("mapped");
    std::fflush(stdout);
  }
  constexpr xdg_surface_listener kXdgListener = {.configure = xdgConfigure};

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
  void toplevelClose(void*, xdg_toplevel*) {}
  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (std::strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
      state.inhibitManager = static_cast<zwp_idle_inhibit_manager_v1*>(
          wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1)
      );
    } else if (std::strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
      state.idleNotifier = static_cast<ext_idle_notifier_v1*>(
          wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, std::min(version, 1U))
      );
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  bool createBuffer(State& state) {
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int stride = width * 4;
    state.size = stride * height;
    const int fd = memfd_create("umbriel-idle-inhibit-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(state.size)) < 0) {
      return false;
    }
    state.pixels = mmap(nullptr, state.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state.pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    std::fill_n(static_cast<uint32_t*>(state.pixels), state.size / sizeof(uint32_t), 0xFF5577AA);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(state.size));
    state.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return state.buffer != nullptr;
  }
} // namespace

int main() {
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "idle-inhibit-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.seat == nullptr
      || state.wmBase == nullptr
      || state.inhibitManager == nullptr
      || state.idleNotifier == nullptr
      || !createBuffer(state)) {
    std::println(stderr, "idle-inhibit-client: missing required global or buffer");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, "idle-inhibit-client");
  state.inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(state.inhibitManager, state.surface);
  state.notification = ext_idle_notifier_v1_get_idle_notification(state.idleNotifier, 150, state.seat);
  ext_idle_notification_v1_add_listener(state.notification, &kIdleListener, &state);
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }
  return EXIT_SUCCESS;
}

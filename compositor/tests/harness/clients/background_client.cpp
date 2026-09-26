// Maps an xdg toplevel that redraws on every wl_surface.frame callback, the way a game or a browser does, and prints
// "frame" for each callback plus "suspended" / "resumed" when the xdg_toplevel suspended state changes. With
// CONTENT_TYPE=game it declares the game content type before its first commit. Usage: background-client [title].

#include "content-type-v1-client-protocol.h"
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
  constexpr int kSize = 64;

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wp_content_type_manager_v1* contentTypeManager = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    bool mapped = false;
    bool suspended = false;
  };

  void requestFrame(State& state);

  void frameDone(void* data, wl_callback* callback, uint32_t) {
    wl_callback_destroy(callback);
    auto& state = *static_cast<State*>(data);
    std::println("frame");
    std::fflush(stdout);
    requestFrame(state);
  }
  constexpr wl_callback_listener kFrameListener = {.done = frameDone};

  void requestFrame(State& state) {
    wl_callback_add_listener(wl_surface_frame(state.surface), &kFrameListener, &state);
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, kSize, kSize);
    wl_surface_commit(state.surface);
  }

  void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  void xdgConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (state.mapped) {
      return;
    }
    state.mapped = true;
    requestFrame(state);
    std::println("mapped");
    std::fflush(stdout);
  }
  constexpr xdg_surface_listener kXdgListener = {.configure = xdgConfigure};

  void toplevelConfigure(void* data, xdg_toplevel*, int32_t, int32_t, wl_array* states) {
    auto& state = *static_cast<State*>(data);
    const auto* configured = static_cast<const uint32_t*>(states->data);
    const bool suspended =
        std::ranges::find(configured, configured + states->size / sizeof(uint32_t), XDG_TOPLEVEL_STATE_SUSPENDED)
        != configured + states->size / sizeof(uint32_t);
    if (suspended != state.suspended) {
      state.suspended = suspended;
      std::println("{}", suspended ? "suspended" : "resumed");
      std::fflush(stdout);
    }
  }
  void toplevelClose(void*, xdg_toplevel*) {}
  void toplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  void toplevelCapabilities(void*, xdg_toplevel*, wl_array*) {}
  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = toplevelBounds,
      .wm_capabilities = toplevelCapabilities,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 6U))
      );
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (std::strcmp(interface, wp_content_type_manager_v1_interface.name) == 0) {
      state.contentTypeManager = static_cast<wp_content_type_manager_v1*>(
          wl_registry_bind(registry, name, &wp_content_type_manager_v1_interface, 1)
      );
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  bool createBuffer(State& state) {
    constexpr int stride = kSize * 4;
    constexpr size_t size = stride * kSize;
    const int fd = memfd_create("umbriel-background-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return false;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), 0xFF55AA77);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    state.buffer = wl_shm_pool_create_buffer(pool, 0, kSize, kSize, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return state.buffer != nullptr;
  }
} // namespace

int main(int argc, char** argv) {
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "background-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr || !createBuffer(state)) {
    std::println(stderr, "background-client: missing required global or buffer");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  const char* contentType = std::getenv("CONTENT_TYPE");
  if (contentType != nullptr && std::strcmp(contentType, "game") == 0) {
    if (state.contentTypeManager == nullptr) {
      std::println(stderr, "background-client: no content-type manager");
      return EXIT_FAILURE;
    }
    wp_content_type_v1_set_content_type(
        wp_content_type_manager_v1_get_surface_content_type(state.contentTypeManager, state.surface),
        WP_CONTENT_TYPE_V1_TYPE_GAME
    );
  }
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, argc > 1 ? argv[1] : "background-client");
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }
  return EXIT_SUCCESS;
}

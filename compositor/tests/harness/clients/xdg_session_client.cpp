// Maps one xdg toplevel (app id "xdg-session-test") in an xdg-session-management-v1 session and prints its events:
// "created <id>", "session-restored", "toplevel-restored", "mapped". With a session id argument it restores that
// session and its toplevel "main"; without one it starts a new session and adds the toplevel as "main".
// --drop-handle destroys the xdg_toplevel_session_v1 once mapped and prints "handle-dropped". --empty only creates a
// session, prints "created <id>" and exits.

#include "xdg-session-management-v1-client-protocol.h"
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
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    xdg_session_manager_v1* sessionManager = nullptr;
    wl_surface* surface = nullptr;
    wl_buffer* buffer = nullptr;
    bool mapped = false;
    bool dropHandle = false;
    xdg_toplevel_session_v1* handle = nullptr;
  };

  void print(const char* line) {
    std::println("{}", line);
    std::fflush(stdout);
  }

  void sessionCreated(void*, xdg_session_v1*, const char* id) {
    std::println("created {}", id);
    std::fflush(stdout);
  }
  void sessionRestored(void*, xdg_session_v1*) { print("session-restored"); }
  void sessionReplaced(void*, xdg_session_v1*) { print("session-replaced"); }
  constexpr xdg_session_v1_listener kSessionListener = {
      .created = sessionCreated,
      .restored = sessionRestored,
      .replaced = sessionReplaced,
  };

  void toplevelRestored(void*, xdg_toplevel_session_v1*) { print("toplevel-restored"); }
  constexpr xdg_toplevel_session_v1_listener kToplevelSessionListener = {.restored = toplevelRestored};

  void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  void xdgConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (state.mapped) {
      wl_surface_commit(state.surface);
      return;
    }
    state.mapped = true;
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, 64, 64);
    wl_surface_commit(state.surface);
    print("mapped");
    if (state.dropHandle) {
      xdg_toplevel_session_v1_destroy(state.handle);
      state.handle = nullptr;
      print("handle-dropped");
    }
  }
  constexpr xdg_surface_listener kXdgListener = {.configure = xdgConfigure};

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
  void toplevelClose(void*, xdg_toplevel*) { std::exit(EXIT_SUCCESS); }
  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t /*version*/) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (std::strcmp(interface, xdg_session_manager_v1_interface.name) == 0) {
      state.sessionManager = static_cast<xdg_session_manager_v1*>(
          wl_registry_bind(registry, name, &xdg_session_manager_v1_interface, 1)
      );
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  bool createBuffer(State& state) {
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int stride = width * 4;
    constexpr size_t size = stride * height;
    const int fd = memfd_create("andromeda-xdg-session-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return false;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), 0xFF3366CC);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    state.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return state.buffer != nullptr;
  }
} // namespace

int main(int argc, char** argv) {
  State state;
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "xdg-session-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);
  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr || state.sessionManager == nullptr
      || !createBuffer(state)) {
    std::println(stderr, "xdg-session-client: missing required global or buffer");
    return EXIT_FAILURE;
  }

  const char* restoreId = nullptr;
  bool empty = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--drop-handle") == 0) {
      state.dropHandle = true;
    } else if (std::strcmp(argv[i], "--empty") == 0) {
      empty = true;
    } else {
      restoreId = argv[i];
    }
  }
  xdg_session_v1* session = xdg_session_manager_v1_get_session(
      state.sessionManager,
      restoreId != nullptr ? XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE : XDG_SESSION_MANAGER_V1_REASON_LAUNCH,
      restoreId
  );
  xdg_session_v1_add_listener(session, &kSessionListener, &state);
  if (empty) {
    wl_display_roundtrip(display);
    xdg_session_v1_destroy(session);
    wl_display_roundtrip(display);
    return EXIT_SUCCESS;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(xdgSurface, &kXdgListener, &state);
  xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
  xdg_toplevel_add_listener(toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_app_id(toplevel, "xdg-session-test");
  xdg_toplevel_set_title(toplevel, "xdg-session-client");
  xdg_toplevel_session_v1* toplevelSession = restoreId != nullptr
      ? xdg_session_v1_restore_toplevel(session, toplevel, "main")
      : xdg_session_v1_add_toplevel(session, toplevel, "main");
  xdg_toplevel_session_v1_add_listener(toplevelSession, &kToplevelSessionListener, &state);
  state.handle = toplevelSession;
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(display) >= 0) {
  }
  return EXIT_SUCCESS;
}

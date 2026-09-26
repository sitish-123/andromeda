// Maps a small layer surface and starts a Wayland data-device drag when its left mouse button is pressed. The headless
// harness drives that press through pointer-client, then observes whether the compositor terminates the drag.

#include <wayland-client.h>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

  constexpr int kSurfaceSize = 96;
  constexpr uint32_t kLeftButton = 0x110;

  struct Buffer {
    wl_buffer* resource = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_data_device_manager* dataDeviceManager = nullptr;
    wl_data_device* dataDevice = nullptr;
    zwlr_layer_shell_v1* layerShell = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    wl_surface* surface = nullptr;
    wl_surface* iconSurface = nullptr;
    wl_data_source* source = nullptr;
    Buffer windowBuffer;
    Buffer iconBuffer;
    bool ready = false;
    bool dragStarted = false;
    bool dragFinished = false;
    bool waitForPointerRefresh = false;
    bool pointerRefreshObserved = false;
    bool complete = false;
    bool failed = false;
  };

  Buffer createBuffer(State& state, int width, int height, uint32_t color) {
    Buffer buffer;
    const int stride = width * 4;
    buffer.size = static_cast<size_t>(stride * height);
    const int fd = memfd_create("umbriel-drag-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(buffer.size)) < 0) {
      if (fd >= 0) {
        close(fd);
      }
      return buffer;
    }

    buffer.pixels = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer.pixels == MAP_FAILED) {
      close(fd);
      return buffer;
    }
    std::fill_n(static_cast<uint32_t*>(buffer.pixels), buffer.size / sizeof(uint32_t), color);

    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(buffer.size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  void finishDrag(State& state, std::string_view result) {
    if (state.dragFinished) {
      return;
    }
    state.dragFinished = true;
    std::println("{}", result);
    std::fflush(stdout);

    if (!state.waitForPointerRefresh) {
      state.complete = true;
    }
  }

  void sourceTarget(void*, wl_data_source*, const char*) {}

  void sourceSend(void*, wl_data_source*, const char*, int32_t fd) {
    constexpr std::string_view payload = "umbriel drag regression";
    size_t offset = 0;
    while (offset < payload.size()) {
      const ssize_t size = write(fd, payload.data() + offset, payload.size() - offset);
      if (size > 0) {
        offset += static_cast<size_t>(size);
        continue;
      }
      if (size < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    close(fd);
  }

  void sourceCancelled(void* data, wl_data_source*) { finishDrag(*static_cast<State*>(data), "drag-cancelled"); }

  void sourceDropPerformed(void*, wl_data_source*) {}

  void sourceFinished(void* data, wl_data_source*) { finishDrag(*static_cast<State*>(data), "drag-finished"); }

  void sourceAction(void*, wl_data_source*, uint32_t) {}

  constexpr wl_data_source_listener kDataSourceListener = {
      .target = sourceTarget,
      .send = sourceSend,
      .cancelled = sourceCancelled,
      .dnd_drop_performed = sourceDropPerformed,
      .dnd_finished = sourceFinished,
      .action = sourceAction,
  };

  void observePointerRefresh(State& state) {
    if (!state.waitForPointerRefresh || !state.dragFinished || state.pointerRefreshObserved) {
      return;
    }
    state.pointerRefreshObserved = true;
    state.complete = true;
    std::println("pointer-refreshed");
    std::fflush(stdout);
  }

  void pointerEnter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    observePointerRefresh(*static_cast<State*>(data));
  }
  void pointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}
  void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {
    observePointerRefresh(*static_cast<State*>(data));
  }

  void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t buttonState) {
    auto& state = *static_cast<State*>(data);
    if (button != kLeftButton || buttonState != WL_POINTER_BUTTON_STATE_PRESSED || state.dragStarted) {
      return;
    }

    state.source = wl_data_device_manager_create_data_source(state.dataDeviceManager);
    wl_data_source_add_listener(state.source, &kDataSourceListener, &state);
    wl_data_source_offer(state.source, "text/plain");
    wl_data_source_set_actions(state.source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

    state.iconSurface = wl_compositor_create_surface(state.compositor);
    wl_data_device_start_drag(state.dataDevice, state.source, state.surface, state.iconSurface, serial);
    wl_surface_attach(state.iconSurface, state.iconBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.iconSurface, 0, 0, 24, 24);
    wl_surface_commit(state.iconSurface);

    state.dragStarted = true;
    std::println("drag-started");
    std::fflush(stdout);
  }

  void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
  void pointerFrame(void*, wl_pointer*) {}
  void pointerAxisSource(void*, wl_pointer*, uint32_t) {}
  void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
  void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
  void pointerAxisValue120(void*, wl_pointer*, uint32_t, int32_t) {}
  void pointerAxisRelativeDirection(void*, wl_pointer*, uint32_t, uint32_t) {}
#ifdef WL_POINTER_WARP_SINCE_VERSION
  void pointerWarp(void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {}
#endif

  constexpr wl_pointer_listener kPointerListener = {
      .enter = pointerEnter,
      .leave = pointerLeave,
      .motion = pointerMotion,
      .button = pointerButton,
      .axis = pointerAxis,
      .frame = pointerFrame,
      .axis_source = pointerAxisSource,
      .axis_stop = pointerAxisStop,
      .axis_discrete = pointerAxisDiscrete,
      .axis_value120 = pointerAxisValue120,
      .axis_relative_direction = pointerAxisRelativeDirection,
#ifdef WL_POINTER_WARP_SINCE_VERSION
      .warp = pointerWarp,
#endif
  };

  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && state.pointer == nullptr) {
      state.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  void layerConfigure(void* data, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t, uint32_t) {
    auto& state = *static_cast<State*>(data);
    zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
    wl_surface_attach(state.surface, state.windowBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, kSurfaceSize, kSurfaceSize);
    wl_surface_commit(state.surface);
    if (!state.ready) {
      state.ready = true;
      std::println("ready");
      std::fflush(stdout);
    }
  }

  void layerClosed(void* data, zwlr_layer_surface_v1*) {
    auto& state = *static_cast<State*>(data);
    state.failed = true;
    state.complete = true;
  }

  constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = layerConfigure,
      .closed = layerClosed,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
      wl_seat_add_listener(state.seat, &kSeatListener, &state);
    } else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0) {
      state.dataDeviceManager = static_cast<wl_data_device_manager*>(
          wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U))
      );
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
      state.layerShell = static_cast<zwlr_layer_shell_v1*>(
          wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4U))
      );
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
    }
  }

} // namespace

int main(int argc, char* argv[]) {
  if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "cursor-refresh")) {
    std::println(stderr, "usage: drag-client [cursor-refresh]");
    return EXIT_FAILURE;
  }

  State state;
  state.waitForPointerRefresh = argc == 2;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "drag-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.seat == nullptr
      || state.pointer == nullptr
      || state.dataDeviceManager == nullptr
      || state.layerShell == nullptr) {
    std::println(stderr, "drag-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  state.dataDevice = wl_data_device_manager_get_data_device(state.dataDeviceManager, state.seat);
  state.windowBuffer = createBuffer(state, kSurfaceSize, kSurfaceSize, 0xFF4477CC);
  state.iconBuffer = createBuffer(state, 24, 24, 0xFFFFAA22);
  if (state.windowBuffer.resource == nullptr || state.iconBuffer.resource == nullptr) {
    std::println(stderr, "drag-client: failed to allocate shared-memory buffers");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      state.layerShell, state.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "umbriel-drag-regression"
  );
  zwlr_layer_surface_v1_add_listener(state.layerSurface, &kLayerSurfaceListener, &state);
  zwlr_layer_surface_v1_set_size(state.layerSurface, kSurfaceSize, kSurfaceSize);
  zwlr_layer_surface_v1_set_anchor(
      state.layerSurface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
  );
  zwlr_layer_surface_v1_set_margin(state.layerSurface, 16, 0, 0, 16);
  zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, 0);
  wl_surface_commit(state.surface);

  while (!state.complete && wl_display_dispatch(state.display) >= 0) {
  }

  if (state.source != nullptr) {
    wl_data_source_destroy(state.source);
  }
  if (state.iconSurface != nullptr) {
    wl_surface_destroy(state.iconSurface);
  }
  zwlr_layer_surface_v1_destroy(state.layerSurface);
  wl_surface_destroy(state.surface);
  wl_data_device_release(state.dataDevice);
  wl_pointer_release(state.pointer);
  destroyBuffer(state.iconBuffer);
  destroyBuffer(state.windowBuffer);
  wl_display_disconnect(state.display);
  return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

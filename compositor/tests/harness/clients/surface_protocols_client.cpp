// Maps an xdg toplevel that exercises one surface protocol, then prints what a check needs. Every mode prints
// "mapped" once its window has been drawn. Usage: surface-protocols-client <mode> [title].
//   single-pixel   the window's content is one opaque red wp_single_pixel_buffer pixel, scaled by wp_viewporter
//   alpha          white content; each stdin line "alpha <0..1>" sets its wp_alpha_modifier multiplier and prints
//                  "alpha <value>" after that commit is drawn
//   dialog         then maps a child toplevel "<title>-modal" that xdg-dialog marks modal; "modal" once it is drawn
//   icon           names its xdg-toplevel-icon "utilities-terminal"
//   bell           rings xdg_system_bell_v1 with its surface and prints "rang"
//   fifo           sends 60 commits at once, each with wp_fifo set_barrier and wait_barrier and a frame callback, and
//                  prints "fifo <ms>": how long until the last callback, about one refresh per commit when honoured
//   commit-timing  commits with a wp_commit_timer timestamp 400 ms ahead and prints "early_ms <n>" or "late_ms <n>"
//                  for when its frame callback arrived, against that timestamp
//   toplevel-drag  a left press on it starts a data-device drag that carries a new toplevel "<title>-torn", attached
//                  with xdg-toplevel-drag at offset 20,10: prints "dragging", "torn" once that window is drawn,
//                  "enter <title>" for each drop-target surface the drag enters, and "drag-ended"

#include "alpha-modifier-v1-client-protocol.h"
#include "commit-timing-v1-client-protocol.h"
#include "fifo-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <limits>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  constexpr int kFifoCommits = 60;
  constexpr auto kCommitDelay = std::chrono::milliseconds(400);

  struct Window {
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wp_viewport* viewport = nullptr;
    int width = 256;
    int height = 256;
    bool configured = false;
    bool drawn = false;
  };

  struct State {
    std::string_view mode;
    std::string title;
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_single_pixel_buffer_manager_v1* singlePixel = nullptr;
    wp_alpha_modifier_v1* alphaModifier = nullptr;
    xdg_wm_dialog_v1* dialogs = nullptr;
    xdg_toplevel_icon_manager_v1* icons = nullptr;
    xdg_system_bell_v1* bell = nullptr;
    wp_fifo_manager_v1* fifoManager = nullptr;
    wp_commit_timing_manager_v1* timingManager = nullptr;
    wp_alpha_modifier_surface_v1* alpha = nullptr;
    Window main;
    Window child;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_data_device_manager* dataDevices = nullptr;
    wl_data_device* dataDevice = nullptr;
    xdg_toplevel_drag_manager_v1* toplevelDrags = nullptr;
    bool dragStarted = false;
    int fifoPending = 0;
    std::chrono::steady_clock::time_point fifoStart;
    timespec commitTarget{};
    bool failed = false;
  };

  std::chrono::nanoseconds monotonicNow() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
  }

  void say(std::string_view line) {
    std::println("{}", line);
    std::fflush(stdout);
  }

  wl_buffer* shmBuffer(State& state, int width, int height, uint32_t argb) {
    const int stride = width * 4;
    const auto size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    const int fd = memfd_create("umbriel-surface-protocols-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return nullptr;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return nullptr;
    }
    std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), argb);
    munmap(pixels, size);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  using FrameHandler = void (*)(State&);

  struct FrameWait {
    State* state;
    FrameHandler handler;
  };

  void frameDone(void* data, wl_callback* callback, uint32_t /*time*/) {
    wl_callback_destroy(callback);
    auto* wait = static_cast<FrameWait*>(data);
    wait->handler(*wait->state);
    delete wait;
  }
  constexpr wl_callback_listener kFrameListener = {.done = frameDone};

  void onFrame(State& state, wl_surface* surface, FrameHandler handler) {
    wl_callback_add_listener(wl_surface_frame(surface), &kFrameListener, new FrameWait{&state, handler});
  }

  void fifoFrame(State& state) {
    if (--state.fifoPending > 0) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - state.fifoStart;
    say(std::format("fifo {}", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
  }

  void runFifo(State& state) {
    wp_fifo_v1* fifo = wp_fifo_manager_v1_get_fifo(state.fifoManager, state.main.surface);
    wl_buffer* colors[] = {
        shmBuffer(state, state.main.width, state.main.height, 0xFF2040E0),
        shmBuffer(state, state.main.width, state.main.height, 0xFF20E040),
    };
    state.fifoPending = kFifoCommits;
    state.fifoStart = std::chrono::steady_clock::now();
    for (int index = 0; index < kFifoCommits; ++index) {
      wp_fifo_v1_set_barrier(fifo);
      wp_fifo_v1_wait_barrier(fifo);
      onFrame(state, state.main.surface, fifoFrame);
      wl_surface_attach(state.main.surface, colors[index % 2], 0, 0);
      wl_surface_damage_buffer(state.main.surface, 0, 0, state.main.width, state.main.height);
      wl_surface_commit(state.main.surface);
    }
  }

  void timedFrame(State& state) {
    const auto target =
        std::chrono::seconds(state.commitTarget.tv_sec) + std::chrono::nanoseconds(state.commitTarget.tv_nsec);
    const auto offset = std::chrono::duration_cast<std::chrono::milliseconds>(monotonicNow() - target).count();
    say(offset < 0 ? std::format("early_ms {}", -offset) : std::format("late_ms {}", offset));
  }

  void runCommitTiming(State& state) {
    wp_commit_timer_v1* timer = wp_commit_timing_manager_v1_get_timer(state.timingManager, state.main.surface);
    const auto target = monotonicNow() + kCommitDelay;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(target);
    state.commitTarget = {
        .tv_sec = static_cast<time_t>(seconds.count()),
        .tv_nsec = static_cast<long>((target - seconds).count()),
    };
    const auto sec = static_cast<uint64_t>(state.commitTarget.tv_sec);
    wp_commit_timer_v1_set_timestamp(
        timer, static_cast<uint32_t>(sec >> 32U), static_cast<uint32_t>(sec & 0xFFFFFFFFU),
        static_cast<uint32_t>(state.commitTarget.tv_nsec)
    );
    onFrame(state, state.main.surface, timedFrame);
    wl_surface_attach(state.main.surface, shmBuffer(state, state.main.width, state.main.height, 0xFFE04020), 0, 0);
    wl_surface_damage_buffer(state.main.surface, 0, 0, state.main.width, state.main.height);
    wl_surface_commit(state.main.surface);
  }

  void createWindow(State& state, Window& window, const std::string& title);

  void childDrawn(State& state) { say(state.mode == "dialog" ? "modal" : "torn"); }

  void mainDrawn(State& state) {
    say("mapped");
    if (state.mode == "bell") {
      xdg_system_bell_v1_ring(state.bell, state.main.surface);
      wl_display_roundtrip(state.display);
      say("rang");
    } else if (state.mode == "dialog") {
      createWindow(state, state.child, state.title + "-modal");
      xdg_toplevel_set_parent(state.child.toplevel, state.main.toplevel);
      xdg_dialog_v1_set_modal(xdg_wm_dialog_v1_get_xdg_dialog(state.dialogs, state.child.toplevel));
      wl_surface_commit(state.child.surface);
    } else if (state.mode == "fifo") {
      runFifo(state);
    } else if (state.mode == "commit-timing") {
      runCommitTiming(state);
    }
  }

  // Draws at the configured size; the first draw also reports the window once it is on screen.
  void draw(State& state, Window& window, bool first) {
    wl_buffer* buffer = nullptr;
    if (state.mode == "single-pixel" && &window == &state.main) {
      constexpr uint32_t kFull = std::numeric_limits<uint32_t>::max();
      buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(state.singlePixel, kFull, 0, 0, kFull);
      if (window.viewport == nullptr) {
        window.viewport = wp_viewporter_get_viewport(state.viewporter, window.surface);
      }
      wp_viewport_set_destination(window.viewport, window.width, window.height);
    } else {
      buffer = shmBuffer(state, window.width, window.height, &window == &state.main ? 0xFFFFFFFF : 0xFF808080);
    }
    if (buffer == nullptr) {
      state.failed = true;
      return;
    }
    if (first) {
      onFrame(state, window.surface, &window == &state.main ? mainDrawn : childDrawn);
    }
    wl_surface_attach(window.surface, buffer, 0, 0);
    wl_surface_damage_buffer(
        window.surface, 0, 0, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()
    );
    wl_surface_commit(window.surface);
  }

  struct WindowRef {
    State* state;
    Window* window;
  };

  void xdgConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* ref = static_cast<WindowRef*>(data);
    xdg_surface_ack_configure(surface, serial);
    const bool first = !ref->window->drawn;
    ref->window->drawn = true;
    draw(*ref->state, *ref->window, first);
  }
  constexpr xdg_surface_listener kXdgListener = {.configure = xdgConfigure};

  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto* ref = static_cast<WindowRef*>(data);
    if (width > 0 && height > 0) {
      ref->window->width = width;
      ref->window->height = height;
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

  void createWindow(State& state, Window& window, const std::string& title) {
    auto* ref = new WindowRef{&state, &window};
    window.surface = wl_compositor_create_surface(state.compositor);
    window.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, window.surface);
    xdg_surface_add_listener(window.xdgSurface, &kXdgListener, ref);
    window.toplevel = xdg_surface_get_toplevel(window.xdgSurface);
    xdg_toplevel_add_listener(window.toplevel, &kToplevelListener, ref);
    xdg_toplevel_set_title(window.toplevel, title.c_str());
    xdg_toplevel_set_app_id(window.toplevel, title.c_str());
  }

  constexpr uint32_t kLeftButton = 0x110;
  constexpr int kTornOffsetX = 20;
  constexpr int kTornOffsetY = 10;

  void sourceTarget(void*, wl_data_source*, const char*) {}
  void sourceSend(void*, wl_data_source*, const char*, int32_t fd) { close(fd); }
  void sourceEnded(void*, wl_data_source*) { say("drag-ended"); }
  void sourceDropPerformed(void*, wl_data_source*) {}
  void sourceAction(void*, wl_data_source*, uint32_t) {}
  constexpr wl_data_source_listener kSourceListener = {
      .target = sourceTarget,
      .send = sourceSend,
      .cancelled = sourceEnded,
      .dnd_drop_performed = sourceDropPerformed,
      .dnd_finished = sourceEnded,
      .action = sourceAction,
  };

  void deviceDataOffer(void*, wl_data_device*, wl_data_offer*) {}
  void deviceEnter(void* data, wl_data_device*, uint32_t, wl_surface* surface, wl_fixed_t, wl_fixed_t, wl_data_offer*) {
    const auto& state = *static_cast<State*>(data);
    say(std::format("enter {}", surface == state.child.surface ? state.title + "-torn" : state.title));
  }
  void deviceLeave(void*, wl_data_device*) {}
  void deviceMotion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
  void deviceDrop(void*, wl_data_device*) {}
  void deviceSelection(void*, wl_data_device*, wl_data_offer*) {}
  constexpr wl_data_device_listener kDeviceListener = {
      .data_offer = deviceDataOffer,
      .enter = deviceEnter,
      .leave = deviceLeave,
      .motion = deviceMotion,
      .drop = deviceDrop,
      .selection = deviceSelection,
  };

  void startTearOff(State& state, uint32_t serial) {
    wl_data_source* source = wl_data_device_manager_create_data_source(state.dataDevices);
    wl_data_source_add_listener(source, &kSourceListener, &state);
    wl_data_source_offer(source, "text/plain");
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    xdg_toplevel_drag_v1* drag = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(state.toplevelDrags, source);
    createWindow(state, state.child, state.title + "-torn");
    xdg_toplevel_drag_v1_attach(drag, state.child.toplevel, kTornOffsetX, kTornOffsetY);
    wl_data_device_start_drag(state.dataDevice, source, state.main.surface, nullptr, serial);
    wl_surface_commit(state.child.surface);
    say("dragging");
  }

  void pointerEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {}
  void pointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}
  void pointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
  void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t pressed) {
    auto& state = *static_cast<State*>(data);
    if (button == kLeftButton && pressed == WL_POINTER_BUTTON_STATE_PRESSED && !state.dragStarted) {
      state.dragStarted = true;
      startTearOff(state, serial);
    }
  }
  void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
  constexpr wl_pointer_listener kPointerListener = {
      .enter = pointerEnter,
      .leave = pointerLeave,
      .motion = pointerMotion,
      .button = pointerButton,
      .axis = pointerAxis,
      .frame = [](void*, wl_pointer*) {},
      .axis_source = [](void*, wl_pointer*, uint32_t) {},
      .axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
      .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
      .axis_value120 = [](void*, wl_pointer*, uint32_t, int32_t) {},
      .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
      .warp = [](void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {},
  };

  void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  template <typename T> T* bind(wl_registry* registry, uint32_t name, const wl_interface* interface, uint32_t version) {
    return static_cast<T*>(wl_registry_bind(registry, name, interface, version));
  }

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    const std::string_view id = interface;
    if (id == wl_compositor_interface.name) {
      state.compositor = bind<wl_compositor>(registry, name, &wl_compositor_interface, 4);
    } else if (id == wl_shm_interface.name) {
      state.shm = bind<wl_shm>(registry, name, &wl_shm_interface, 1);
    } else if (id == xdg_wm_base_interface.name) {
      state.wmBase = bind<xdg_wm_base>(registry, name, &xdg_wm_base_interface, std::min(version, 6U));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (id == wp_viewporter_interface.name) {
      state.viewporter = bind<wp_viewporter>(registry, name, &wp_viewporter_interface, 1);
    } else if (id == wp_single_pixel_buffer_manager_v1_interface.name) {
      state.singlePixel =
          bind<wp_single_pixel_buffer_manager_v1>(registry, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (id == wp_alpha_modifier_v1_interface.name) {
      state.alphaModifier = bind<wp_alpha_modifier_v1>(registry, name, &wp_alpha_modifier_v1_interface, 1);
    } else if (id == xdg_wm_dialog_v1_interface.name) {
      state.dialogs = bind<xdg_wm_dialog_v1>(registry, name, &xdg_wm_dialog_v1_interface, 1);
    } else if (id == xdg_toplevel_icon_manager_v1_interface.name) {
      state.icons = bind<xdg_toplevel_icon_manager_v1>(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (id == xdg_system_bell_v1_interface.name) {
      state.bell = bind<xdg_system_bell_v1>(registry, name, &xdg_system_bell_v1_interface, 1);
    } else if (id == wp_fifo_manager_v1_interface.name) {
      state.fifoManager = bind<wp_fifo_manager_v1>(registry, name, &wp_fifo_manager_v1_interface, 1);
    } else if (id == wl_seat_interface.name && state.seat == nullptr) {
      state.seat = bind<wl_seat>(registry, name, &wl_seat_interface, 1);
    } else if (id == wl_data_device_manager_interface.name) {
      state.dataDevices = bind<wl_data_device_manager>(registry, name, &wl_data_device_manager_interface, 3);
    } else if (id == xdg_toplevel_drag_manager_v1_interface.name) {
      state.toplevelDrags =
          bind<xdg_toplevel_drag_manager_v1>(registry, name, &xdg_toplevel_drag_manager_v1_interface, 1);
    } else if (id == wp_commit_timing_manager_v1_interface.name) {
      state.timingManager =
          bind<wp_commit_timing_manager_v1>(registry, name, &wp_commit_timing_manager_v1_interface, 1);
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  // The global the mode needs, or null when the compositor does not offer it.
  const void* modeGlobal(const State& state) {
    if (state.mode == "single-pixel") {
      return state.viewporter != nullptr ? static_cast<const void*>(state.singlePixel) : nullptr;
    }
    if (state.mode == "alpha") {
      return state.alphaModifier;
    }
    if (state.mode == "dialog") {
      return state.dialogs;
    }
    if (state.mode == "icon") {
      return state.icons;
    }
    if (state.mode == "bell") {
      return state.bell;
    }
    if (state.mode == "fifo") {
      return state.fifoManager;
    }
    if (state.mode == "commit-timing") {
      return state.timingManager;
    }
    if (state.mode == "toplevel-drag") {
      return state.seat != nullptr && state.dataDevices != nullptr ? static_cast<const void*>(state.toplevelDrags)
                                                                   : nullptr;
    }
    return nullptr;
  }

  void handleStdinLine(State& state, std::string_view line) {
    if (!line.starts_with("alpha ") || state.alpha == nullptr) {
      return;
    }
    const double value = std::clamp(std::strtod(std::string(line.substr(6)).c_str(), nullptr), 0.0, 1.0);
    wp_alpha_modifier_surface_v1_set_multiplier(
        state.alpha, static_cast<uint32_t>(value * std::numeric_limits<uint32_t>::max())
    );
    wl_surface_damage_buffer(state.main.surface, 0, 0, state.main.width, state.main.height);
    wl_surface_commit(state.main.surface);
    wl_display_roundtrip(state.display);
    say(std::format("alpha {}", value));
  }
} // namespace

int main(int argc, char** argv) {
  State state;
  state.mode = argc > 1 ? argv[1] : "";
  state.title = argc > 2 ? argv[2] : std::string(state.mode);
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "surface-protocols-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr) {
    std::println(stderr, "surface-protocols-client: missing a core global");
    return EXIT_FAILURE;
  }
  if (modeGlobal(state) == nullptr) {
    std::println(stderr, "surface-protocols-client: no global for mode '{}'", state.mode);
    return EXIT_FAILURE;
  }

  createWindow(state, state.main, state.title);
  if (state.mode == "alpha") {
    state.alpha = wp_alpha_modifier_v1_get_surface(state.alphaModifier, state.main.surface);
  } else if (state.mode == "toplevel-drag") {
    state.pointer = wl_seat_get_pointer(state.seat);
    wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    state.dataDevice = wl_data_device_manager_get_data_device(state.dataDevices, state.seat);
    wl_data_device_add_listener(state.dataDevice, &kDeviceListener, &state);
  } else if (state.mode == "icon") {
    xdg_toplevel_icon_v1* icon = xdg_toplevel_icon_manager_v1_create_icon(state.icons);
    xdg_toplevel_icon_v1_set_name(icon, "utilities-terminal");
    xdg_toplevel_icon_manager_v1_set_icon(state.icons, state.main.toplevel, icon);
    xdg_toplevel_icon_v1_destroy(icon);
  }
  wl_surface_commit(state.main.surface);

  std::string input;
  bool stdinOpen = true;
  while (!state.failed) {
    wl_display_flush(state.display);
    pollfd fds[2] = {
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = stdinOpen ? STDIN_FILENO : -1, .events = POLLIN, .revents = 0},
    };
    if (poll(fds, 2, -1) < 0) {
      break;
    }
    if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0) {
      break;
    }
    if ((fds[0].revents & (POLLERR | POLLHUP)) != 0) {
      break;
    }
    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[256];
      const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
      if (count <= 0) {
        stdinOpen = false;
        continue;
      }
      input.append(buffer, static_cast<size_t>(count));
      for (size_t end = input.find('\n'); end != std::string::npos; end = input.find('\n')) {
        handleStdinLine(state, std::string_view(input).substr(0, end));
        input.erase(0, end + 1);
      }
    }
  }
  return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

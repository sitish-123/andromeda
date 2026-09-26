// Sets an output's power through wlr-output-power-management and prints each mode the compositor reports as
// "mode on" / "mode off". Usage: output-power-client <output-name> on|off. Exits once the requested mode is reported,
// or with status 1 on a failed event.

#include "wlr-output-power-management-unstable-v1-client-protocol.h"

#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>
#include <wayland-client.h>

namespace {
  struct Output {
    wl_output* output = nullptr;
    std::string name;
  };

  struct State {
    zwlr_output_power_manager_v1* manager = nullptr;
    std::vector<Output*> outputs;
    uint32_t wanted = ZWLR_OUTPUT_POWER_V1_MODE_ON;
    bool done = false;
    bool failed = false;
  };

  void outputName(void* data, wl_output*, const char* name) { static_cast<Output*>(data)->name = name; }
  void outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
  void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
  void outputDone(void*, wl_output*) {}
  void outputScale(void*, wl_output*, int32_t) {}
  void outputDescription(void*, wl_output*, const char*) {}
  constexpr wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  void powerMode(void* data, zwlr_output_power_v1*, uint32_t mode) {
    auto& state = *static_cast<State*>(data);
    std::println("mode {}", mode == ZWLR_OUTPUT_POWER_V1_MODE_ON ? "on" : "off");
    std::fflush(stdout);
    state.done = state.done || mode == state.wanted;
  }
  void powerFailed(void* data, zwlr_output_power_v1*) {
    std::println("failed");
    static_cast<State*>(data)->failed = true;
  }
  constexpr zwlr_output_power_v1_listener kPowerListener = {.mode = powerMode, .failed = powerFailed};

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
      auto* output = new Output{};
      output->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 4));
      wl_output_add_listener(output->output, &kOutputListener, output);
      state.outputs.push_back(output);
    } else if (std::strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0) {
      state.manager = static_cast<zwlr_output_power_manager_v1*>(
          wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1)
      );
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || (std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0)) {
    std::println(stderr, "usage: output-power-client <output-name> on|off");
    return EXIT_FAILURE;
  }
  State state;
  state.wanted = std::strcmp(argv[2], "on") == 0 ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF;
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "output-power-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);
  if (state.manager == nullptr) {
    std::println(stderr, "output-power-client: no zwlr_output_power_manager_v1");
    return EXIT_FAILURE;
  }
  wl_output* target = nullptr;
  for (const Output* output : state.outputs) {
    if (output->name == argv[1]) {
      target = output->output;
    }
  }
  if (target == nullptr) {
    std::println(stderr, "output-power-client: no output named {}", argv[1]);
    return EXIT_FAILURE;
  }
  zwlr_output_power_v1* power = zwlr_output_power_manager_v1_get_output_power(state.manager, target);
  zwlr_output_power_v1_add_listener(power, &kPowerListener, &state);
  zwlr_output_power_v1_set_mode(power, state.wanted);
  while (!state.done && !state.failed && wl_display_dispatch(display) >= 0) {
  }
  return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

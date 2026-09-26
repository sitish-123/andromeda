// Drives dsk_output_manager_v1 for harness checks:
//   desktop-client state                     print "TARGET SOURCE" per output (SOURCE "-" when not mirroring)
//   desktop-client mirror TARGET SOURCE      exit 1 and print the reason if the compositor rejects it
//   desktop-client clear TARGET
//   desktop-client input-state               print "KEY=VALUE" per input setting, with " locked" when config.toml sets
//   it desktop-client input-set KEY VALUE       exit 1 and print the reason if the compositor rejects it
#include "desktop-unstable-v1-client-protocol.h"

#include <cstdint>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <wayland-client.h>

namespace {

  struct State {
    dsk_output_manager_v1* manager = nullptr;
    dsk_input_manager_v1* input = nullptr;
    bool inputDone = false;
    bool printInput = false;
    bool done = false;
    std::string failure;
    bool print = false;
  };

  void onMirror(void* data, dsk_output_manager_v1*, const char* target, const char* source) {
    if (static_cast<State*>(data)->print) {
      std::println("{} {}", target, source[0] != '\0' ? source : "-");
    }
  }
  void onDone(void* data, dsk_output_manager_v1*) { static_cast<State*>(data)->done = true; }
  void onFailed(void* data, dsk_output_manager_v1*, const char*, const char* reason) {
    static_cast<State*>(data)->failure = reason;
  }
  const dsk_output_manager_v1_listener kManager = {.mirror = onMirror, .done = onDone, .failed = onFailed};

  void onDeviceAdded(void*, dsk_input_manager_v1*, const char*, uint32_t) {}
  void onDeviceRemoved(void*, dsk_input_manager_v1*, const char*) {}
  void onSetting(void* data, dsk_input_manager_v1*, const char* key, const char* value, uint32_t locked) {
    if (static_cast<State*>(data)->printInput) {
      std::println("{}={}{}", key, value, locked != 0 ? " locked" : "");
    }
  }
  void onInputDone(void* data, dsk_input_manager_v1*) { static_cast<State*>(data)->inputDone = true; }
  void onInputFailed(void* data, dsk_input_manager_v1*, const char*, const char* reason) {
    static_cast<State*>(data)->failure = reason;
  }
  const dsk_input_manager_v1_listener kInput = {
      .device_added = onDeviceAdded,
      .device_removed = onDeviceRemoved,
      .setting = onSetting,
      .done = onInputDone,
      .failed = onInputFailed,
  };

  void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    if (std::strcmp(interface, dsk_output_manager_v1_interface.name) == 0) {
      auto* state = static_cast<State*>(data);
      state->manager =
          static_cast<dsk_output_manager_v1*>(wl_registry_bind(registry, name, &dsk_output_manager_v1_interface, 1));
      dsk_output_manager_v1_add_listener(state->manager, &kManager, state);
    } else if (std::strcmp(interface, dsk_input_manager_v1_interface.name) == 0) {
      auto* state = static_cast<State*>(data);
      state->input =
          static_cast<dsk_input_manager_v1*>(wl_registry_bind(registry, name, &dsk_input_manager_v1_interface, 1));
      dsk_input_manager_v1_add_listener(state->input, &kInput, state);
    }
  }
  void globalRemove(void*, wl_registry*, uint32_t) {}
  const wl_registry_listener kRegistry = {.global = global, .global_remove = globalRemove};

} // namespace

int main(int argc, char** argv) {
  const std::string_view command = argc > 1 ? argv[1] : "";
  const bool inputCommand = command == "input-state" || command == "input-set";
  if (!(command == "state" && argc == 2)
      && !(command == "mirror" && argc == 4)
      && !(command == "clear" && argc == 3)
      && !(command == "input-state" && argc == 2)
      && !(command == "input-set" && argc == 4)) {
    std::println(
        stderr, "usage: desktop-client state | mirror TARGET SOURCE | clear TARGET | input-state | input-set KEY VALUE"
    );
    return 2;
  }
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "desktop-client: cannot connect");
    return 1;
  }
  State state;
  state.print = command == "state";
  state.printInput = command == "input-state";
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistry, &state);
  wl_display_roundtrip(display);
  if (inputCommand) {
    if (state.input == nullptr) {
      std::println(stderr, "desktop-client: dsk_input_manager_v1 is not advertised");
      return 1;
    }
    while (!state.inputDone && wl_display_dispatch(display) >= 0) {
    }
    if (command == "input-set") {
      state.printInput = false;
      dsk_input_manager_v1_set(state.input, argv[2], argv[3]);
      wl_display_roundtrip(display);
    }
    if (!state.failure.empty()) {
      std::println(stderr, "{}", state.failure);
      return 1;
    }
    return 0;
  }
  if (state.manager == nullptr) {
    std::println(stderr, "desktop-client: dsk_output_manager_v1 is not advertised");
    return 1;
  }
  while (!state.done && wl_display_dispatch(display) >= 0) {
  }
  if (command == "mirror") {
    dsk_output_manager_v1_set_mirror(state.manager, argv[2], argv[3]);
  } else if (command == "clear") {
    dsk_output_manager_v1_clear_mirror(state.manager, argv[2]);
  }
  // A rejection arrives before the roundtrip's callback.
  wl_display_roundtrip(display);
  if (!state.failure.empty()) {
    std::println(stderr, "{}", state.failure);
    return 1;
  }
  return 0;
}

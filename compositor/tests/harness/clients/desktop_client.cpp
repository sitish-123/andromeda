// Drives dsk_output_manager_v1 for harness checks:
//   desktop-client state                     print "TARGET SOURCE" per output (SOURCE "-" when not mirroring)
//   desktop-client mirror TARGET SOURCE      exit 1 and print the reason if the compositor rejects it
//   desktop-client clear TARGET
//   desktop-client properties-state          print "TARGET KEY=VALUE" per output and display property
//   desktop-client property-set TARGET KEY VALUE  exit 1 and print the reason if the compositor rejects it
//   desktop-client settings-state            print "KEY=VALUE" per managed setting, with " customized" when the
//                                            settings file holds it
//   desktop-client settings-set KEY VALUE    exit 1 and print the reason if the compositor rejects it
//   desktop-client settings-watch            print every "KEY=VALUE" event, as settings-state does, until killed
//   desktop-client layer-hold LAYER KEY VALUE  set KEY in LAYER, print "ready", then hold the connection until killed
//                                            (DESKTOP_CLIENT_SETTINGS_VERSION=1 binds the v1 protocol;
//                                            DESKTOP_CLIENT_LAYER_REPEAT=1 sends the request again after a stdin line and prints "repeated")
//   desktop-client keybinds-state            print "CHORD=ACTION" per effective keybind, " customized" as above
//   desktop-client keybind-set CHORD ACTION  exit 1 and print the reason if the compositor rejects it
//   desktop-client shell-events              bind dsk_shell_v1 as the shell does and print "ready", then
//                                            "action COMMAND", "bell APP_ID", "text-input-state enabled=E purpose=P hint=H
//                                            rect=X,Y,W,H", "relock", "recovered" and "restarted" per event until killed. Stdin lines
//                                            send requests: "insert TEXT" (a backslash and n sends a newline), "key KEYSYM STATE" (decimal), "activity PID
//                                            FLAGS", and "mark LABEL" prints "mark LABEL" after the compositor handled
//                                            every earlier request. DESKTOP_CLIENT_SHELL_VERSION=2 binds version 2.
#include "desktop-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  struct State {
    dsk_output_manager_v1* manager = nullptr;
    dsk_settings_manager_v1* input = nullptr;
    dsk_shell_v1* shell = nullptr;
    bool inputDone = false;
    bool printInput = false;
    bool printKeybinds = false;
    bool done = false;
    std::string failure;
    bool print = false;
    bool printProperties = false;
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
  void onProperty(void* data, dsk_output_manager_v1*, const char* target, const char* key, const char* value) {
    if (static_cast<State*>(data)->printProperties) {
      std::println("{} {}={}", target, key, value);
    }
  }
  const dsk_output_manager_v1_listener kManager = {
      .property = onProperty,
      .mirror = onMirror,
      .done = onDone,
      .failed = onFailed,
  };

  void onDeviceAdded(void*, dsk_settings_manager_v1*, const char*, uint32_t) {}
  void onDeviceRemoved(void*, dsk_settings_manager_v1*, const char*) {}
  void onSetting(void* data, dsk_settings_manager_v1*, const char* key, const char* value, uint32_t customized) {
    if (static_cast<State*>(data)->printInput) {
      std::println("{}={}{}", key, value, customized != 0 ? " customized" : "");
      std::fflush(stdout);
    }
  }
  void onKeybind(void* data, dsk_settings_manager_v1*, const char* chord, const char* action, uint32_t customized) {
    if (static_cast<State*>(data)->printKeybinds) {
      std::println("{}={}{}", chord, action, customized != 0 ? " customized" : "");
    }
  }
  void onActionSpec(void*, dsk_settings_manager_v1*, const char*, const char*, const char*) {}
  void onChordCaptured(void*, dsk_settings_manager_v1*, const char*) {}
  void onInputDone(void* data, dsk_settings_manager_v1*) { static_cast<State*>(data)->inputDone = true; }
  void onInputFailed(void* data, dsk_settings_manager_v1*, const char*, const char* reason) {
    static_cast<State*>(data)->failure = reason;
  }
  const dsk_settings_manager_v1_listener kInput = {
      .device_added = onDeviceAdded,
      .device_removed = onDeviceRemoved,
      .setting = onSetting,
      .keybind = onKeybind,
      .action_spec = onActionSpec,
      .chord_captured = onChordCaptured,
      .done = onInputDone,
      .failed = onInputFailed,
  };

  void onShellAction(void*, dsk_shell_v1*, const char* command) {
    std::println("action {}", command);
    std::fflush(stdout);
  }
  void onShellLockKeys(void*, dsk_shell_v1*, uint32_t, uint32_t, uint32_t) {}
  void onShellBell(void*, dsk_shell_v1*, const char* appId) {
    std::println("bell {}", appId);
    std::fflush(stdout);
  }
  void onShellTextInputState(
      void*, dsk_shell_v1*, uint32_t enabled, uint32_t purpose, uint32_t hint, int32_t x, int32_t y, int32_t w, int32_t h
  ) {
    std::println("text-input-state enabled={} purpose={} hint={} rect={},{},{},{}", enabled, purpose, hint, x, y, w, h);
    std::fflush(stdout);
  }
  void onShellRelock(void*, dsk_shell_v1*) {
    std::println("relock");
    std::fflush(stdout);
  }
  void onShellRecovered(void*, dsk_shell_v1*) {
    std::println("recovered");
    std::fflush(stdout);
  }
  void onShellRestarted(void*, dsk_shell_v1*) {
    std::println("restarted");
    std::fflush(stdout);
  }
  const dsk_shell_v1_listener kShell = {
      .action = onShellAction,
      .lock_keys = onShellLockKeys,
      .bell = onShellBell,
      .text_input_state = onShellTextInputState,
      .relock = onShellRelock,
      .recovered = onShellRecovered,
      .restarted = onShellRestarted,
  };

  uint32_t settingsVersion() {
    const char* forced = std::getenv("DESKTOP_CLIENT_SETTINGS_VERSION");
    return forced != nullptr ? static_cast<uint32_t>(std::atoi(forced)) : 2U;
  }

  void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (std::strcmp(interface, dsk_output_manager_v1_interface.name) == 0) {
      auto* state = static_cast<State*>(data);
      state->manager =
          static_cast<dsk_output_manager_v1*>(wl_registry_bind(registry, name, &dsk_output_manager_v1_interface, 1));
      dsk_output_manager_v1_add_listener(state->manager, &kManager, state);
    } else if (std::strcmp(interface, dsk_settings_manager_v1_interface.name) == 0) {
      auto* state = static_cast<State*>(data);
      state->input = static_cast<dsk_settings_manager_v1*>(
          wl_registry_bind(registry, name, &dsk_settings_manager_v1_interface, settingsVersion())
      );
      dsk_settings_manager_v1_add_listener(state->input, &kInput, state);
    } else if (std::strcmp(interface, dsk_shell_v1_interface.name) == 0) {
      auto* state = static_cast<State*>(data);
      const char* wanted = std::getenv("DESKTOP_CLIENT_SHELL_VERSION");
      state->shell = static_cast<dsk_shell_v1*>(wl_registry_bind(
          registry, name, &dsk_shell_v1_interface, std::min(version, wanted != nullptr ? static_cast<uint32_t>(std::atoi(wanted)) : 4U)
      ));
      dsk_shell_v1_add_listener(state->shell, &kShell, state);
    }
  }
  void globalRemove(void*, wl_registry*, uint32_t) {}
  const wl_registry_listener kRegistry = {.global = global, .global_remove = globalRemove};

} // namespace

int main(int argc, char** argv) {
  const std::string_view command = argc > 1 ? argv[1] : "";
  const bool inputCommand = command == "settings-state"
      || command == "settings-set"
      || command == "layer-hold"
      || command == "settings-watch"
      || command == "keybinds-state"
      || command == "keybind-set";
  if (!(command == "state" && argc == 2)
      && !(command == "mirror" && argc == 4)
      && !(command == "clear" && argc == 3)
      && !(command == "properties-state" && argc == 2)
      && !(command == "property-set" && argc == 5)
      && !(command == "settings-state" && argc == 2)
      && !(command == "settings-set" && argc == 4)
      && !(command == "layer-hold" && argc == 5)
      && !(command == "settings-watch" && argc == 2)
      && !(command == "keybinds-state" && argc == 2)
      && !(command == "keybind-set" && argc == 4)
      && !(command == "shell-events" && argc == 2)) {
    std::println(
        stderr,
        "usage: desktop-client state | mirror TARGET SOURCE | clear TARGET | properties-state | property-set TARGET KEY "
        "VALUE | settings-state | settings-set KEY VALUE | keybinds-state | keybind-set CHORD ACTION | shell-events"
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
  state.printProperties = command == "properties-state";
  state.printInput = command == "settings-state" || command == "settings-watch";
  state.printKeybinds = command == "keybinds-state";
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistry, &state);
  wl_display_roundtrip(display);
  if (command == "shell-events") {
    if (state.shell == nullptr) {
      std::println(stderr, "desktop-client: dsk_shell_v1 is not advertised");
      return 1;
    }
    wl_display_roundtrip(display);
    std::println("ready");
    std::fflush(stdout);
    bool stdinOpen = true;
    std::string pending;
    while (wl_display_flush(display) >= 0) {
      while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
      }
      pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
      if (poll(fds, stdinOpen ? 2 : 1, -1) < 0) {
        wl_display_cancel_read(display);
        return 1;
      }
      if ((fds[0].revents & POLLIN) == 0) {
        wl_display_cancel_read(display);
      } else if (wl_display_read_events(display) < 0) {
        return 1;
      }
      wl_display_dispatch_pending(display);
      if (stdinOpen && (fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        char chunk[512];
        const ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got <= 0) {
          stdinOpen = false;
        } else {
          pending.append(chunk, static_cast<size_t>(got));
        }
      }
      for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
        const std::string line = pending.substr(0, end);
        pending.erase(0, end + 1);
        const std::string_view text(line);
        if (text.starts_with("insert ")) {
          std::string inserted = line.substr(7);
          for (size_t at; (at = inserted.find("\\n")) != std::string::npos;) {
            inserted.replace(at, 2, "\n");
          }
          dsk_shell_v1_insert_text(state.shell, inserted.c_str());
        } else if (text.starts_with("key ")) {
          unsigned long keysym = 0;
          unsigned long keyState = 0;
          std::sscanf(line.c_str() + 4, "%lu %lu", &keysym, &keyState);
          dsk_shell_v1_key(state.shell, static_cast<uint32_t>(keysym), static_cast<uint32_t>(keyState));
        } else if (text.starts_with("activity ")) {
          unsigned long pid = 0;
          unsigned long flags = 0;
          std::sscanf(line.c_str() + 9, "%lu %lu", &pid, &flags);
          dsk_shell_v1_app_activity(state.shell, static_cast<uint32_t>(pid), static_cast<uint32_t>(flags));
        } else if (text.starts_with("mark ")) {
          wl_display_roundtrip(display);
          std::println("mark {}", line.c_str() + 5);
          std::fflush(stdout);
        }
      }
    }
    return 0;
  }
  if (inputCommand) {
    if (state.input == nullptr) {
      std::println(stderr, "desktop-client: dsk_settings_manager_v1 is not advertised");
      return 1;
    }
    while (!state.inputDone && wl_display_dispatch(display) >= 0) {
    }
    if (command == "settings-watch") {
      while (wl_display_dispatch(display) >= 0) {
      }
    } else if (command == "settings-set") {
      state.printInput = false;
      dsk_settings_manager_v1_set(state.input, argv[2], argv[3]);
      wl_display_roundtrip(display);
    } else if (command == "layer-hold") {
      state.printInput = false;
      dsk_settings_manager_v1_set_in_layer(state.input, argv[2], argv[3], argv[4]);
      wl_display_roundtrip(display);
      if (state.failure.empty()) {
        std::println("ready");
        std::fflush(stdout);
        std::string line;
        if (std::getenv("DESKTOP_CLIENT_LAYER_REPEAT") != nullptr && std::getline(std::cin, line)) {
          dsk_settings_manager_v1_set_in_layer(state.input, argv[2], argv[3], argv[4]);
          wl_display_roundtrip(display);
          std::println("repeated");
          std::fflush(stdout);
        }
        while (wl_display_dispatch(display) >= 0) {
        }
      }
    } else if (command == "keybind-set") {
      dsk_settings_manager_v1_bind(state.input, argv[2], argv[3]);
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
  } else if (command == "property-set") {
    dsk_output_manager_v1_set_property(state.manager, argv[2], argv[3], argv[4]);
  }
  // A rejection arrives before the roundtrip's callback.
  wl_display_roundtrip(display);
  if (!state.failure.empty()) {
    std::println(stderr, "{}", state.failure);
    return 1;
  }
  return 0;
}

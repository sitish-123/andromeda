// Harness helper: requests an xdg-activation token with an app id hint, as the shell's dock and launcher do for a
// launch, and prints it. Usage: activation-token-client <app-id>
#include "xdg-activation-v1-client-protocol.h"

#include <cstring>
#include <print>
#include <string>
#include <wayland-client.h>

namespace {

  struct State {
    xdg_activation_v1* activation = nullptr;
    std::string token;
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t /*version*/) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0) {
      state.activation = static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    }
  }

  void registryGlobalRemove(void* /*data*/, wl_registry* /*registry*/, uint32_t /*name*/) {}

  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryGlobalRemove};

  void tokenDone(void* data, xdg_activation_token_v1* /*token*/, const char* token) {
    static_cast<State*>(data)->token = token;
  }

  constexpr xdg_activation_token_v1_listener kTokenListener = {.done = tokenDone};

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::println(stderr, "usage: activation-token-client <app-id>");
    return 2;
  }
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "activation-token-client: cannot connect");
    return 1;
  }
  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);
  if (state.activation == nullptr) {
    std::println(stderr, "activation-token-client: compositor is missing xdg_activation_v1");
    return 1;
  }
  xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(state.activation);
  xdg_activation_token_v1_add_listener(token, &kTokenListener, &state);
  xdg_activation_token_v1_set_app_id(token, argv[1]);
  xdg_activation_token_v1_commit(token);
  while (state.token.empty() && wl_display_dispatch(display) != -1) {
  }
  xdg_activation_token_v1_destroy(token);
  wl_display_roundtrip(display);
  std::println("{}", state.token);
  wl_display_disconnect(display);
  return state.token.empty() ? 1 : 0;
}

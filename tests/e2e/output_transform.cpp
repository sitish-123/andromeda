// Applies a transform to one output through OutputManagement, keeping every other head as reported.
// Usage: output-transform OUTPUT TRANSFORM (a wl_output_transform value). Exits non-zero unless the apply succeeds.
#include "wayland/output_management.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <wayland-client.h>

namespace {

  struct Global {
    std::uint32_t name = 0;
    std::uint32_t version = 0;
  };

  void onGlobal(void* data, wl_registry*, std::uint32_t name, const char* interface, std::uint32_t version) {
    if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
      *static_cast<Global*>(data) = {name, std::min(version, 4U)};
    }
  }
  void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
  const wl_registry_listener kRegistry = {.global = onGlobal, .global_remove = onGlobalRemove};

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fputs("usage: output-transform OUTPUT TRANSFORM\n", stderr);
    return 2;
  }
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    return 1;
  }
  Global global;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistry, &global);
  wl_display_roundtrip(display);
  if (global.name == 0) {
    return 1;
  }
  OutputManagement om(registry, global.name, global.version, {});
  for (int i = 0; i < 200 && !om.ready(); ++i) {
    wl_display_roundtrip(display);
  }
  const auto head = std::ranges::find(om.heads(), std::string(argv[1]), &OutputHead::name);
  if (head == om.heads().end()) {
    std::fprintf(stderr, "unknown output %s\n", argv[1]);
    return 1;
  }
  OutputHeadConfig changed = OutputManagement::currentConfig(*head);
  changed.transform = std::atoi(argv[2]);
  std::optional<OutputApplyResult> result;
  if (!om.apply({&changed, 1}, false, [&](OutputApplyResult r) { result = r; })) {
    return 1;
  }
  for (int i = 0; i < 200 && !result; ++i) {
    wl_display_roundtrip(display);
  }
  std::printf("apply transform %s on %s: %s\n", argv[2], argv[1], result == OutputApplyResult::Succeeded ? "succeeded" : "failed");
  return result == OutputApplyResult::Succeeded ? 0 : 1;
}

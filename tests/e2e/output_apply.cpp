// Drives OutputManagement against a live compositor: apply, verify, disable, re-enable, revert.
// Prints one line per step and exits non-zero on the first mismatch.
#include "wayland/output_management.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
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

  wl_display* g_display = nullptr;
  int g_failures = 0;

  void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
  }

  // Dispatches until `done` or 200 roundtrips, the compositor answers within one or two.
  template <typename Pred> bool dispatchUntil(Pred done) {
    for (int i = 0; i < 200 && !done(); ++i) {
      if (wl_display_roundtrip(g_display) < 0) {
        return false;
      }
    }
    return done();
  }

  std::optional<OutputApplyResult> applyAndWait(OutputManagement& om, const std::vector<OutputHeadConfig>& config) {
    std::optional<OutputApplyResult> result;
    if (!om.apply(config, false, [&](OutputApplyResult r) { result = r; })) {
      return std::nullopt;
    }
    dispatchUntil([&] { return result.has_value(); });
    wl_display_roundtrip(g_display); // collect the state events that follow a success
    return result;
  }

  const OutputHead* head(const OutputManagement& om, const std::string& name) {
    const auto it = std::ranges::find(om.heads(), name, &OutputHead::name);
    return it != om.heads().end() ? &*it : nullptr;
  }

} // namespace

int main() {
  g_display = wl_display_connect(nullptr);
  if (g_display == nullptr) {
    std::puts("FAIL connect");
    return 1;
  }
  Global global;
  wl_registry* registry = wl_display_get_registry(g_display);
  wl_registry_add_listener(registry, &kRegistry, &global);
  wl_display_roundtrip(g_display);
  check(global.name != 0, "compositor advertises zwlr_output_manager_v1");
  if (global.name == 0) {
    return 1;
  }

  OutputManagement om(registry, global.name, global.version, {});
  check(dispatchUntil([&] { return om.ready(); }), "initial state received");
  check(om.heads().size() == 2, "two heads reported");
  if (om.heads().size() != 2) {
    return 1;
  }
  const std::string target = om.heads()[1].name;
  std::vector<OutputHeadConfig> original;
  for (const OutputHead& h : om.heads()) {
    original.push_back(OutputManagement::currentConfig(h));
  }

  OutputHeadConfig changed = OutputManagement::currentConfig(*head(om, target));
  changed.scale = 2.0;
  changed.x = 0;
  changed.y = 720;
  auto result = applyAndWait(om, {changed});
  check(result == OutputApplyResult::Succeeded, "apply scale 2 and position 0,720 on " + target);
  const OutputHead* h = head(om, target);
  check(h != nullptr && std::abs(h->scale - 2.0) < 0.001, "compositor reports scale 2");
  check(h != nullptr && h->x == 0 && h->y == 720, "compositor reports position 0,720");

  OutputHeadConfig disabled = OutputManagement::currentConfig(*head(om, target));
  disabled.enabled = false;
  result = applyAndWait(om, {disabled});
  check(result == OutputApplyResult::Succeeded, "disable " + target);
  check(!head(om, target)->enabled, "compositor reports disabled");

  result = applyAndWait(om, original);
  check(result == OutputApplyResult::Succeeded, "revert to original configuration");
  h = head(om, target);
  check(h != nullptr && h->enabled && std::abs(h->scale - original[1].scale) < 0.001, "original state restored");

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

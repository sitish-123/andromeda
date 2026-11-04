#include <dlfcn.h>

#include <cstdio>

#include <cstdint>

struct liftoff_output;
struct _drmModeAtomicReq;

struct liftoff_output_apply_options {
  int64_t timeout_ns;
};

namespace {
  // wlroots passes no options, so libliftoff searches planes for at most 1 ms. One i915 test commit takes 1-2 ms (Skylake), so
  // that, and the overlay layer then always falls back to composition. The search runs only when the layer layout
  // changes; libliftoff reuses its result on other frames.
  constexpr int64_t kPlaneSearchNs = 20'000'000;

  using ApplyFn = int (*)(liftoff_output*, _drmModeAtomicReq*, uint32_t, const liftoff_output_apply_options*);
} // namespace

// Interposes libliftoff's symbol for libwlroots (the executable comes first in symbol lookup).
extern "C" __attribute__((visibility("default"))) int liftoff_output_apply(
    liftoff_output* output, _drmModeAtomicReq* req, uint32_t flags, const liftoff_output_apply_options* options
) {
  static const auto real = [] {
    std::fprintf(stderr, "liftoff_output_apply: plane search deadline %lld ns\n", static_cast<long long>(kPlaneSearchNs));
    return reinterpret_cast<ApplyFn>(dlsym(RTLD_NEXT, "liftoff_output_apply"));
  }();
  const liftoff_output_apply_options longer{.timeout_ns = kPlaneSearchNs};
  return real(output, req, flags, options != nullptr && options->timeout_ns != 0 ? options : &longer);
}

struct liftoff_layer;
struct liftoff_plane;

// wlroots 0.20 asks this for every DRM plane when a committed layer falls back to composition, but planes lit at
// startup get no libliftoff plane until a modeset: a NULL plane crashes libliftoff. Such a plane is no candidate.
extern "C" __attribute__((visibility("default"))) bool
liftoff_layer_is_candidate_plane(liftoff_layer* layer, liftoff_plane* plane) {
  using CandidateFn = bool (*)(liftoff_layer*, liftoff_plane*);
  static const auto real = reinterpret_cast<CandidateFn>(dlsym(RTLD_NEXT, "liftoff_layer_is_candidate_plane"));
  return plane != nullptr && real(layer, plane);
}

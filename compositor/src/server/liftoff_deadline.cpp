#include <dlfcn.h>

#include <cstdio>

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <utility>
#include <vector>

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

  struct LayerRecord {
    liftoff_output* output;
    void* layer;
  };
  // Every libliftoff layer and each output's composition layer. wlroots gives the composition layer the primary
  // buffer too; it is unused when every layer has a plane, so only the others tell whether a layer was composited.
  std::vector<LayerRecord> g_layers;
  std::vector<LayerRecord> g_compositionLayers;
  bool g_composited = false;
  // Layers that hold an IN_FENCE_FD; fresh = set since the output's last apply.
  struct FenceRecord {
    void* layer;
    bool fresh;
  };
  std::vector<FenceRecord> g_fences;

  template <typename Fn> Fn next(const char* name) { return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name)); }
} // namespace

extern "C" __attribute__((visibility("default"))) void* liftoff_layer_create(liftoff_output* output) {
  static const auto real = next<void* (*)(liftoff_output*)>("liftoff_layer_create");
  void* layer = real(output);
  if (layer != nullptr) {
    g_layers.push_back({output, layer});
  }
  return layer;
}

extern "C" __attribute__((visibility("default"))) void liftoff_layer_destroy(void* layer) {
  static const auto real = next<void (*)(void*)>("liftoff_layer_destroy");
  std::erase_if(g_layers, [layer](const LayerRecord& r) { return r.layer == layer; });
  std::erase_if(g_compositionLayers, [layer](const LayerRecord& r) { return r.layer == layer; });
  std::erase_if(g_fences, [layer](const FenceRecord& r) { return r.layer == layer; });
  real(layer);
}

extern "C" __attribute__((visibility("default"))) void liftoff_output_set_composition_layer(liftoff_output* output, void* layer) {
  static const auto real = next<void (*)(liftoff_output*, void*)>("liftoff_output_set_composition_layer");
  std::erase_if(g_compositionLayers, [output](const LayerRecord& r) { return r.output == output; });
  g_compositionLayers.push_back({output, layer});
  real(output, layer);
}

extern "C" __attribute__((visibility("default"))) int liftoff_layer_set_property(void* layer, const char* name, uint64_t value) {
  static const auto real = next<int (*)(void*, const char*, uint64_t)>("liftoff_layer_set_property");
  if (std::strcmp(name, "IN_FENCE_FD") == 0) {
    std::erase_if(g_fences, [layer](const FenceRecord& r) { return r.layer == layer; });
    g_fences.push_back({layer, true});
  }
  return real(layer, name, value);
}

// wlroots 0.20 sets IN_FENCE_FD on the primary and composition layers only for a commit that has a fence, never clears
// it, and closes the fd after the commit. A later commit without a fence (a direct-scanout buffer with no acquire
// point) then sends that closed fd and the kernel fails it with EINVAL. Clear each fence that was not set again.
static void clearStaleFences(liftoff_output* output) {
  static const auto unset = next<void (*)(void*, const char*)>("liftoff_layer_unset_property");
  std::erase_if(g_fences, [output](FenceRecord& fence) {
    const bool ours = std::ranges::any_of(g_layers, [&](const LayerRecord& r) {
      return r.layer == fence.layer && r.output == output;
    });
    if (!ours) {
      return false;
    }
    if (fence.fresh) {
      fence.fresh = false;
      return false;
    }
    unset(fence.layer, "IN_FENCE_FD");
    return true;
  });
}

// Whether the plane search since the last call left a layer to composition. wlroots' test commit passes then, and the
// scene reads this to reject an overlay layout that would lose a layer.
extern "C" __attribute__((visibility("default"))) bool andromeda_liftoff_take_composited() {
  const bool composited = g_composited;
  g_composited = false;
  return composited;
}

// Interposes libliftoff's symbol for libwlroots (the executable comes first in symbol lookup).
extern "C" __attribute__((visibility("default"))) int liftoff_output_apply(
    liftoff_output* output, _drmModeAtomicReq* req, uint32_t flags, const liftoff_output_apply_options* options
) {
  static const auto real = [] {
    std::fprintf(stderr, "liftoff_output_apply: plane search deadline %lld ns\n", static_cast<long long>(kPlaneSearchNs));
    return reinterpret_cast<ApplyFn>(dlsym(RTLD_NEXT, "liftoff_output_apply"));
  }();
  static const auto needsComposition =
      reinterpret_cast<bool (*)(void*)>(dlsym(RTLD_DEFAULT, "liftoff_layer_needs_composition"));
  clearStaleFences(output);
  const liftoff_output_apply_options longer{.timeout_ns = kPlaneSearchNs};
  const int ret = real(output, req, flags, options != nullptr && options->timeout_ns != 0 ? options : &longer);
  if (ret == 0 && needsComposition != nullptr) {
    for (const LayerRecord& record : g_layers) {
      const bool composition = std::ranges::any_of(g_compositionLayers, [&record](const LayerRecord& c) {
        return c.layer == record.layer;
      });
      g_composited |= record.output == output && !composition && needsComposition(record.layer);
    }
  }
  return ret;
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

#pragma once

#include "pipewire/pipewire_service.h"

#include <cstdint>
#include <span>
#include <vector>

using AudioDeviceRoutes = std::span<const PipeWireService::DeviceRouteData>;

// Passed as profileDevice when a route list is not bound to a card.profile.device.
inline constexpr std::int32_t kAnyProfileDevice = -1;

// Select the best available route for one audio node. A non-negative profileDevice binds the
// selection to that card.profile.device; kAnyProfileDevice selects direction-wide.
[[nodiscard]] const PipeWireService::DeviceRouteData*
activeAudioDeviceRoute(AudioDeviceRoutes routes, std::uint32_t wantDirection, std::int32_t profileDevice);

// Node-local routes are direction-wide. Card routes only affect a node when their device matches
// card.profile.device; an explicitly unavailable matching route hides the node.
[[nodiscard]] bool audioNodeRouteAvailable(
    AudioDeviceRoutes nodeRoutes, AudioDeviceRoutes deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
);

// The route describing one audio node, available or not: the best node-local one, else the card route bound to
// profileDevice. nullptr when neither exists.
[[nodiscard]] const PipeWireService::DeviceRouteData* audioNodeRoute(
    AudioDeviceRoutes nodeRoutes, AudioDeviceRoutes deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
);

// Headphones came out of the jack: the card's active output route for `now.device` was a headphones or headset port
// and is now another port, or that route became unavailable (jack sense). `before` is the card's routes until now.
// WirePlumber pauses media when a Bluetooth, USB or HDMI sink goes away, never for a jack (docs/system-health.md §12.2).
[[nodiscard]] bool headphonesUnplugged(AudioDeviceRoutes before, const PipeWireService::DeviceRouteData& now);

// One output row a user picks from. A virtual sink that enhances another (andromeda.enhances) is not a row: the sink
// it enhances stands for it, and picking that row picks the enhancer (`pickId`). Unavailable ports are dropped unless
// current.
struct SinkChoice {
  const AudioNode* node = nullptr;
  std::uint32_t pickId = 0;
  bool current = false;
};
[[nodiscard]] std::vector<SinkChoice> sinkChoices(const AudioState& state);

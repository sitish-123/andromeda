#include "pipewire/audio_route_selection.h"

#include <algorithm>

namespace {
  [[nodiscard]] bool routeMatchesDevice(const PipeWireService::DeviceRouteData& route, std::int32_t profileDevice) {
    return profileDevice == kAnyProfileDevice || route.device == profileDevice;
  }

  [[nodiscard]] bool routeIsSelectable(
      const PipeWireService::DeviceRouteData& route, std::uint32_t wantDirection, std::int32_t profileDevice
  ) {
    return route.index >= 0
        && route.direction == wantDirection
        && route.available != SPA_PARAM_AVAILABILITY_no
        && routeMatchesDevice(route, profileDevice);
  }

  [[nodiscard]] bool routeIsBetterCandidate(
      const PipeWireService::DeviceRouteData& candidate, const PipeWireService::DeviceRouteData& current
  ) {
    const bool candidateAvailable = candidate.available == SPA_PARAM_AVAILABILITY_yes;
    const bool currentAvailable = current.available == SPA_PARAM_AVAILABILITY_yes;
    if (candidateAvailable != currentAvailable) {
      return candidateAvailable;
    }
    return candidate.priority > current.priority;
  }
} // namespace

const PipeWireService::DeviceRouteData*
activeAudioDeviceRoute(AudioDeviceRoutes routes, std::uint32_t wantDirection, std::int32_t profileDevice) {
  const PipeWireService::DeviceRouteData* best = nullptr;
  for (const auto& route : routes) {
    if (!routeIsSelectable(route, wantDirection, profileDevice)) {
      continue;
    }
    if (best == nullptr || routeIsBetterCandidate(route, *best)) {
      best = &route;
    }
  }
  return best;
}

bool audioNodeRouteAvailable(
    AudioDeviceRoutes nodeRoutes, AudioDeviceRoutes deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
) {
  if (activeAudioDeviceRoute(nodeRoutes, wantDirection, kAnyProfileDevice) != nullptr
      || activeAudioDeviceRoute(deviceRoutes, wantDirection, profileDevice) != nullptr) {
    return true;
  }

  const auto matchesDirection = [wantDirection](const PipeWireService::DeviceRouteData& route) {
    return route.direction == wantDirection;
  };
  const auto matchesDevice = [wantDirection, profileDevice](const PipeWireService::DeviceRouteData& route) {
    return route.direction == wantDirection && routeMatchesDevice(route, profileDevice);
  };
  return !std::ranges::any_of(nodeRoutes, matchesDirection) && !std::ranges::any_of(deviceRoutes, matchesDevice);
}

const PipeWireService::DeviceRouteData* audioNodeRoute(
    AudioDeviceRoutes nodeRoutes, AudioDeviceRoutes deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
) {
  const auto best = [wantDirection](AudioDeviceRoutes routes, std::int32_t device) {
    const PipeWireService::DeviceRouteData* found = nullptr;
    for (const auto& route : routes) {
      if (route.index >= 0
          && route.direction == wantDirection
          && routeMatchesDevice(route, device)
          && (found == nullptr || routeIsBetterCandidate(route, *found))) {
        found = &route;
      }
    }
    return found;
  };
  const PipeWireService::DeviceRouteData* nodeRoute = best(nodeRoutes, kAnyProfileDevice);
  return nodeRoute != nullptr ? nodeRoute : best(deviceRoutes, profileDevice);
}

namespace {
  bool headphonesRoute(const PipeWireService::DeviceRouteData& route) {
    return (route.portType == "headphones" || route.portType == "headset") && route.available != SPA_PARAM_AVAILABILITY_no;
  }
} // namespace

bool headphonesUnplugged(AudioDeviceRoutes before, const PipeWireService::DeviceRouteData& now) {
  if (now.direction != SPA_DIRECTION_OUTPUT) {
    return false;
  }
  const auto was = std::ranges::find_if(before, [&now](const PipeWireService::DeviceRouteData& r) {
    return r.direction == SPA_DIRECTION_OUTPUT && r.device == now.device;
  });
  return was != before.end() && headphonesRoute(*was) && !headphonesRoute(now);
}

std::vector<SinkChoice> sinkChoices(const AudioState& state) {
  std::vector<SinkChoice> out;
  for (const AudioNode& n : state.sinks) {
    if (!n.enhances.empty()) {
      continue;
    }
    std::uint32_t pick = n.id;
    for (const AudioNode& e : state.sinks) {
      if (e.enhances == n.name) {
        pick = e.id;
        break;
      }
    }
    const bool current = n.id == state.defaultSinkId || pick == state.defaultSinkId;
    if (n.available || current) {
      out.push_back({.node = &n, .pickId = pick, .current = current});
    }
  }
  return out;
}

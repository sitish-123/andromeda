#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

enum class HotspotState : std::uint8_t { Off, Activating, On, Deactivating };

enum class HotspotEvent : std::uint8_t {
  // Starts the hotspot on a Wi-Fi device; a connected Wi-Fi client on it drops.
  RequestOn,
  // Starts the hotspot on a new access-point interface beside the connected Wi-Fi client.
  RequestOnBesideClient,
  RequestOff,
  ActivateOk,
  ActivateFailed,
  DeactivateOk,
  DeactivateFailed,
  InterfaceCreated,
  InterfaceFailed,
  // NetworkManager lists the new interface and can activate on it.
  DeviceReady,
  // The wait for DeviceReady ran out.
  DeviceTimeout,
  InterfaceRemoved,
  RemoveFailed,
  // The first answer to "how does a hotspot start now": requests wait for it.
  PlanKnown,
  // The Wi-Fi client moved to another channel while the hotspot runs beside it: the hotspot restarts there.
  ClientChannelChanged,
};

// The call the owner starts after the event or observation that returned it.
enum class HotspotWork : std::uint8_t {
  Activate,
  CreateInterface,
  // Wait for DeviceReady, with a timer that sends DeviceTimeout.
  AwaitDevice,
  ActivateOnInterface,
  Deactivate,
  RemoveInterface,
};

// The NetworkManager 802-11-wireless band ("bg" or "a") and channel of an access point.
struct HotspotChannel {
  std::string_view band;
  std::uint32_t channel = 0;
};

// The channel of Wi-Fi frequency `mhz`, so a hotspot can share the client's channel. None for 6 GHz, which
// NetworkManager's band setting cannot name, and for frequencies outside the 2.4 and 5 GHz channel plans.
[[nodiscard]] constexpr std::optional<HotspotChannel> hotspotChannelOf(std::uint32_t mhz) noexcept {
  if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0) {
    return HotspotChannel{"bg", (mhz - 2407) / 5};
  }
  if (mhz == 2484) {
    return HotspotChannel{"bg", 14};
  }
  if (mhz >= 5160 && mhz <= 5885 && mhz % 5 == 0) {
    return HotspotChannel{"a", (mhz - 5000) / 5};
  }
  return std::nullopt;
}

// What one NetworkManager query saw: no hotspot connection, one in NM state Activating (1), or Activated (2).
// OnBesideClient: Activated on an access-point interface named `<client>ap`, which System1 removes on request even
// when an earlier shell or System1 run created it.
enum class HotspotObservation : std::uint8_t { Off, Activating, On, OnBesideClient };

// The hotspot switch: one request in flight at a time, a failed request reverts and keeps its error until the
// next request or an observed change. Activation ends only when NetworkManager reports the connection Activated.
// An access-point interface created for a request, or adopted from an observation, is removed exactly once, when
// the hotspot stops or fails. Requests to turn on wait for PlanKnown.
class HotspotMachine {
public:
  [[nodiscard]] HotspotState state() const noexcept {
    switch (m_phase) {
    case Phase::Off:
      return HotspotState::Off;
    case Phase::Calling:
    case Phase::Starting:
    case Phase::Creating:
    case Phase::AwaitingDevice:
    case Phase::ApCalling:
    case Phase::ApStarting:
    case Phase::ApRestarting:
    case Phase::ApRearming:
      return HotspotState::Activating;
    case Phase::On:
    case Phase::ApOn:
      return HotspotState::On;
    case Phase::Deactivating:
    case Phase::ApDeactivating:
    case Phase::Removing:
      return HotspotState::Deactivating;
    }
    return HotspotState::Off;
  }
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }
  // The target of the last user request; Try Again issues it again.
  [[nodiscard]] bool requestedOn() const noexcept { return m_requestedOn; }
  // False until PlanKnown: the switch is disabled.
  [[nodiscard]] bool planKnown() const noexcept { return m_planKnown; }

  // Issues the token of a new query. Only the latest token issued after the last request or reply counts.
  std::uint64_t beginQuery() noexcept { return ++m_lastQuery; }

  // The work the last accepted event or observation started; returns it once.
  [[nodiscard]] std::optional<HotspotWork> takeWork() noexcept { return std::exchange(m_work, std::nullopt); }

  // Returns false and changes nothing when `event` is not valid in the current state.
  bool apply(HotspotEvent event, std::string_view error = {}) {
    switch (event) {
    case HotspotEvent::RequestOn:
    case HotspotEvent::RequestOnBesideClient:
      if (m_phase != Phase::Off || !m_planKnown) {
        return false;
      }
      m_requestedOn = true;
      return transition(event == HotspotEvent::RequestOn ? Phase::Calling : Phase::Creating, {});
    case HotspotEvent::RequestOff:
      if (m_phase != Phase::On && m_phase != Phase::ApOn) {
        return false;
      }
      m_requestedOn = false;
      return transition(m_phase == Phase::On ? Phase::Deactivating : Phase::ApDeactivating, {});
    case HotspotEvent::ActivateOk:
      return (m_phase == Phase::Calling && transition(Phase::Starting, {}))
             || (m_phase == Phase::ApCalling && transition(Phase::ApStarting, {}));
    case HotspotEvent::ActivateFailed:
      return (m_phase == Phase::Calling && transition(Phase::Off, std::string(error)))
             || (m_phase == Phase::ApCalling && transition(Phase::Removing, std::string(error)));
    case HotspotEvent::DeactivateOk:
      return (m_phase == Phase::Deactivating && transition(Phase::Off, {}))
             || (m_phase == Phase::ApDeactivating && transition(Phase::Removing, {}))
             || (m_phase == Phase::ApRestarting && transition(Phase::ApRearming, {}));
    case HotspotEvent::DeactivateFailed:
      return (m_phase == Phase::Deactivating && transition(Phase::On, std::string(error)))
             || (m_phase == Phase::ApDeactivating && transition(Phase::ApOn, std::string(error)))
             || (m_phase == Phase::ApRestarting && transition(Phase::Removing, std::string(error)));
    case HotspotEvent::InterfaceCreated:
      return (m_phase == Phase::Creating && transition(Phase::AwaitingDevice, {}))
             || (m_phase == Phase::ApRearming && transition(Phase::ApCalling, {}));
    case HotspotEvent::InterfaceFailed:
      return (m_phase == Phase::Creating && transition(Phase::Off, std::string(error)))
             || (m_phase == Phase::ApRearming && transition(Phase::Removing, std::string(error)));
    case HotspotEvent::DeviceReady:
      return m_phase == Phase::AwaitingDevice && transition(Phase::ApCalling, {});
    case HotspotEvent::DeviceTimeout:
      return m_phase == Phase::AwaitingDevice && transition(Phase::Removing, std::string(error));
    case HotspotEvent::InterfaceRemoved:
      return m_phase == Phase::Removing && transition(Phase::Off, m_error);
    case HotspotEvent::RemoveFailed:
      // The failure that started the removal explains more than the removal's own.
      return m_phase == Phase::Removing && transition(Phase::Off, m_error.empty() ? std::string(error) : m_error);
    case HotspotEvent::PlanKnown:
      return !std::exchange(m_planKnown, true);
    case HotspotEvent::ClientChannelChanged:
      return m_phase == Phase::ApOn && transition(Phase::ApRestarting, {});
    }
    return false;
  }

  // Applies the result of query `token`. `error` explains a hotspot that vanished while starting.
  // Returns true when the state or the error changed.
  bool observe(std::uint64_t token, HotspotObservation seen, std::string_view error = {}) {
    if (token != m_lastQuery || token <= m_fence) {
      return false;
    }
    switch (m_phase) {
    case Phase::Calling:
    case Phase::Deactivating:
    case Phase::Creating:
    case Phase::AwaitingDevice:
    case Phase::ApCalling:
    case Phase::ApDeactivating:
    case Phase::ApRestarting:
    case Phase::ApRearming:
    case Phase::Removing:
      return false;
    case Phase::Starting:
      if (seen == HotspotObservation::On || seen == HotspotObservation::OnBesideClient) {
        return settle(Phase::On, {});
      }
      return seen == HotspotObservation::Off && settle(Phase::Off, std::string(error));
    case Phase::ApStarting:
      if (seen == HotspotObservation::On || seen == HotspotObservation::OnBesideClient) {
        return settle(Phase::ApOn, {});
      }
      return seen == HotspotObservation::Off && settle(Phase::Removing, std::string(error));
    case Phase::ApOn:
      return seen == HotspotObservation::Off && settle(Phase::Removing, {});
    case Phase::Off:
    case Phase::On: {
      const Phase next = seen == HotspotObservation::Off              ? Phase::Off
                         : seen == HotspotObservation::OnBesideClient ? Phase::ApOn
                                                                      : Phase::On;
      return next != m_phase && settle(next, {});
    }
    }
    return false;
  }

private:
  // Calling: the Activate call is in flight. Starting: NM accepted it; waiting for the connection to come up.
  // Ap*: the same on an access-point interface this machine created; Creating and Removing are its System1 calls.
  // ApRestarting: deactivating to start again on the client's new channel. ApRearming: CreateApInterface again,
  // because NetworkManager resets the interface type to managed when it deactivates.
  enum class Phase : std::uint8_t {
    Off,
    Calling,
    Starting,
    On,
    Deactivating,
    Creating,
    AwaitingDevice,
    ApCalling,
    ApStarting,
    ApOn,
    ApDeactivating,
    ApRestarting,
    ApRearming,
    Removing,
  };

  static std::optional<HotspotWork> workOf(Phase phase) noexcept {
    switch (phase) {
    case Phase::Calling:
      return HotspotWork::Activate;
    case Phase::Creating:
    case Phase::ApRearming:
      return HotspotWork::CreateInterface;
    case Phase::AwaitingDevice:
      return HotspotWork::AwaitDevice;
    case Phase::ApCalling:
      return HotspotWork::ActivateOnInterface;
    case Phase::Deactivating:
    case Phase::ApDeactivating:
    case Phase::ApRestarting:
      return HotspotWork::Deactivate;
    case Phase::Removing:
      return HotspotWork::RemoveInterface;
    case Phase::Off:
    case Phase::Starting:
    case Phase::On:
    case Phase::ApStarting:
    case Phase::ApOn:
      return std::nullopt;
    }
    return std::nullopt;
  }

  // A request or reply makes every query in flight stale.
  bool transition(Phase next, std::string error) {
    m_fence = m_lastQuery;
    return settle(next, std::move(error));
  }

  bool settle(Phase next, std::string error) {
    m_phase = next;
    m_error = std::move(error);
    m_work = workOf(next);
    return true;
  }

  Phase m_phase = Phase::Off;
  std::string m_error;
  bool m_requestedOn = false;
  bool m_planKnown = false;
  std::uint64_t m_lastQuery = 0;
  std::uint64_t m_fence = 0;
  std::optional<HotspotWork> m_work;
};

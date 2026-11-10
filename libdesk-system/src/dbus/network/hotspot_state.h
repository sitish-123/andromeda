#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

enum class HotspotState : std::uint8_t { Off, Activating, On, Deactivating };

enum class HotspotEvent : std::uint8_t {
  RequestOn,
  RequestOff,
  ActivateOk,
  ActivateFailed,
  DeactivateOk,
  DeactivateFailed,
};

// What one NetworkManager query saw: no hotspot connection, one in NM state Activating (1), or Activated (2).
enum class HotspotObservation : std::uint8_t { Off, Activating, On };

// The hotspot switch: one request in flight at a time, a failed request reverts and keeps its error until the
// next request or an observed change. Activation ends only when NetworkManager reports the connection Activated.
class HotspotMachine {
public:
  [[nodiscard]] HotspotState state() const noexcept {
    switch (m_phase) {
    case Phase::Off:
      return HotspotState::Off;
    case Phase::Calling:
    case Phase::Starting:
      return HotspotState::Activating;
    case Phase::On:
      return HotspotState::On;
    case Phase::Deactivating:
      return HotspotState::Deactivating;
    }
    return HotspotState::Off;
  }
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }
  // The target of the last user request; Try Again issues it again.
  [[nodiscard]] bool requestedOn() const noexcept { return m_requestedOn; }

  // Issues the token of a new query. Only the latest token issued after the last request or reply counts.
  std::uint64_t beginQuery() noexcept { return ++m_lastQuery; }

  // Returns false and changes nothing when `event` is not valid in the current state.
  bool apply(HotspotEvent event, std::string_view error = {}) {
    switch (event) {
    case HotspotEvent::RequestOn:
      if (m_phase != Phase::Off) {
        return false;
      }
      m_requestedOn = true;
      return transition(Phase::Calling, {});
    case HotspotEvent::RequestOff:
      if (m_phase != Phase::On) {
        return false;
      }
      m_requestedOn = false;
      return transition(Phase::Deactivating, {});
    case HotspotEvent::ActivateOk:
      return m_phase == Phase::Calling && transition(Phase::Starting, {});
    case HotspotEvent::ActivateFailed:
      return m_phase == Phase::Calling && transition(Phase::Off, std::string(error));
    case HotspotEvent::DeactivateOk:
      return m_phase == Phase::Deactivating && transition(Phase::Off, {});
    case HotspotEvent::DeactivateFailed:
      return m_phase == Phase::Deactivating && transition(Phase::On, std::string(error));
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
      return false;
    case Phase::Starting:
      if (seen == HotspotObservation::On) {
        return settle(Phase::On, {});
      }
      return seen == HotspotObservation::Off && settle(Phase::Off, std::string(error));
    case Phase::Off:
    case Phase::On: {
      const Phase next = seen == HotspotObservation::Off ? Phase::Off : Phase::On;
      return next != m_phase && settle(next, {});
    }
    }
    return false;
  }

private:
  // Calling: the Activate call is in flight. Starting: NM accepted it; waiting for the connection to come up.
  enum class Phase : std::uint8_t { Off, Calling, Starting, On, Deactivating };

  // A request or reply makes every query in flight stale.
  bool transition(Phase next, std::string error) {
    m_fence = m_lastQuery;
    return settle(next, std::move(error));
  }

  bool settle(Phase next, std::string error) {
    m_phase = next;
    m_error = std::move(error);
    return true;
  }

  Phase m_phase = Phase::Off;
  std::string m_error;
  bool m_requestedOn = false;
  std::uint64_t m_lastQuery = 0;
  std::uint64_t m_fence = 0;
};

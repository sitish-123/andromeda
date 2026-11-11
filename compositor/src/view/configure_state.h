#pragma once

#include <cstdint>
#include <optional>

namespace andromeda {

  // Where a window is with the configures the compositor sends it. Queued: scheduled, not sent yet. AwaitCommit: sent,
  // not yet acknowledged and committed (wlroots applies an ack only on commit, so the ack alone is no separate state).
  // Suspended: one is owed, but the window is suspended and already shows the scheduled size; it gets no frame
  // callbacks, so it may never commit a configure that only suspends it.
  enum class ConfigureState : uint8_t { Unmapped, Idle, Queued, AwaitCommit, Suspended, Destroyed };

  // Scheduled: the compositor changed the state it asks for. Sent: wlroots sent the queued configure, also one that
  // another module scheduled (an xdg-decoration mode).
  enum class ConfigureEvent : uint8_t { Map, Scheduled, Sent, Commit, Unmap, Destroy };

  // What wlroots reports right after the event.
  struct ConfigureFacts {
    bool queued = false;
    bool acked = true;
    bool excused = false;
  };

  constexpr bool configureSettled(ConfigureState state) {
    return state != ConfigureState::Queued && state != ConfigureState::AwaitCommit;
  }

  constexpr bool configureOwed(ConfigureState state) {
    return state == ConfigureState::Queued || state == ConfigureState::AwaitCommit || state == ConfigureState::Suspended;
  }

  // The one transition function; nullopt rejects the event in that state.
  constexpr std::optional<ConfigureState> configureAfter(
      ConfigureState state, ConfigureEvent event, ConfigureFacts facts
  ) {
    if (state == ConfigureState::Destroyed) {
      return std::nullopt;
    }
    const bool mapped = state != ConfigureState::Unmapped;
    switch (event) {
      case ConfigureEvent::Destroy:
        return ConfigureState::Destroyed;
      case ConfigureEvent::Unmap:
        return mapped ? std::optional(ConfigureState::Unmapped) : std::nullopt;
      case ConfigureEvent::Map:
        if (mapped) {
          return std::nullopt;
        }
        break;
      case ConfigureEvent::Scheduled:
      case ConfigureEvent::Sent:
      case ConfigureEvent::Commit:
        if (!mapped) {
          return ConfigureState::Unmapped;
        }
        break;
    }
    if (facts.acked && !facts.queued) {
      return ConfigureState::Idle;
    }
    if (facts.excused) {
      return ConfigureState::Suspended;
    }
    return facts.queued ? ConfigureState::Queued : ConfigureState::AwaitCommit;
  }

  // Applies one event; `onSettled` runs once on each entry into a settled state. False keeps the state.
  template <typename OnSettled>
  constexpr bool applyConfigureEvent(
      ConfigureState& state, ConfigureEvent event, ConfigureFacts facts, OnSettled&& onSettled
  ) {
    const std::optional<ConfigureState> next = configureAfter(state, event, facts);
    if (!next) {
      return false;
    }
    const bool entered = !configureSettled(state) && configureSettled(*next);
    state = *next;
    if (entered) {
      onSettled();
    }
    return true;
  }

} // namespace andromeda

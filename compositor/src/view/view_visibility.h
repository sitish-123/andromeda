#pragma once

#include <cstdint>
#include <optional>

namespace andromeda {

  // Where a window is in its life: not mapped, mapped but hidden until the arrange that places a tiled opener, or mapped.
  enum class Lifecycle : uint8_t { Unmapped, OpeningDeferred, Mapped };

  enum class LifecycleEvent : uint8_t { Map, DeferOpening, ResumeOpening, Unmap };

  // The one transition function; nullopt rejects the event in that state.
  constexpr std::optional<Lifecycle> lifecycleAfter(Lifecycle state, LifecycleEvent event) {
    switch (event) {
      case LifecycleEvent::Map:
        return state == Lifecycle::Unmapped ? std::optional(Lifecycle::Mapped) : std::nullopt;
      case LifecycleEvent::DeferOpening:
        return state == Lifecycle::Mapped ? std::optional(Lifecycle::OpeningDeferred) : std::nullopt;
      case LifecycleEvent::ResumeOpening:
        return state == Lifecycle::OpeningDeferred ? std::optional(Lifecycle::Mapped) : std::nullopt;
      case LifecycleEvent::Unmap:
        return state != Lifecycle::Unmapped ? std::optional(Lifecycle::Unmapped) : std::nullopt;
    }
    return std::nullopt;
  }

  // The one rule for whether a window's frame draws. `placeShows` is what its place says: its workspace is active, a
  // slide or drag presents it, or its scratchpad shows.
  struct WindowVisibility {
    Lifecycle lifecycle = Lifecycle::Unmapped;
    bool tabHidden = false;
    bool minimized = false;
    bool placeShows = false;

    // True while nothing but the place can hide the frame.
    [[nodiscard]] constexpr bool presentable() const {
      return lifecycle == Lifecycle::Mapped && !tabHidden && !minimized;
    }
    [[nodiscard]] constexpr bool shown() const { return placeShows && presentable(); }
  };

} // namespace andromeda

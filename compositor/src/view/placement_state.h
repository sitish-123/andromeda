#pragma once

#include "view/snap.h"

#include <cstdint>
#include <optional>

namespace andromeda {

  // How a window takes part in its workspace. LayoutFloated: floating only because its workspace floats every window,
  // so it tiles again in a tiling layout. Pinned: floating above every workspace of its output.
  enum class Arrangement : uint8_t { Tiled, Floating, LayoutFloated, Pinned };

  // How a window fills its area. A tile is only Free or EdgeMaximized; a floating EdgeMaximized window is also
  // maximized for the client.
  enum class Fit : uint8_t { Free, FloatMaximized, EdgeMaximized };

  // What a later transition brings back. Fullscreen: a float toggle dropped the tile's fullscreen, so re-tiling restores
  // it. Pinned, EdgeMaximized, PinnedEdgeMaximized: fullscreen covers that state, so leaving fullscreen restores it.
  enum class RestoreTo : uint8_t { None, Fullscreen, Pinned, EdgeMaximized, PinnedEdgeMaximized };

  struct Placement {
    Arrangement arrangement = Arrangement::Floating;
    Fit fit = Fit::Free;
    RestoreTo restoreTo = RestoreTo::None;
    // Where unpinning goes (Tiled, Floating or LayoutFloated) while the window is pinned or fullscreen covers its pin;
    // Floating otherwise.
    Arrangement unpinTo = Arrangement::Floating;
    // Where a floating window rests snapped; a maximize covers it and unmaximizing returns there.
    std::optional<SnapSpot> snap{};

    bool operator==(const Placement&) const = default;
  };

  constexpr bool restoresPin(RestoreTo restore) {
    return restore == RestoreTo::Pinned || restore == RestoreTo::PinnedEdgeMaximized;
  }

  constexpr bool restoresEdges(RestoreTo restore) {
    return restore == RestoreTo::EdgeMaximized || restore == RestoreTo::PinnedEdgeMaximized;
  }

  constexpr bool placementFloating(Arrangement arrangement) { return arrangement != Arrangement::Tiled; }

  // Map: the window maps tiled or floating. Float, Tile: the float toggle moves a tile out of or into the layout.
  // RememberFullscreen: the float toggle dropped the tile's fullscreen. LayoutFloat: a floating layout takes the window.
  // ClearLayoutFloat: the user floats it on purpose. Pin: from where it floats now; PinTile: a tile the float toggle just
  // floated. EnterFullscreen covers a pin and `edges`; ExitFullscreen drops a pending re-tile fullscreen; DropEdgeRestore
  // and RestorePin consume what fullscreen covered. DropMaximize: a resize or snap leaves either maximize. Attach: the
  // window becomes a modal dialog of another and has no placement of its own.
  enum class PlacementEvent : uint8_t {
    Map,
    Float,
    Tile,
    RememberFullscreen,
    LayoutFloat,
    ClearLayoutFloat,
    Pin,
    PinTile,
    Unpin,
    EnterFullscreen,
    ExitFullscreen,
    DropEdgeRestore,
    RestorePin,
    FloatMaximize,
    FloatUnmaximize,
    EdgeMaximize,
    EdgeUnmaximize,
    DropMaximize,
    Snap,
    Unsnap,
    Attach,
    Unmap,
  };

  struct PlacementInput {
    PlacementEvent event;
    // Map: maps as a tile. EnterFullscreen: fullscreen covers an edge maximize.
    bool flag = false;
    // Snap: the spot.
    SnapSpot spot{};
  };

  constexpr RestoreTo withoutEdges(RestoreTo restore) {
    switch (restore) {
      case RestoreTo::EdgeMaximized:
        return RestoreTo::None;
      case RestoreTo::PinnedEdgeMaximized:
        return RestoreTo::Pinned;
      default:
        return restore;
    }
  }

  constexpr RestoreTo withoutPin(RestoreTo restore) {
    switch (restore) {
      case RestoreTo::Pinned:
        return RestoreTo::None;
      case RestoreTo::PinnedEdgeMaximized:
        return RestoreTo::EdgeMaximized;
      default:
        return restore;
    }
  }

  // The one transition function; nullopt rejects the event in that placement.
  constexpr std::optional<Placement> placementAfter(const Placement& state, const PlacementInput& input) {
    Placement next = state;
    const bool floating = placementFloating(state.arrangement);
    const bool pinned = state.arrangement == Arrangement::Pinned;
    switch (input.event) {
      case PlacementEvent::Map:
        return Placement{.arrangement = input.flag ? Arrangement::Tiled : Arrangement::Floating};
      case PlacementEvent::Attach:
        return Placement{};
      case PlacementEvent::Unmap:
        return Placement{.arrangement = floating ? Arrangement::Floating : Arrangement::Tiled};
      case PlacementEvent::Float:
        if (floating) {
          return std::nullopt;
        }
        next.arrangement = Arrangement::Floating;
        return next;
      case PlacementEvent::Tile:
        if (state.arrangement != Arrangement::Floating && state.arrangement != Arrangement::LayoutFloated) {
          return std::nullopt;
        }
        next.fit = Fit::Free;
        next.snap.reset();
        next.arrangement = Arrangement::Tiled;
        next.unpinTo = Arrangement::Floating;
        next.restoreTo = withoutPin(state.restoreTo == RestoreTo::Fullscreen ? RestoreTo::None : state.restoreTo);
        return next;
      case PlacementEvent::RememberFullscreen:
        if (floating) {
          return std::nullopt;
        }
        next.restoreTo = RestoreTo::Fullscreen;
        return next;
      case PlacementEvent::LayoutFloat:
        if (!pinned) {
          next.arrangement = Arrangement::LayoutFloated;
        }
        if ((pinned || restoresPin(state.restoreTo)) && state.unpinTo == Arrangement::Floating) {
          next.unpinTo = Arrangement::LayoutFloated;
        }
        return next;
      case PlacementEvent::ClearLayoutFloat:
        if (state.arrangement == Arrangement::LayoutFloated) {
          next.arrangement = Arrangement::Floating;
        }
        if (state.unpinTo == Arrangement::LayoutFloated) {
          next.unpinTo = Arrangement::Floating;
        }
        return next;
      case PlacementEvent::Pin:
      case PlacementEvent::PinTile:
        if (!floating || pinned || restoresPin(state.restoreTo)) {
          return std::nullopt;
        }
        if (input.event == PlacementEvent::PinTile && state.arrangement != Arrangement::Floating) {
          return std::nullopt;
        }
        next.unpinTo = input.event == PlacementEvent::PinTile ? Arrangement::Tiled : state.arrangement;
        next.arrangement = Arrangement::Pinned;
        return next;
      case PlacementEvent::Unpin:
        if (!pinned) {
          return std::nullopt;
        }
        next.arrangement =
            state.unpinTo == Arrangement::LayoutFloated ? Arrangement::LayoutFloated : Arrangement::Floating;
        next.unpinTo = Arrangement::Floating;
        return next;
      case PlacementEvent::EnterFullscreen: {
        const bool pin = pinned || restoresPin(state.restoreTo);
        const bool edges = input.flag || restoresEdges(state.restoreTo);
        if (pinned) {
          next.arrangement =
              state.unpinTo == Arrangement::LayoutFloated ? Arrangement::LayoutFloated : Arrangement::Floating;
        }
        next.restoreTo = pin ? (edges ? RestoreTo::PinnedEdgeMaximized : RestoreTo::Pinned)
                             : (edges ? RestoreTo::EdgeMaximized : RestoreTo::None);
        return next;
      }
      case PlacementEvent::ExitFullscreen:
        if (state.restoreTo == RestoreTo::Fullscreen) {
          next.restoreTo = RestoreTo::None;
        }
        return next;
      case PlacementEvent::DropEdgeRestore:
        next.restoreTo = withoutEdges(state.restoreTo);
        return next;
      case PlacementEvent::RestorePin:
        if (!restoresPin(state.restoreTo) || !floating || pinned) {
          return std::nullopt;
        }
        next.arrangement = Arrangement::Pinned;
        next.restoreTo = withoutPin(state.restoreTo);
        return next;
      case PlacementEvent::FloatMaximize:
        if (!floating) {
          return std::nullopt;
        }
        if (state.fit == Fit::Free) {
          next.fit = Fit::FloatMaximized;
        }
        return next;
      case PlacementEvent::FloatUnmaximize:
        if (!floating || state.fit == Fit::EdgeMaximized) {
          return std::nullopt;
        }
        next.fit = Fit::Free;
        return next;
      case PlacementEvent::EdgeMaximize:
        next.fit = Fit::EdgeMaximized;
        return next;
      case PlacementEvent::EdgeUnmaximize:
      case PlacementEvent::DropMaximize:
        if (input.event == PlacementEvent::DropMaximize) {
          next.restoreTo = withoutEdges(state.restoreTo);
        } else if (state.fit != Fit::EdgeMaximized) {
          return next;
        }
        next.fit = Fit::Free;
        return next;
      case PlacementEvent::Snap:
        if (!floating || pinned || state.fit != Fit::Free) {
          return std::nullopt;
        }
        next.snap = input.spot;
        return next;
      case PlacementEvent::Unsnap:
        next.snap.reset();
        return next;
    }
    return std::nullopt;
  }

  // Applies one event; false keeps the placement.
  constexpr bool applyPlacementEvent(Placement& state, const PlacementInput& input) {
    const std::optional<Placement> next = placementAfter(state, input);
    if (!next) {
      return false;
    }
    state = *next;
    return true;
  }

} // namespace andromeda

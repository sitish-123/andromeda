#pragma once

#include "overview/arrange.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Where everything sits when the overview is fully open, per style. Pure: the controller lerps each window from its
// desktop rect to the rect here by the open progress. Coordinates are output-local layout pixels.
namespace andromeda::ov {

  // `overview.style`. Spaces = workspace thumbs on top, current workspace below (macOS Mission Control). TaskView =
  // the current workspace's windows spread out, desktops along the bottom (Windows 11). Strip = one zoomed row per
  // workspace stacked vertically (niri). Grid = every workspace as an equal tile (KDE desktop grid, hyprexpo).
  enum class Style : uint8_t { Spaces, TaskView, Strip, Grid };

  [[nodiscard]] std::string_view styleName(Style style);
  [[nodiscard]] std::optional<Style> styleNamed(std::string_view name);

  // How a workspace arranges its windows, as far as placement cares: Free spreads out (exposé), Scrolling shows its
  // whole strip, Tiled (Split, Main + Stack) scales its real boxes.
  enum class Arrangement : uint8_t { Free, Scrolling, Tiled };

  struct Member {
    uint32_t id = 0;
    // The rect the layout gives the window on its own workspace, output-local; a scrolled-away column lies partly or
    // wholly outside the output. A minimized window keeps its last rect here.
    Rect desk;
    bool minimized = false;
  };

  struct Workspace {
    Arrangement arrangement = Arrangement::Scrolling;
    std::vector<Member> members;
  };

  struct Input {
    double width = 0;
    double height = 0;
    Rect usable; // the work area (output minus bars and docks)
    std::vector<Workspace> workspaces;
    size_t current = 0;
    // Strip only: the row the column is centred on, fractional while it scrolls. Unset = `current`.
    std::optional<double> stripRow;
  };

  struct Insets {
    double top = 0;
    double right = 0;
    double bottom = 0;
    double left = 0;
    bool operator==(const Insets&) const = default;
  };

  struct WindowPlace {
    uint32_t id = 0;
    size_t workspace = 0;
    Rect rect;
    double opacity = 1;
    bool dimmed = false;        // off screen in its strip; drawn at half strength
    std::optional<Insets> clip; // in desk-rect pixels: the part outside the output, cut away
  };

  enum class CardKind : uint8_t { Thumb, Frame, Tile };

  struct Card {
    size_t workspace = 0;
    Rect rect;
    CardKind kind = CardKind::Thumb;
  };

  struct Point {
    double x = 0;
    double y = 0;
  };

  struct Placement {
    std::vector<WindowPlace> windows;
    std::vector<Card> cards;
    std::optional<Rect> plus;     // the new-workspace target
    std::optional<Rect> main;     // the area the current workspace fills (Spaces, Task View)
    std::optional<Point> title;   // workspace name + layout chip, top-left corner
    std::optional<Point> filter;  // filter field, top-left corner; kFilterWidth wide
    std::optional<Rect> viewport; // Spaces + Scrolling: the frame marking what is on screen
  };

  inline constexpr double kFilterWidth = 260;
  inline constexpr double kMargin = 56;

  [[nodiscard]] Placement place(Style style, const Input& input);
  [[nodiscard]] const WindowPlace* findWindow(const Placement& placement, uint32_t id);

} // namespace andromeda::ov

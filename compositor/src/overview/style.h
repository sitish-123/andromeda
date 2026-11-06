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

    bool operator==(const Member&) const = default;
  };

  struct Workspace {
    Arrangement arrangement = Arrangement::Scrolling;
    std::vector<Member> members;

    bool operator==(const Workspace&) const = default;
  };

  struct Input {
    double width = 0;
    double height = 0;
    Rect usable; // the work area (output minus bars and docks)
    std::vector<Workspace> workspaces;
    size_t current = 0;
    // Strip only: the row the column is centred on, fractional while it scrolls. Unset = `current`.
    std::optional<double> stripRow;
    // Strip only: each row's share of the output (`overview.zoom`), and whether rows step along x (the output's
    // workspace axis is horizontal) instead of y.
    double stripScale = 0.5;
    bool horizontal = false;

    bool operator==(const Input&) const = default;
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
    std::optional<Rect> plus; // the new-workspace target
    std::optional<Rect> main; // the area the current workspace fills (Spaces, Task View)
    // Spaces, Task View: the current workspace's desktop -> the main area. A spread (exposé) workspace has no single
    // map, so it is the whole output fitted into the area.
    std::optional<Fit> mainFit;
    std::optional<Point> title;   // workspace name + layout chip, top-left corner
    std::optional<Point> filter;  // filter field, top-left corner; kFilterWidth wide
    std::optional<Rect> viewport; // Spaces + Scrolling: the frame marking what is on screen
    // Spaces, Task View: the current workspace's shown windows on its desktop, cut to the output, bottom to top. Its
    // windows sit in the main area, so its thumb draws these as blocks.
    std::vector<Rect> currentDesk;
  };

  inline constexpr double kFilterWidth = 260;
  inline constexpr double kMargin = 56;

  [[nodiscard]] Placement place(Style style, const Input& input);
  [[nodiscard]] const WindowPlace* findWindow(const Placement& placement, uint32_t id);

  // The placement while the chosen workspace is `row`, fractional while the wheel, a key or a swipe moves it. Strip
  // scrolls its column; Spaces and Task View morph between the two neighbouring workspaces' placements; Grid only
  // marks a tile. `input.current` and `input.stripRow` are ignored.
  [[nodiscard]] Placement placeAt(Style style, Input input, double row);

  // Where a window of workspace `workspace` sits on the desktop while the workspaces are scrolled to `row`: its own
  // rect, one output away per workspace along the workspace axis. Open progress 0 starts here.
  [[nodiscard]] Rect deskRect(const Input& input, const Rect& desk, size_t workspace, double row);
  // Where a workspace card starts at open progress 0: thumbs slide in from past the nearest edge, frames and tiles
  // grow from the full output one workspace step away.
  [[nodiscard]] Rect cardDeskRect(Style style, const Input& input, const Card& card, double row);
  // A window's opacity at open progress `p` in [0, 1]: a minimized window fades in from 0, a window that the
  // placement hides fades out early, an off-screen strip column dims to half.
  [[nodiscard]] double windowOpacity(const WindowPlace& place, bool minimized, double p);

} // namespace andromeda::ov

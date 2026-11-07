#pragma once

#include "overview/style.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wlr/util/box.h>

struct wlr_scene_border;
struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_scene_tree;

namespace andromeda::ov {

  // One workspace as the chrome labels it: its name, the arrangement chip text and its drawn card.
  struct ChromeWorkspace {
    std::string_view name;
    std::string_view arrangement;
    wlr_box card{}; // layout coordinates, as drawn this frame
    CardKind kind = CardKind::Thumb;
    bool shown = false;
  };

  // One window card as drawn this frame.
  struct ChromeWindow {
    const void* key = nullptr;
    std::string_view title;
    wlr_box box{}; // the visible part of the card, layout coordinates
    // The workspace card holding it, which its capsule stays inside; empty for the main area's windows, whose capsule
    // sits under the card down to the usable bottom.
    std::optional<wlr_box> within;
    float opacity = 1.0F;
    bool selected = false;
    bool dragged = false;
  };

  // Rows of the arrangement menu, top to bottom.
  inline constexpr int kMenuRows = 4; // Free, Scrolling, Split, Main + Stack

  // The arrangement menu, open under a workspace's chip. Rows are indices into kMenuRows; -1 is none.
  struct ChromeMenu {
    size_t workspace = 0;
    int hovered = -1;
    int current = -1;
  };

  // The window menu, open at the pointer over a window card (right-click). Rows from `dangerFrom` on are destructive.
  struct ChromeWindowMenu {
    int x = 0; // layout coordinates of the press
    int y = 0;
    std::span<const std::string> rows;
    int hovered = -1;
    int dangerFrom = -1;
  };

  struct ChromeFrame {
    Style style = Style::Spaces;
    double progress = 0;
    wlr_box output{}; // layout coordinates
    wlr_box usable{}; // layout coordinates
    double scale = 1; // device-pixel scale text renders at
    const Placement* placement = nullptr;
    size_t current = 0; // the chosen workspace
    std::span<const ChromeWorkspace> workspaces;
    std::span<const ChromeWindow> windows;
    bool titlesAlways = true;
    std::string_view filter;
    const void* hovered = nullptr; // the window card under the pointer; it shows its title and close button
    bool closeHovered = false;     // the pointer is on that close button
    std::optional<ChromeMenu> menu;
    std::optional<ChromeWindowMenu> windowMenu;
    std::optional<std::string_view> renaming; // the title row's name being typed (F2), drawn as a text field
  };

  // The overview's labels and targets on one output: workspace names, the title row (name + layout chip), the filter
  // field, the new-workspace target, the On screen frame and the window title capsules. Text renders once
  // per change of text or scale; every frame after that only moves and fades nodes. The nodes live under `parent`
  // and are freed by its scene-tree teardown, never by this class.
  class Chrome {
  public:
    explicit Chrome(wlr_scene_tree* parent);
    Chrome(const Chrome&) = delete;
    Chrome& operator=(const Chrome&) = delete;

    void update(const ChromeFrame& frame);
    // Keeps the chrome above cards created after it.
    void raise();
    // The window whose close button holds the layout point, as drawn by the last update; null when none does.
    [[nodiscard]] const void* closeAt(double lx, double ly) const;
    // The workspace whose arrangement chip holds the layout point, as drawn by the last update.
    [[nodiscard]] std::optional<size_t> chipAt(double lx, double ly) const;
    // The open menu's row under the point, or -1; menuContains covers its whole panel.
    [[nodiscard]] int menuRowAt(double lx, double ly) const;
    [[nodiscard]] bool menuContains(double lx, double ly) const;
    // The same for the window menu.
    [[nodiscard]] int windowMenuRowAt(double lx, double ly) const;
    [[nodiscard]] bool windowMenuContains(double lx, double ly) const;
    // Strip: the tab under the point, a workspace index; the workspace count is the + tab.
    [[nodiscard]] std::optional<size_t> tabAt(double lx, double ly) const;
    // True when the point is on the title row's workspace name.
    [[nodiscard]] bool titleAt(double lx, double ly) const;

  private:
    // Text over an optional rounded capsule.
    struct Label {
      wlr_scene_tree* tree = nullptr;
      wlr_scene_rect* fill = nullptr;
      wlr_scene_buffer* text = nullptr;
      std::string key; // markup + font + scale + width the buffer was rendered for
      int width = 0;   // capsule (or text) size
      int height = 0;
      int textWidth = 0;
      int textHeight = 0;
      std::array<float, 4> color{};
      float alpha = -1.0F;
      bool used = false;
    };
    struct LabelStyle {
      std::string_view font;
      int padX = 0;
      int height = 0; // 0: the text's own height, no capsule
      int maxWidth = 400;
      std::array<float, 4> fill{};
      bool wrap = false; // wrap at maxWidth instead of one ellipsized line
    };
    // A rounded rect with a ring, sized only when its box changes.
    struct Frame {
      wlr_scene_tree* tree = nullptr;
      wlr_scene_rect* fill = nullptr;
      wlr_scene_border* ring = nullptr;
      wlr_box box{};
      float alpha = -1.0F;
    };

    void setLabel(Label& label, const std::string& markup, const LabelStyle& style, double scale);
    void placeLabel(Label& label, int x, int y, float alpha);
    void ensureFrame(Frame& frame);
    void placeFrame(
        Frame& frame, const wlr_box& box, int radius, int ringWidth, const std::array<float, 4>& fill,
        const std::array<float, 4>& ring, float alpha
    );

    wlr_scene_tree* m_tree = nullptr;
    std::vector<Label> m_names;
    Label m_title;
    Label m_chip;
    Label m_filter;
    Label m_plusMark;
    Frame m_plus;
    Frame m_viewport;
    Frame m_current; // accent ring around the shown workspace's thumb, row or tile
    std::vector<Frame> m_minis; // the shown workspace's windows as blocks in its thumb
    Label m_viewportTag;
    std::unordered_map<const void*, Label> m_titles;
    Label m_close;
    Label m_closeHot; // the same button under the pointer: red, white mark
    const void* m_closeKey = nullptr;
    wlr_box m_closeBox{};
    std::vector<std::pair<size_t, wlr_box>> m_chips; // workspace index, chip box
    Frame m_menu;
    Frame m_menuHover;
    Label m_menuHead;
    std::array<Label, kMenuRows> m_menuLabels;
    std::array<std::array<Frame, 4>, kMenuRows> m_menuArt; // each row's drawing, up to four blocks
    std::array<wlr_box, kMenuRows> m_menuRowBoxes{};
    std::optional<wlr_box> m_menuBox;
    Frame m_windowMenu;
    Frame m_windowMenuHover;
    std::vector<Label> m_windowMenuLabels;
    std::vector<wlr_box> m_windowMenuRows;
    std::optional<wlr_box> m_windowMenuBox;
    std::optional<wlr_box> m_titleBox;
    Frame m_titleRing; // accent ring around the name while it is renamed
    Frame m_tabBar;     // Strip: one tab per workspace and a + tab, in the dock's place (mockup B)
    Frame m_tabCurrent;
    std::vector<Label> m_tabs;
    std::vector<std::pair<size_t, wlr_box>> m_tabBoxes;
  };

} // namespace andromeda::ov

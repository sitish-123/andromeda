#pragma once

#include "core/files/directory_watcher.h"
#include "core/files/file_model.h"
#include "render/animation/animation_manager.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Box;
class ConfigService;
class Input;
class InputArea;
class ContextMenuPopup;
class LayerSurface;
class Node;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;

// Desktop icons (DL3, .agent/DESIGN.md "Desktop icons", mockup .agent/mockups/desktop-icons.html): ~/Desktop on the
// wallpaper of the first output, on a bottom-layer surface over the usable area. Icons sit on a 96 x 104 grid 16 from
// the edge, in columns from the layout's corner, folders first. Click selects (Ctrl toggles, Shift extends), a drag on
// empty space selects by rubber band, double-click or Enter opens, Delete moves to Trash, right-click opens the
// desktop or item menu.
class DesktopIcons {
public:
  DesktopIcons();
  ~DesktopIcons();
  DesktopIcons(const DesktopIcons&) = delete;
  DesktopIcons& operator=(const DesktopIcons&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
                  ThumbnailService& thumbnails);
  // Follows desktop_icons.enabled and the output set; safe to call again.
  void sync();
  [[nodiscard]] DirectoryWatcher& watcher() noexcept { return m_watcher; }
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const;
  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

private:
  struct Slot {
    int column = 0;
    int row = 0;
    bool operator==(const Slot&) const = default;
  };
  struct Cell {
    std::shared_ptr<const FileInfo> info;
    Slot slot;
    float x = 0.0F;
    float y = 0.0F;
    InputArea* area = nullptr;
    Box* plate = nullptr;
  };
  void open();
  void close();
  void rebuildCells();
  void requestBuild();
  void build();
  void selectOnly(std::size_t index);
  void onCellPress(std::size_t index, bool rightButton);
  void activate(const std::vector<std::size_t>& indices);
  void trash(const std::vector<std::size_t>& indices);
  // Copies dropped files (a text/uri-list) into ~/Desktop.
  void copyIn(const std::string& uriList);
  void openDesktopMenu(float x, float y);
  void openItemMenu(std::size_t index, float x, float y);
  void updateBand(float x, float y);
  void onCellMotion(float sceneX, float sceneY);
  void drop();
  [[nodiscard]] float slotX(int column) const;
  [[nodiscard]] float slotY(int row) const;
  void loadSlots();
  void saveSlots() const;
  [[nodiscard]] std::filesystem::path slotsFile() const;
  [[nodiscard]] std::string outputKey() const;
  [[nodiscard]] std::vector<std::size_t> selection() const;
  [[nodiscard]] std::string iconPathFor(const FileInfo& info);
  [[nodiscard]] std::filesystem::path pathOf(const FileInfo& info) const;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  ThumbnailService::Subscription m_thumbnailUploads;
  // Files with a thumbnail acquire() held, released when they leave the desktop or the surface closes.
  std::set<std::string> m_thumbPaths;
  void releaseThumbnails(const std::set<std::string>& keep);
  DirectoryWatcher m_watcher;
  IconResolver m_iconResolver;
  std::filesystem::path m_dir;
  std::unique_ptr<FileModelView> m_view;
  wl_output* m_output = nullptr;
  std::unique_ptr<LayerSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  InputDispatcher m_input;
  std::unique_ptr<ContextMenuPopup> m_menu;
  std::vector<Cell> m_cells;
  std::set<std::string> m_selected; // by file name, so a re-list keeps the selection
  std::optional<std::size_t> m_anchor; // Shift extends from here
  std::optional<std::size_t> m_lastClick;
  std::chrono::steady_clock::time_point m_lastClickAt;
  bool m_banding = false;
  Box* m_band = nullptr;
  void syncBand();
  float m_bandX0 = 0.0F;
  float m_bandY0 = 0.0F;
  float m_bandX1 = 0.0F;
  float m_bandY1 = 0.0F;
  bool m_dirty = false;
  int m_cellW = 96;
  int m_cellH = 104;
  int m_plate = 72;
  int m_nameW = 92;
  // Where each icon is drawn now, by file name: a rebuild starts from here, so a slot change glides (rulebook P13).
  std::map<std::string, std::pair<float, float>> m_shownAt;
  AnimationManager m_animations;
  // Starts `area` at the icon's last drawn place and moves it to (x, y) on the move spring.
  void glide(InputArea& area, const std::string& name, float x, float y);
  // Saved grid places by file name (DESIGN.md: positions per output, $XDG_STATE_HOME/andromeda/desktop-icons.json).
  std::map<std::string, Slot> m_slots;
  std::optional<std::size_t> m_pressIndex;
  float m_pressX = 0.0F;
  float m_pressY = 0.0F;
  bool m_dragging = false;
  // F2: the file being renamed in place, and its field.
  std::optional<std::string> m_renaming;
  // The typed name; a rebuild while renaming (thumbnail upload, directory change) recreates the field from it.
  std::optional<std::string> m_renameText;
  Input* m_renameInput = nullptr;
  void commitRename(const std::string& text);
};

#pragma once

#include "core/files/directory_watcher.h"
#include "core/files/file_model.h"
#include "render/animation/animation_manager.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
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
class DragIconSurface;
class DesktopFileOps;
class SessionBus;
class LayerSurface;
class Node;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
typedef struct _GAppInfo GAppInfo;

namespace sdbus {
  class IProxy;
}

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
                  ThumbnailService& thumbnails, SessionBus* bus);
  // Follows desktop_icons.enabled and the output set; safe to call again.
  void sync();
  [[nodiscard]] DirectoryWatcher& watcher() noexcept { return m_watcher; }
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const;
  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  // Copies an app's .desktop file into ~/Desktop as a shortcut (one FileOps1 job, undo with Ctrl+Z).
  void addShortcut(const std::filesystem::path& desktopEntry);

private:
  struct Slot {
    int column = 0;
    int row = 0;
    bool operator==(const Slot&) const = default;
  };
  // Use Stacks kinds (FILES-UI-SPEC §20), in grid order.
  enum class StackKind : std::uint8_t {
    Images,
    Movies,
    Music,
    Pdfs,
    Documents,
    Spreadsheets,
    Presentations,
    Archives,
    Other,
  };
  struct Cell {
    // A stack cell shows its newest file; `members` holds every file of the kind.
    std::shared_ptr<const FileInfo> info;
    std::optional<StackKind> stack;
    std::vector<std::shared_ptr<const FileInfo>> members;
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
  void activate(const std::vector<std::filesystem::path>& paths);
  void trash(const std::vector<std::filesystem::path>& paths);
  // Copies (or moves) files into `dir` as one FileOps1 job (undo with Ctrl+Z); the first new file in ~/Desktop lands
  // at the surface point `at`.
  void transferIn(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& dir, bool move,
                  std::optional<std::pair<float, float>> at = std::nullopt, bool duplicate = false);
  // Gives `name` (in ~/Desktop, maybe not listed yet) the place under the surface point: the nearest free grid cell,
  // or the point itself with Snap to Grid off. Every listed icon keeps its place from then on.
  void placeAt(const std::string& name, float x, float y);
  [[nodiscard]] bool freePlacement() const;
  // Cut and Copy put the files on the clipboard (GNOME copied-files + uri-list); Paste copies (or, after a cut,
  // moves) the clipboard's files into `dir`, the first at the surface point `at`.
  // New Folder / New Text Document: made in ~/Desktop at `at` (else the next free cell), then renamed in place.
  void createNew(bool folder, std::optional<std::pair<float, float>> at);
  [[nodiscard]] bool canPaste() const;
  void copyToClipboard(const std::vector<std::filesystem::path>& paths, bool cut);
  void paste(const std::filesystem::path& dir, std::optional<std::pair<float, float>> at);
  void openWithApp(GAppInfo* app, const std::vector<std::filesystem::path>& paths);
  void showProperties(const std::vector<std::filesystem::path>& paths);
  [[nodiscard]] static std::vector<std::filesystem::path> urisToPaths(const std::string& uriList);
  // The files the selection stands for: a selected stack gives all its members.
  [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;
  [[nodiscard]] std::string keyOf(const Cell& cell) const;
  [[nodiscard]] bool isSelected(const Cell& cell) const;
  void selectCell(const Cell& cell, bool on);
  [[nodiscard]] static StackKind stackKindOf(const FileInfo& info);
  [[nodiscard]] static std::string stackName(StackKind kind);
  void layoutStacks(int rows);
  void placeSaved(int rows, int columns);
  // A drag past the slop becomes a system drag (text/uri-list) so other apps and the dock take it.
  void startSystemDrag();
  void endSystemDrag(bool droppedHere);
  void onDropTarget(float x, float y, std::uint32_t action, const std::string& data);
  // The folder cell under a surface point that is not part of the selection, if any.
  [[nodiscard]] std::optional<std::size_t> folderAt(float x, float y) const;
  [[nodiscard]] Slot slotAt(float x, float y) const;
  void setDropHighlight(std::optional<std::size_t> index);
  void resetDraggedCells();
  void openDesktopMenu(float x, float y);
  void openItemMenu(std::size_t index, float x, float y);
  void updateBand(float x, float y);
  void onCellMotion(float sceneX, float sceneY);
  void moveSelected(float dx, float dy);
  void drop();
  [[nodiscard]] float slotX(int column) const;
  [[nodiscard]] float slotY(int row) const;
  void loadSlots();
  void saveSlots() const;
  [[nodiscard]] std::filesystem::path slotsFile() const;
  [[nodiscard]] std::string outputKey() const;
  [[nodiscard]] std::vector<std::size_t> selection() const;
  [[nodiscard]] std::string iconPathFor(const FileInfo& info);
  [[nodiscard]] std::string labelFor(const FileInfo& info) const;
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
  // The file of the last click, by key: an index would pair clicks on different files after the list shifts.
  std::optional<std::string> m_lastClick;
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
  // Snap to Grid off: the icon's top-left in px, x measured from the start corner's side. Its slot stays the nearest
  // grid cell, so new files fill around it.
  std::map<std::string, std::pair<float, float>> m_freeAt;
  std::optional<bool> m_snapToGrid;
  mutable std::map<std::string, std::pair<std::int64_t, std::string>> m_labels; // file name -> (mtime ns, app name)
  std::optional<std::size_t> m_pressIndex;
  float m_pressX = 0.0F;
  float m_pressY = 0.0F;
  bool m_dragging = false;
  std::uint32_t m_pressSerial = 0;
  // A system drag from the desktop runs; drops on the desktop itself move icons instead of copying.
  bool m_systemDrag = false;
  std::unique_ptr<DragIconSurface> m_dragIcon;
  std::unique_ptr<Node> m_dragImage;
  std::optional<std::size_t> m_dropHighlight;
  std::set<StackKind> m_expanded;
  // Use Stacks grouping, kept until the listing or its sort changes (not redone on every rebuild).
  bool m_groupsStale = true;
  std::vector<std::shared_ptr<const FileInfo>> m_singles;
  std::map<StackKind, std::vector<std::shared_ptr<const FileInfo>>> m_groups;
  std::unique_ptr<DesktopFileOps> m_fileOps;
  SessionBus* m_bus = nullptr;
  std::unique_ptr<sdbus::IProxy> m_fileManager;
  // The files this client put on the clipboard (the compositor gives no offer back to the owner).
  std::vector<std::filesystem::path> m_clipboard;
  bool m_clipboardCut = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  // F2: the file being renamed in place, and its field.
  std::optional<std::string> m_renaming;
  // The typed name; a rebuild while renaming (thumbnail upload, directory change) recreates the field from it.
  std::optional<std::string> m_renameText;
  Input* m_renameInput = nullptr;
  void commitRename(const std::string& text);
};

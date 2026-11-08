#pragma once

#include "core/files/file_model.h"
#include "core/timer_manager.h"
#include "render/core/color.h"
#include "ui/files/type_ahead.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

class Flex;
class AnimationManager;
class DirectoryWatcher;
class InputArea;
class Node;
class Renderer;
struct KeyboardEvent;

// One file browser (.agent/DESIGN.md "Files v2", FILES-UX.md): the listing of one folder with its history, views,
// sort, selection and keys. The Files window, the FileChooser portal sheets and shell file dialogs host it. A host
// places buildSidebar(), buildHeaderAccessory() and buildContent(); each call builds fresh nodes (the host rebuilds on
// resize or scale), and the browser keeps its state across builds and updates the latest nodes in place.
class FileBrowser {
public:
  enum class View : std::uint8_t { Icons, List, Columns, Gallery };

  struct Place {
    std::string label;
    std::string glyph;
    std::filesystem::path path; // empty for a host place (onClick)
    std::string caption;        // trailing text, e.g. the Trash count
    std::function<void()> onClick;
    std::string id;             // matches listingId() while that listing shows
    bool dropTrash = false;     // a drop on this row moves to the Trash
    std::string art;            // Files art set name (files-icons/<art>.svg); empty = the glyph
    // Favourites: above home. Tags: under the caption "Tags". Last: no caption (Trash).
    enum class Group : std::uint8_t { Favourites, Locations, Tags, Last } group = Group::Locations;
    std::optional<Color> dot;   // set: a 12 px round dot in this colour replaces the art (tags)
    std::optional<float> usage; // set: a 40 x 4 usage bar, 0..1 used (This PC)
    bool eject = false;         // a hover-only eject button that runs onEject
    std::function<void()> onEject;
  };

  struct Host {
    std::function<void()> invalidate;                           // a node changed in place: relayout + repaint
    std::function<void()> rebuild;                              // the host builds the nodes again
    std::function<void(const std::filesystem::path&)> openFile; // Enter / double-click on a file
    std::function<void()> changed;                              // location, selection or status changed
    std::function<std::uint32_t()> modifiers;                   // KeyMod mask held now
    std::function<void(std::size_t index, float sceneX, float sceneY)> menu; // secondary click (FB4)
    // Starts a system drag of these paths; false when the platform has none. The host calls dragEnded() after.
    std::function<bool(const std::vector<std::filesystem::path>&)> startDrag;
    std::function<void(InputArea*)> focus;
    // Renames `path` to `name`; `done` gets "" or the error to show under the field.
    std::function<void(const std::filesystem::path& path, const std::string& name,
                       std::function<void(const std::string& error)> done)>
        rename;
    // Space: Quick Look for the selected or cursor item (D9 runs it out of process).
    std::function<void(const std::filesystem::path&)> quickLook;
    // The view changed (Ctrl+1-4, the switch); the Files window hides its sidebar while the Gallery shows.
    std::function<void(View)> viewChanged;
    // Adds the host's circles after the view switch (mockup .ahdr: split, info).
    std::function<void(Flex& row, float scale)> headerButtons;
    // A pointer press on an item or on empty space in the view (split view: the pane under it becomes active).
    std::function<void()> pressed;
  };

  struct Options {
    float scale = 1.0F;
    View view = View::Icons;
    bool showHidden = false;
    std::vector<Place> extraPlaces; // after the folders (Trash in Files)
    // Rows this rejects are dimmed and not selectable (picker filters, D12); folders stay enabled.
    std::function<bool(const FileInfo&)> enabled;
    class ThumbnailService* thumbnails = nullptr; // previews in the icons; the host owns it and polls it
  };

  // A listing that is not one folder (Trash, Recents, search results). Row names that are absolute paths open as
  // paths; others resolve against location().
  struct Listing {
    std::string id;
    std::string title;
    std::vector<std::shared_ptr<const FileInfo>> rows;
    bool activatable = true; // false: Enter and double-click do nothing (Trash items open only after Put Back)
    // Set: the listing draws this page instead of rows (This PC); built again on each content build.
    std::function<std::unique_ptr<Node>(float scale)> page;
    // Set: activating a row runs this first (folders inside an archive); true = handled.
    std::function<bool(const FileInfo&)> open;
    // Set: a bar above the rows (the archive Extract bar); built again on each content build.
    std::function<std::unique_ptr<Node>(float scale)> banner;
  };

  FileBrowser(DirectoryWatcher& watcher, AnimationManager& animations, Host host, Options options);
  ~FileBrowser();
  FileBrowser(const FileBrowser&) = delete;
  FileBrowser& operator=(const FileBrowser&) = delete;

  [[nodiscard]] std::unique_ptr<Node> buildSidebar(float width);
  // The host shows another browser's sidebar and header accessory (split view): forget the nodes this one built.
  void releaseChrome();
  [[nodiscard]] std::unique_ptr<Node> buildHeaderAccessory(); // view switch + sort (rulebook 9.12 header)
  // The listing and, with `pathBar`, the path bar under it (split view: only the active pane has one). `crumbs` adds
  // the split-pane crumb row on top (FILES-UI-SPEC §13).
  [[nodiscard]] std::unique_ptr<Node> buildContent(bool pathBar = true, bool crumbs = false);
  // Text after the status ("N items · free"), e.g. the other split pane's folder; empty = none.
  void setStatusSuffix(std::string suffix);
  void setCrumbAccent(bool accent); // the active split pane shows its last crumb in accent text
  [[nodiscard]] InputArea* focusArea() const noexcept;
  void setScale(float scale);

  // Directional navigation (open, parent, history) slides (D7); jumps (sidebar, search) crossfade.
  void go(const std::filesystem::path& dir);
  void jump(const std::filesystem::path& dir);
  void showListing(Listing listing); // a jump
  void back();
  void forward();
  void up();
  [[nodiscard]] bool canBack() const noexcept { return !m_back.empty(); }
  [[nodiscard]] bool canForward() const noexcept { return !m_forward.empty(); }

  void setView(View view);
  // Tabs (FB4, mockup §1): the strip shows with 2 or more. A new tab opens the current folder (Finder).
  void newTab();
  // False with one tab left: the host closes the window (Ctrl+W).
  bool closeTab();
  void selectTab(std::size_t index);
  void stepTab(int delta); // Ctrl+Tab, Ctrl+Shift+Tab
  [[nodiscard]] std::size_t tabCount() const noexcept { return m_tabs.size(); }
  // Icons view icon size in logical px, 48-128 on the 16 grid (path bar zoom slider); 0 = component.grid.icon.
  void setIconSize(float size);
  [[nodiscard]] float iconSize() const noexcept;
  // The window is active: selections draw the accent tint; inactive windows draw them fill_2.
  void setActive(bool active);
  void setSort(FileSortKey key); // the current key again flips the direction
  void setShowHidden(bool show);
  [[nodiscard]] bool showHidden() const noexcept { return m_options.showHidden; }
  void setExtraPlaces(std::vector<Place> places);
  void setEnabledPredicate(std::function<bool(const FileInfo&)> enabled);
  // Shows only the items whose name contains `text` (case and accents folded); cleared when the folder changes.
  void setFilter(std::string text);
  void focusSearch(); // Ctrl+F
  void focusList();
  void reload();

  [[nodiscard]] const std::filesystem::path& location() const noexcept { return m_location; }
  [[nodiscard]] const std::string& listingId() const noexcept { return m_listingId; }
  [[nodiscard]] std::string title() const;
  [[nodiscard]] View view() const noexcept { return m_options.view; }
  [[nodiscard]] FileModel::Status status() const;
  [[nodiscard]] const std::vector<std::shared_ptr<const FileInfo>>& rows() const noexcept { return m_rows; }
  [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;
  [[nodiscard]] std::vector<std::shared_ptr<const FileInfo>> selectedRows() const;
  [[nodiscard]] std::size_t selectionCount() const noexcept { return m_selected.size(); }
  [[nodiscard]] std::filesystem::path pathOf(const FileInfo& info) const;
  void selectNames(const std::vector<std::string>& names);
  // Opens the file's folder with the file selected (Go to Folder on a file path, Show in Folder).
  void reveal(const std::filesystem::path& file) { goSelecting(file.parent_path(), {file.filename().string()}); }
  void selectAll();
  // Moves the cursor and the selection by `delta` items in view order (Quick Look walk, FILES-UX 3.6).
  void walk(int delta);
  [[nodiscard]] std::optional<std::filesystem::path> cursorPath() const;
  void clearSelection();

  // Drag and drop (FILES-UX §2.3). A drag over the browser at a scene point updates the drop cue and spring-load and
  // returns where a drop there goes; nullopt = no drop (a dragged item, a descendant, a file, a read-only folder).
  struct DropTarget {
    std::filesystem::path folder; // empty for the Trash
    bool trash = false;
    bool operator==(const DropTarget&) const = default;
  };
  std::optional<DropTarget> dragOver(float sceneX, float sceneY);
  void dragLeave();
  // The drag dropped or was cancelled: the cue clears and a spring-loaded navigation returns (D15).
  void dragEnded();
  [[nodiscard]] const std::vector<std::filesystem::path>& draggedPaths() const noexcept { return m_dragPaths; }
  // The drag image (mockup §22): the first 3 dragged items stacked, a count badge for 2 or more. Size: dragImageSize().
  [[nodiscard]] std::unique_ptr<Node> buildDragImage() const;
  [[nodiscard]] float dragImageSize() const noexcept;

  // Inline rename (FILES-UX §2.5): F2 selects the stem, F2 again all; Enter commits, Escape cancels, Tab commits and
  // renames the next item.
  void beginRename();
  [[nodiscard]] bool renaming() const noexcept { return m_rename.has_value(); }
  // After a host-created item appears (New Folder), select it and rename it.
  void renameWhenListed(std::string name);

  // Keys no text field took (FILES-UX §2.4). True = handled.
  bool onKey(const KeyboardEvent& event);

  // Item pointer input from the views (also used by tests through the adapters).
  void onItemPress(std::size_t index);
  void onItemActivate(std::size_t index);
  // The one selected item opens as on a double-click; false when the selection is not one item.
  bool activateSelection();
  void onItemDrag(std::size_t index);
  void onItemRelease(std::size_t index);
  // A bound cell shows a row whose stat is not read yet (FB-SPEED).
  void wantStat(const std::shared_ptr<const FileInfo>& row);
  void onBandStart(bool icons, float x, float y);
  void onBandMove(bool icons, float x, float y);
  void onBandEnd(bool icons);
  // Icons view press feedback: the pressed cell runs to 0.98 and back on spring.press.
  void pressItem(std::optional<std::size_t> index);
  // List view (FB2): a folder row expands in place, its items 16 further in; Right/Left keys and the triangle.
  void setExpanded(std::size_t index, bool expanded);
  [[nodiscard]] bool expanded(const std::string& name) const { return m_expanded.contains(name); }
  [[nodiscard]] bool listExpandable() const; // list view of a folder, no filter

private:
  struct Ui;
  struct Page;
  enum class Motion : std::uint8_t { None, Deeper, Back, Fade };
  struct HistoryEntry {
    std::filesystem::path path;
    std::optional<Listing> listing;
  };

  void navigate(HistoryEntry target, Motion motion, std::vector<std::string> select = {});
  [[nodiscard]] bool pushHistory();
  void goSelecting(const std::filesystem::path& dir, std::vector<std::string> select);
  void clearSkeleton(Page& page);
  void attachModel();
  void onModelChanged();
  void present(Motion motion);
  void refreshRows();
  void rebindPages();
  void select(std::size_t index, std::uint32_t modifiers);
  void moveCursor(std::ptrdiff_t delta, std::uint32_t modifiers);
  void setCursor(std::size_t index, bool scroll);
  void refreshChrome();
  void refreshViewButtons();
  void refreshTabs();
  void widenSearch(bool focused);
  void refreshSidebar();
  void showSkeleton(bool show);
  void applyViewMix();
  void notifyChanged();
  [[nodiscard]] std::optional<std::size_t> cursorIndex() const;
  [[nodiscard]] std::optional<std::size_t> indexOf(const std::string& name) const;
  [[nodiscard]] bool rowEnabled(const FileInfo& info) const;
  [[nodiscard]] HistoryEntry current() const;
  [[nodiscard]] std::size_t columns() const;
  void setDropCue(std::optional<std::string> item);
  [[nodiscard]] class VirtualGridView* activeGrid() const;
  void syncColumns();
  void syncGallery();
  void galleryStep(int delta);
  void syncCustomPage(Page& page);
  void logGeometry();
  void springLoad();
  void placeRenameField();
  void restoreRename();
  void commitRename(bool next);
  void cancelRename();
  void startRename(std::size_t index, bool all);

  DirectoryWatcher& m_watcher;
  AnimationManager& m_animations;
  Host m_host;
  Options m_options;

  std::filesystem::path m_location;
  std::string m_listingId;
  std::string m_filter; // the header search text
  std::vector<std::filesystem::path> m_galleryItems; // what the Gallery shows, in view order
  std::size_t m_galleryCurrent = 0;
  std::optional<Listing> m_listing;
  std::vector<HistoryEntry> m_back;
  std::vector<HistoryEntry> m_forward;
  std::shared_ptr<FileModel> m_model;
  std::unique_ptr<FileModelView> m_modelView;
  FileSort m_sort;
  std::vector<std::shared_ptr<const FileInfo>> m_rows; // what the current page shows

  std::set<std::string> m_selected;
  std::optional<std::string> m_anchor;
  std::optional<std::string> m_cursor;
  std::vector<std::string> m_pendingSelect;
  TypeAhead m_typeAhead;
  struct Band {
    bool icons = true;
    float x0 = 0.0F, y0 = 0.0F, x1 = 0.0F, y1 = 0.0F;
    bool moved = false;
    std::uint32_t mods = 0;
    std::set<std::string> before; // the selection when the band started (Shift adds to it, Ctrl toggles it)
  };
  std::optional<Band> m_band;
  std::string m_lastPressName;
  std::optional<std::string> m_collapseOnRelease;
  float m_iconSize = 0.0F; // 0 = component.grid.icon
  struct Expansion {
    std::shared_ptr<FileModel> model;
    std::unique_ptr<FileModelView> view;
  };
  std::map<std::string, Expansion> m_expanded;
  struct Tab {
    HistoryEntry entry;
    std::vector<HistoryEntry> back;
    std::vector<HistoryEntry> forward;
    std::vector<std::string> selected;
  };
  std::vector<Tab> m_tabs{Tab{}}; // the current tab's slot is stale until a switch saves into it
  std::size_t m_tab = 0; // list view: expanded folder rows by row name (relative path)
  [[nodiscard]] bool expandable() const;
  bool m_active = true;
  std::string m_pressName;
  float m_pressScale = 1.0F;
  std::uint32_t m_pressAnim = 0;
  std::uint32_t m_searchAnim = 0;
  bool m_keyboardCursor = false; // the focus ring shows after a key moved the cursor, not after a click
  std::chrono::steady_clock::time_point m_lastPressAt{};
  std::chrono::steady_clock::time_point m_navigateAt{}; // DESK speed log: the folder listing started
  bool m_listedLogged = false;

  Motion m_pendingMotion = Motion::None;
  std::uint64_t m_presentGen = 0; // bumps per present(); stale settle callbacks skip
  bool m_waiting = false;    // a navigation waits for its listing
  Timer m_skeletonTimer;     // 200 ms after a listing starts
  Timer m_skeletonHoldTimer; // a shown skeleton stays >= 400 ms
  bool m_skeletonShown = false;
  bool m_skeletonHeld = false;

  std::vector<std::filesystem::path> m_dragPaths; // our own drag; empty for other apps' drags
  std::optional<std::string> m_dropItem;         // the folder row with the drop cue
  std::string m_statusSuffix;
  bool m_crumbAccent = false;
  struct Spring {
    DropTarget target;
    bool sidebar = false;
  };
  std::optional<Spring> m_springTarget;
  struct SpringFrom {
    HistoryEntry entry; // where the drag started before spring-loaded navigation
    std::vector<HistoryEntry> back;
    std::vector<HistoryEntry> forward;
  };
  std::optional<SpringFrom> m_springFrom;
  Timer m_springTimer;
  Timer m_edgeScroll;     // a drag at a view edge scrolls it (FILES-UX §2.3)
  float m_edgeSpeed = 0.0F; // px/s, negative = up
  void updateEdgeScroll(float sceneX, float sceneY);
  std::optional<DropTarget> m_lastDrop;

  struct Rename {
    std::string name; // FileInfo::name of the row
    std::string error;
    std::string text;    // the field text, kept across content rebuilds
    bool edited = false; // typed into: a rebuild keeps the text, not the stem selection
    bool busy = false;
  };
  std::optional<Rename> m_rename;
  std::string m_renamePending;
  std::string m_geometry; // last DESK_FILES_GEOMETRY dump
  int m_peekListener = 0;
  std::shared_ptr<void> m_thumbWake; // ThumbnailService::Subscription
  std::vector<std::shared_ptr<const FileInfo>> m_statWanted;
  bool m_statPosted = false;
  std::chrono::steady_clock::time_point m_statHoldSince{};

  std::unique_ptr<class FileColumns> m_columns;
  std::unique_ptr<Ui> m_ui;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

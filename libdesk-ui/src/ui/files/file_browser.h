#pragma once

#include "core/files/file_model.h"
#include "core/timer_manager.h"
#include "render/core/color.h"
#include "ui/controls/context_menu.h"
#include "ui/files/file_gallery.h"
#include "ui/files/type_ahead.h"
#include "ui/palette.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <string_view>
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
// What a folder keeps of its view (FILES-UI-SPEC §16).
enum class FileGroupBy : std::uint8_t { None, Kind, Date, Size, Tags };
struct FileBrowserViewState {
  std::uint8_t view = 0;  // FileBrowser::View
  float iconSize = 0.0F;  // 0 = component.grid.icon
  FileSort sort{};
  bool showHidden = false;
  FileGroupBy group = FileGroupBy::None;
  bool extensions = true; // false: names show without their extension (folders keep theirs)
  std::uint8_t columns = 0; // file_columns bits (file_cells.h)
};

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
    bool dropFiles = false;     // a host place that takes dropped files (a phone): DropTarget::place = id
  };

  struct Host {
    std::function<void()> invalidate;                           // a node changed in place: relayout + repaint
    std::function<void()> rebuild;                              // the host builds the nodes again
    std::function<void(const std::filesystem::path&)> openFile; // Enter / double-click on a file
    std::function<void()> changed;                              // location, selection or status changed
    std::function<std::uint32_t()> modifiers;                   // KeyMod mask held now
    std::function<void(std::size_t index, float sceneX, float sceneY)> menu; // secondary click (FB4)
    std::function<void(float sceneX, float sceneY)> backgroundMenu;           // secondary click on empty space
    // Starts a system drag of these paths; false when the platform has none. The host calls dragEnded() after.
    std::function<bool(const std::vector<std::filesystem::path>&)> startDrag;
    std::function<void(InputArea*)> focus;
    // True while a modal sheet covers the window: the search field behind it does not take the keys back.
    std::function<bool()> modal;
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
    // Set: search offers the This PC scope (FILES-UI-SPEC §15). The host queries its index for `text` with an index
    // kind word ("" = any, "pdf", "image", "doc", "sheet") and answers with setSearchResults.
    std::function<void(const std::string& text, std::string_view kind)> search;
    // A pointer press on an item or on empty space in the view (split view: the pane under it becomes active).
    std::function<void()> pressed;
    // Per-folder views (FILES-UI-SPEC §16): the view a folder opens with (nullopt = keep the current one), and the
    // view to keep after the user changes it there. Folders only: listings (Trash, Recents, search) never ask.
    std::function<std::optional<struct FileBrowserViewState>(const std::filesystem::path&)> viewFor;
    std::function<void(const std::filesystem::path&, const struct FileBrowserViewState&)> viewSaved;
    std::function<void(const Node* anchor)> viewOptions; // set: the header sort button opens View Options here
    // Search mode starts (field focused or text) or ends: the window hides or shows its own header items.
    std::function<void(bool on)> searchMode;
    // Group By Tags: the first tag of a file with its rank in the tag list; nullopt = no tag. Memory only (a cache).
    std::function<std::optional<std::pair<int, std::string>>(const std::filesystem::path&)> tagOf;
    // Gallery (FileGallery::Host): its action capsule acts on the current photo, which is the selection.
    std::function<void(std::string_view action, float sceneX, float sceneY)> galleryAction;
    std::function<bool(const std::filesystem::path&)> starred;
    std::function<std::string()> phoneName;
    std::function<void(const std::filesystem::path&, int mapWidth, int mapHeight,
                       std::function<void(FileGallery::PhotoInfo)> done)>
        galleryInfo;
    std::function<void(const std::filesystem::path& video, bool sound, std::array<float, 4> rect)> galleryPlayVideo;
    std::function<void(std::array<float, 4> rect)> galleryPlaceVideo;
    std::function<void()> galleryToggleVideo;
    std::function<void()> galleryStopVideo;
    std::function<void(const std::filesystem::path& photo, std::function<void(std::filesystem::path)> done)>
        galleryMotionVideo;
    // A menu of checkable rows at a scene point (the list header's column picker); `activated` gets the entry id.
    std::function<void(std::vector<ContextMenuControlEntry> entries, float sceneX, float sceneY,
                       std::function<void(std::int32_t)> activated)>
        contextMenu;
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
  void openInNewTab(std::size_t index); // a folder row; files are ignored
  // False with one tab left: the host closes the window (Ctrl+W).
  bool closeTab();
  void selectTab(std::size_t index);
  void stepTab(int delta); // Ctrl+Tab, Ctrl+Shift+Tab
  [[nodiscard]] std::size_t tabCount() const noexcept { return m_tabs.size(); }
  [[nodiscard]] std::size_t currentTab() const noexcept { return m_tab; }
  // Each tab's folder (empty for a listing tab such as Recents), for restore at the next launch.
  [[nodiscard]] std::vector<std::filesystem::path> tabFolders() const;
  // Opens one tab per folder after the current one (missing folders are skipped), then shows tab `current`.
  void restoreTabs(const std::vector<std::filesystem::path>& folders, std::size_t current);
  // Icons view icon size in logical px, 48-128 on the 16 grid (path bar zoom slider); 0 = component.grid.icon.
  void setIconSize(float size);
  [[nodiscard]] float iconSize() const noexcept;
  // The window is active: selections draw the accent tint; inactive windows draw them fill_2.
  void setActive(bool active);
  void setSort(FileSortKey key); // the current key again flips the direction
  void setSortOrder(FileSort sort);
  [[nodiscard]] FileSort sortOrder() const noexcept { return m_sort; }
  [[nodiscard]] struct FileBrowserViewState viewState() const;
  [[nodiscard]] bool searchMode() const noexcept; // the search field has focus or text: the header shows only search
  [[nodiscard]] const Node* viewOptionsAnchor() const; // the header sort button; null before the header is built
  void setShowHidden(bool show);
  void setGroupBy(FileGroupBy group);
  [[nodiscard]] FileGroupBy groupBy() const noexcept { return m_group; }
  void regroup(); // the host's tags changed: Group By Tags orders the rows again
  void setShowExtensions(bool show);
  [[nodiscard]] bool showExtensions() const noexcept { return m_extensions; }
  void setListColumns(std::uint8_t columns); // file_columns bits
  [[nodiscard]] std::uint8_t listColumns() const noexcept { return m_listColumns; }
  // Coloured bars on rows by name (split-view folder compare); an empty map clears them, and so does a location change.
  void setRowMarks(std::map<std::string, ColorSpec> marks);
  [[nodiscard]] bool showHidden() const noexcept { return m_options.showHidden; }
  void setExtraPlaces(std::vector<Place> places);
  // The current Gallery photo changed outside (starred, rotated): draw its state again.
  void refreshGallery();
  // From the host's Gallery video player.
  void galleryVideoProgress(bool playing, float progress, const std::string& time);
  void galleryVideoEnded();
    void setEnabledPredicate(std::function<bool(const FileInfo&)> enabled);
  // Shows only the items whose name contains `text` (case and accents folded); cleared when the folder changes.
  void setFilter(std::string text);
  enum class SearchScope : std::uint8_t { Folder, Everywhere };
  enum class SearchKind : std::uint8_t { All, Pdf, Image, Doc, Sheet, Recent };
  void setSearchScope(SearchScope scope);
  void setSearchKind(SearchKind kind);
  struct SearchState {
    std::string text;
    SearchScope scope = SearchScope::Folder;
    SearchKind kind = SearchKind::All; // Recent = changed in the last 30 days
  };
  // The search the header shows now (Save Search); nullopt while no search runs.
  [[nodiscard]] std::optional<SearchState> searchState() const;
  // The host's index answer for `text`; rows have absolute names. An answer for an older text is dropped.
  void setSearchResults(const std::string& text, std::vector<std::shared_ptr<const FileInfo>> rows);
  void focusSearch(); // Ctrl+F
  void focusList();
  void reload();

  [[nodiscard]] const std::filesystem::path& location() const noexcept { return m_location; }
  [[nodiscard]] const std::string& listingId() const noexcept { return m_listingId; }
  [[nodiscard]] std::string title() const;
  [[nodiscard]] View view() const noexcept { return m_options.view; }
  [[nodiscard]] FileSortKey sortKey() const noexcept { return m_sort.key; }
  [[nodiscard]] FileModel::Status status() const;
  [[nodiscard]] const std::vector<std::shared_ptr<const FileInfo>>& rows() const noexcept { return m_rows; }
  [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;
  [[nodiscard]] std::vector<std::shared_ptr<const FileInfo>> selectedRows() const;
  [[nodiscard]] std::size_t selectionCount() const noexcept { return m_selected.size(); }
  [[nodiscard]] std::filesystem::path pathOf(const FileInfo& info) const;
  void selectNames(const std::vector<std::string>& names);
  // Opens the file's folder with the file selected (Go to Folder on a file path, Show in Folder).
  void reveal(const std::filesystem::path& file) { goSelecting(file.parent_path(), {file.filename().string()}); }
  // Shows a listed photo in the Gallery without making it the folder's view; false for other files.
  bool openInGallery(const std::filesystem::path& file);
  // The same for a file of this folder that may not be listed yet: it opens once the listing lands.
  void showInGallery(const std::string& name);
  void selectAll();
  // Moves the cursor and the selection by `delta` items in view order (Quick Look walk, FILES-UX 3.6).
  void walk(int delta);
  [[nodiscard]] std::optional<std::filesystem::path> cursorPath() const;
  void clearSelection();

  // Drag and drop (FILES-UX §2.3). A drag over the browser at a scene point updates the drop cue and spring-load and
  // returns where a drop there goes; nullopt = no drop (a dragged item, a descendant, a file, a read-only folder).
  struct DropTarget {
    std::filesystem::path folder; // empty for the Trash and a host place
    bool trash = false;
    std::string place;            // a Place::dropFiles row's id
    bool operator==(const DropTarget&) const = default;
  };
  std::optional<DropTarget> dragOver(float sceneX, float sceneY);
  void dragLeave();
  // The drag dropped or was cancelled: the cue clears and a spring-loaded navigation returns (D15).
  void dragEnded();
  [[nodiscard]] const std::vector<std::filesystem::path>& draggedPaths() const noexcept { return m_dragPaths; }
  // The drag image (mockup §22): the first 3 dragged items stacked, a count badge for 2 or more. Size: dragImageSize().
  [[nodiscard]] std::unique_ptr<Node> buildDragImage(Renderer& renderer) const;
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
  void backgroundMenu(float sceneX, float sceneY); // clears the selection, then the host's background menu
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
  void syncListHeader(); // the list header columns follow the stage width, as FileListRow does
  void refreshViewButtons();
  void refreshTabs();
  void setSearchFocused(bool focused);
  void syncSearchMode();
  void closeSearch(); // clears the text and leaves search mode
  [[nodiscard]] bool searchingEverywhere() const;
  void refreshChips();
  void showChips();
  void querySearch();
  void syncSearchView();
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
  void setPlaceCue(Node* row);
  [[nodiscard]] class VirtualGridView* activeGrid() const;
  void syncColumns();
  void syncGallery();
  void galleryStep(int delta);
  void galleryAction(std::string_view action, float sceneX, float sceneY);
  void leaveGallery(); // Esc or the back button: the view the Gallery was opened from
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
  SearchScope m_searchScope = SearchScope::Folder;
  SearchKind m_searchKind = SearchKind::All;
  std::string m_searchText; // the text m_searchRows answer
  std::vector<std::shared_ptr<const FileInfo>> m_searchRows;
  std::optional<View> m_viewBeforeSearch; // the view This PC results replaced with the list
  bool m_searchViewSwitch = false;         // setView for search: the host does not save it
  std::string m_pendingGallery;
  bool m_galleryTransient = false; // the Gallery opened on one photo: leaving it restores the view, nothing is saved
  bool m_applyingView = false;             // navigate applies the folder's stored view: nothing to save
  void saveView();
  std::vector<std::filesystem::path> m_galleryItems; // what the Gallery shows, in view order
  std::shared_ptr<FileGallery::State> m_galleryState = std::make_shared<FileGallery::State>(); // across rebuilds
  std::size_t m_galleryCurrent = 0;
  View m_beforeGallery = View::Icons;
  std::optional<Listing> m_listing;
  std::vector<HistoryEntry> m_back;
  std::vector<HistoryEntry> m_forward;
  std::shared_ptr<FileModel> m_model;
  std::unique_ptr<FileModelView> m_modelView;
  std::shared_ptr<void> m_modelEdits; // FileModel subscription: an edited file's preview decodes again
  std::set<std::string> m_staleThumbs; // edited files whose previews reload once they are quiet
  Timer m_staleTimer;
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
  Node* m_placeCue = nullptr;                    // the Place::dropFiles row with the drop cue
  ColorSpec m_placeCueFill;                      // its fill before the cue (the selected row's tint)
  std::optional<std::string> m_dropItem;         // the folder row with the drop cue
  std::string m_statusSuffix;
  // The status bar's free space, read off the UI thread (statvfs blocks on a hung network mount); one read at a time.
  void startMediaWorker();
  void readFreeSpace();

public:
  // The location's free space as last read off the UI thread (empty until known); asks for a fresh read when stale.
  [[nodiscard]] std::optional<std::uint64_t> freeSpace() {
    readFreeSpace();
    return m_freeFor == m_location ? m_free : std::nullopt;
  }

private:
  std::filesystem::path m_freeFor;
  std::optional<std::uint64_t> m_free;
  std::chrono::steady_clock::time_point m_freeAt{};
  bool m_freePending = false;
  std::map<std::string, ColorSpec> m_rowMarks;
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
  FileGroupBy m_group = FileGroupBy::None;
  bool m_extensions = true;
  std::uint8_t m_listColumns = 0;
  // Optional list columns: header facts per absolute path, read off the main thread (wantMedia).
  struct MediaText {
    std::string dimensions;
    std::string duration;
    std::string artist;
  };
  std::map<std::string, MediaText> m_media;
  std::set<std::string> m_mediaPending;
  std::vector<std::string> m_mediaWanted;
  bool m_mediaPosted = false;
  bool m_mediaRunning = false;
  void wantMedia(const std::string& path);
  void groupRows(); // Group By: orders m_rows by group, keeps the sort inside each, fills Page::groups
  void columnsMenu(float sceneX, float sceneY);
  [[nodiscard]] std::ptrdiff_t rowStep(int rows, std::ptrdiff_t columns) const; // Up/Down in item indexes
  std::chrono::steady_clock::time_point m_statHoldSince{};

  std::unique_ptr<class FileColumns> m_columns;
  std::unique_ptr<Ui> m_ui;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

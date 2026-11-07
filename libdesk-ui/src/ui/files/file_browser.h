#pragma once

#include "core/files/file_model.h"
#include "core/timer_manager.h"
#include "ui/files/type_ahead.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
  enum class View : std::uint8_t { Icons, List };

  struct Place {
    std::string label;
    std::string glyph;
    std::filesystem::path path; // empty for a host place (onClick)
    std::string caption;        // trailing text, e.g. the Trash count
    std::function<void()> onClick;
    std::string id;             // matches listingId() while that listing shows
  };

  struct Host {
    std::function<void()> invalidate;                           // a node changed in place: relayout + repaint
    std::function<void()> rebuild;                              // the host builds the nodes again
    std::function<void(const std::filesystem::path&)> openFile; // Enter / double-click on a file
    std::function<void()> changed;                              // location, selection or status changed
    std::function<std::uint32_t()> modifiers;                   // KeyMod mask held now
    std::function<void(std::size_t index, float sceneX, float sceneY)> menu; // secondary click (FB4)
  };

  struct Options {
    float scale = 1.0F;
    View view = View::Icons;
    bool showHidden = false;
    std::vector<Place> extraPlaces; // after the folders (Trash in Files)
    // Rows this rejects are dimmed and not selectable (picker filters, D12); folders stay enabled.
    std::function<bool(const FileInfo&)> enabled;
  };

  // A listing that is not one folder (Trash, Recents, search results). Row names that are absolute paths open as
  // paths; others resolve against location().
  struct Listing {
    std::string id;
    std::string title;
    std::vector<std::shared_ptr<const FileInfo>> rows;
  };

  FileBrowser(DirectoryWatcher& watcher, AnimationManager& animations, Host host, Options options);
  ~FileBrowser();
  FileBrowser(const FileBrowser&) = delete;
  FileBrowser& operator=(const FileBrowser&) = delete;

  [[nodiscard]] std::unique_ptr<Node> buildSidebar(float width);
  [[nodiscard]] std::unique_ptr<Node> buildHeaderAccessory(); // view switch + sort (rulebook 9.12 header)
  [[nodiscard]] std::unique_ptr<Node> buildContent();         // listing + path bar
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
  void setSort(FileSortKey key); // the current key again flips the direction
  void setShowHidden(bool show);
  [[nodiscard]] bool showHidden() const noexcept { return m_options.showHidden; }
  void setExtraPlaces(std::vector<Place> places);
  void setEnabledPredicate(std::function<bool(const FileInfo&)> enabled);
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
  void selectAll();
  void clearSelection();

  // Keys no text field took (FILES-UX §2.4). True = handled.
  bool onKey(const KeyboardEvent& event);

  // Item pointer input from the views (also used by tests through the adapters).
  void onItemPress(std::size_t index);
  void onItemActivate(std::size_t index);

private:
  struct Ui;
  struct Page;
  enum class Motion : std::uint8_t { None, Deeper, Back, Fade };
  struct HistoryEntry {
    std::filesystem::path path;
    std::optional<Listing> listing;
  };

  void navigate(HistoryEntry target, Motion motion);
  void attachModel();
  void onModelChanged();
  void present(Motion motion);
  void refreshRows();
  void rebindPages();
  void select(std::size_t index, std::uint32_t modifiers);
  void moveCursor(std::ptrdiff_t delta, std::uint32_t modifiers);
  void setCursor(std::size_t index, bool scroll);
  void refreshChrome();
  void refreshSidebar();
  void showSkeleton(bool show);
  void notifyChanged();
  [[nodiscard]] std::optional<std::size_t> cursorIndex() const;
  [[nodiscard]] std::optional<std::size_t> indexOf(const std::string& name) const;
  [[nodiscard]] bool rowEnabled(const FileInfo& info) const;
  [[nodiscard]] HistoryEntry current() const;
  [[nodiscard]] std::size_t columns() const;

  DirectoryWatcher& m_watcher;
  AnimationManager& m_animations;
  Host m_host;
  Options m_options;

  std::filesystem::path m_location;
  std::string m_listingId;
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
  std::string m_lastPressName;
  bool m_keyboardCursor = false; // the focus ring shows after a key moved the cursor, not after a click
  std::chrono::steady_clock::time_point m_lastPressAt{};

  Motion m_pendingMotion = Motion::None;
  bool m_waiting = false;    // a navigation waits for its listing
  Timer m_skeletonTimer;     // 200 ms after a listing starts
  Timer m_skeletonHoldTimer; // a shown skeleton stays >= 400 ms
  bool m_skeletonShown = false;
  bool m_skeletonHeld = false;

  std::unique_ptr<Ui> m_ui;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

#pragma once

#include "core/files/file_model.h"
#include "files/file_ops_client.h"
#include "picker_portal.h"
#include "trash.h"
#include "archive_listing.h"
#include "batch_rename_sheet.h"
#include "compress_sheet.h"
#include "extract_sheet.h"
#include "get_info_sheet.h"
#include "job_pill.h"
#include "storage_page.h"
#include "ui/files/file_browser.h"

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/timer_manager.h"

class AppChooserSheet;
class AppWindow;
namespace desk {
  class ForeignExport;
}
namespace sdbus {
  class IProxy;
}
class Node;
class Renderer;
class FilesApp;
struct KeyboardEvent;

// One Files window: a FileBrowser (location, history, views, selection) plus what Files adds around it: the Trash
// listing, jobs with conflicts and undo, the toast, and picker mode.
class FilesWindow {
public:
  static constexpr std::uint32_t kPickerWidth = 800; // the expanded picker (logical px before ui scale)
  static constexpr std::uint32_t kPickerHeight = 520;

  FilesWindow(FilesApp& app, AppWindow& window, std::filesystem::path location);
  ~FilesWindow();

  FilesWindow(const FilesWindow&) = delete;
  FilesWindow& operator=(const FilesWindow&) = delete;

  void go(const std::filesystem::path& location);
  void goTrash();
  void goThisPc();
  void refreshPlaces() { loadTrash(); }
  void searchAnswered(const std::string& text, const std::vector<std::shared_ptr<const FileInfo>>& rows);
  void tagsChanged();
  void goTag(const std::string& tag);
  void eject(const std::filesystem::path& mount);
  // After an eject or unmount: an error shows as a toast; success leaves the drive and refreshes the sidebar.
  void unmounted(const std::filesystem::path& mount, const std::string& error);
  void goRecents();
  void goNetwork();
  void newTab();
  bool closeTab(); // false with one tab left: the caller closes the window
  void stepTab(int delta);
  [[nodiscard]] std::size_t tabCount() const;
  void openConnect(const std::string& address); // Connect to Server sheet; "" = empty address
  void openWith(); // Open With sheet for the selected files (one content type)
  void toggleDetails(); // the details pane at the right edge (Alt+P)
  void goStarred();
  void toggleStar(); // Ctrl+D on the selection
  [[nodiscard]] bool selectionStarred() const;
  void openTerminalHere(); // the selected folder, else the current one
  void copyLocation(); // the selected paths, else the folder, as text on the clipboard
  void showItemMenu(std::size_t index, float sceneX, float sceneY); // right click on an item (mockup §4)
  void showBackgroundMenu(float sceneX, float sceneY);              // right click on empty view space
  [[nodiscard]] bool detailsOpen() const noexcept { return m_detailsOpen; }
  [[nodiscard]] bool canOpenWith() const;
  [[nodiscard]] AppChooserSheet* openWithSheet() const noexcept { return m_openWith.get(); }
  void launchWith(const std::string& id, const std::vector<std::filesystem::path>& paths);
  void openPathField(const std::string& initial); // Go to Folder; "" starts at the current folder
  void openArchive(const std::filesystem::path& archive);
  void showArchive(const std::filesystem::path& inner);
  void extractHere(const std::filesystem::path& archive);
  void openExtractSheet(const std::filesystem::path& archive, ExtractSheet::State state);
  void startExtract(const std::filesystem::path& archive, ExtractSheet::Request request);
  void back();
  void forward();
  void up();
  [[nodiscard]] bool canBack() const noexcept;
  [[nodiscard]] bool canForward() const noexcept;
  [[nodiscard]] bool inTrash() const noexcept;
  [[nodiscard]] const std::filesystem::path& location() const noexcept;
  [[nodiscard]] float scale() const noexcept;
  [[nodiscard]] FileBrowser& browser() noexcept { return *m_browser; }

  void openSelection();
  void copySelection(bool cut);
  void paste();
  void pasteInternal();
  void trashSelection();
  void restoreSelection();
  void undo();
  [[nodiscard]] bool canUndo() const noexcept;
  void newFolder();
  // One item: the inline field; 2 or more: the Batch Rename sheet.
  void renameSelection();
  void focusSearch();
  void compressSelection();
  void getInfo();
  void showStorage();
  void emptyTrash();    // no question: the caller confirmed
  void askEmptyTrash(); // the confirm sheet, then emptyTrash()
  [[nodiscard]] bool trashHasItems() const noexcept { return !m_trash.empty(); }
  void extractSelection(bool here);
  [[nodiscard]] bool canExtract() const;
  [[nodiscard]] bool renaming() const noexcept { return m_browser->renaming(); }
  void selectAll();
  // Selects the rows with these names (E2E).
  void selectNames(const std::vector<std::string>& names);
  void toggleHidden();
  void reload();
  void resolveConflict(FileOpsClient::Resolution resolution);
  void showConflict(const FileOpsClient::Job& job);
  void closeConflict();
  void toggleApplyToAll() { m_applyToAll = !m_applyToAll; }
  [[nodiscard]] bool hasSelection() const noexcept;
  [[nodiscard]] bool hasConflict() const noexcept { return m_conflictJob.has_value(); }

  void onJobChanged(const FileOpsClient::Job& job);
  // A closing window hands its running jobs to another one, so their conflicts and undo still have a home.
  [[nodiscard]] std::set<std::string> takeJobs();
  void adoptJobs(const std::set<std::string>& jobs);

  // Picker mode (`andromeda-files --picker`): the window answers one FileChooser request, then closes.
  void setPicker(std::uint64_t id, PickerPortal::Request request);
  [[nodiscard]] bool isPicker() const noexcept { return m_picker.has_value(); }
  [[nodiscard]] std::optional<std::uint64_t> pickerId() const noexcept;
  void pickerAccept();
  void pickerCancel();
  void setPickerName(std::string name);
  void setPickerFilter(std::size_t index);

private:
  void loadTrash();
  void rebuild();
  // `started` gets the job path once the daemon accepted it.
  void startJob(const char* verb, std::size_t count, std::function<void(FileOpsClient&, FileOpsClient::Started)> start,
                std::function<void(const std::string& job)> started = {});
  void showToast(std::string text, bool undo);
  void onBrowserChanged();
  [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;
  [[nodiscard]] std::unique_ptr<Node> buildContent(Renderer& renderer, float width, float height);
  void registerDropTarget();
  // D14: Ctrl+Shift = link, Ctrl = copy, Shift = move, else move on the same file system and copy across.
  enum class DropOp : std::uint8_t { Move, Copy, Link, Trash };
  [[nodiscard]] static DropOp dropOp(const FileBrowser::DropTarget& target,
                                     const std::vector<std::filesystem::path>& sources, std::uint32_t modifiers);
  void drop(const FileBrowser::DropTarget& target, const std::string& uriList, std::uint32_t action);
  void dropData(const std::filesystem::path& folder, const std::string& mime, const std::string& data);

  FilesApp& m_app;
  std::string m_tagShown; // the tag whose listing shows, re-listed when the index walks again
  AppWindow& m_window;
  // Split view (F3, FILES-UI-SPEC §13): pane 1 exists only while split. m_browser is the active pane: it takes the
  // keys, the header accessory, the title, the path bar and the status.
  std::array<std::unique_ptr<FileBrowser>, 2> m_panes;
  std::unique_ptr<FileBrowser> m_retiredPane;
  FileBrowser* m_browser = nullptr;
  std::size_t m_activePane = 0;
  FileBrowser::Host m_host; // copied into each pane, with its own `pressed`
  void makePane(std::size_t index);
  void toggleSplit();
  void setActivePane(std::size_t index);
  void refreshSplitStatus();
  [[nodiscard]] std::unique_ptr<Node> buildPanes(float scale);
  // Drops: the pane under the point; our own drag's paths, whichever pane started it.
  std::optional<FileBrowser::DropTarget> dragOver(float x, float y);
  [[nodiscard]] const std::vector<std::filesystem::path>& draggedPaths() const;
  std::vector<TrashItem> m_trash;
  FileBrowser::Listing m_trashListing; // built by loadTrash() while another folder shows
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // D-Bus replies skip a closed window
  std::set<std::string> m_jobs; // started by this window, not finished
  std::set<std::string> m_trashReloaded; // other windows' finished jobs the Trash view reloaded for
  std::map<std::string, std::function<void(const std::string&)>> m_renameDone; // rename job -> its inline field
  struct wl_surface* m_dropSurface = nullptr;
  std::uint32_t m_dragMods = 0; // modifiers at the last drag motion
  std::unique_ptr<BatchRenameSheet> m_batchRename;
  std::unique_ptr<ArchiveListing> m_archive; // the archive open as a listing
  std::unique_ptr<CompressSheet> m_compress;
  std::unique_ptr<ExtractSheet> m_extract;
  std::unique_ptr<GetInfoSheet> m_info;
  std::shared_ptr<class NetworkPage> m_network; // made on the first Network visit; discovery keeps running after
  std::shared_ptr<class ConnectSheet> m_connect;
  std::shared_ptr<class AppChooserSheet> m_openWith;
  std::shared_ptr<class ConflictSheet> m_conflict;
  std::unique_ptr<class DetailsPane> m_details;
  bool m_detailsOpen = false;
  std::string m_pathText;  // Go to Folder field
  std::string m_pathError;
  static constexpr std::size_t kPathMatchScan = 4096; // entries read per completion at most
  static constexpr std::size_t kPathMatchesShown = 8;
  [[nodiscard]] std::string absolutePathText() const;
  [[nodiscard]] std::vector<std::string> pathMatches() const;
  void completePath();
  void submitPath();
  [[nodiscard]] std::unique_ptr<Node> buildPathSheet(float scale);
  std::shared_ptr<StoragePage> m_storage;
  struct PendingExtract {
    std::filesystem::path archive;
    ExtractSheet::Request request;
  };
  std::map<std::string, PendingExtract> m_extracts; // by job path: each extract keeps its own retry and follow-up
  std::unique_ptr<class DragIconSurface> m_dragIcon;
  std::unique_ptr<Node> m_dragImage;
  std::unique_ptr<desk::ForeignExport> m_export;
  std::unique_ptr<sdbus::IProxy> m_preview; // org.gnome.NautilusPreviewer2 (andromeda-preview)
  bool m_previewOpen = false;
  void quickLook(const std::filesystem::path& path, bool toggle);
  std::optional<std::string> m_conflictJob;
  std::uint64_t m_conflictShownIndex = 0; // the k of the conflict the sheet shows
  bool m_applyToAll = false;
  std::string m_toast;
  bool m_toastUndo = false;
  Timer m_toastTimer;
  // Job pill (FILES-UI-SPEC §14): this window's unfinished jobs older than kPillDelay.
  static constexpr auto kPillDelay = std::chrono::milliseconds(500);
  std::map<std::string, std::chrono::steady_clock::time_point> m_jobStarted;
  Timer m_pillTimer;
  Node* m_pillAnchor = nullptr;
  class CountdownRing* m_pillRing = nullptr;
  std::size_t m_pillShown = 0;
  [[nodiscard]] std::vector<FileOpsClient::Job> pillJobs() const;
  void updateJobPill();
  void openJobsPopover();
  void armPillTimer();
  // Speed: bytes done over the last kRateWindow of Running samples; paused time and adopted progress do not count.
  static constexpr auto kRateWindow = std::chrono::seconds(3);
  std::map<std::string, std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>> m_rateSamples;
  [[nodiscard]] double jobRate(const std::string& path) const;
  void sampleRate(const FileOpsClient::Job& job);
  std::string m_popoverKey; // job_pill::layoutKey of the open popover's rows
  std::vector<job_pill::RowNodes> m_popoverRows;
  struct Picker {
    std::uint64_t id = 0;
    PickerPortal::Request request;
    std::string name;
    std::optional<std::size_t> filter;
    std::string confirmReplace; // the name the last Save found taken; a second Save of it replaces
    std::vector<PickerPortal::Choice> choices;
    bool nameFocused = false; // the stem is selected and focused once, on the first build
    bool expanded = false;    // Save: false = the compact sheet (name, Tags, Where); true = the full browser
    std::vector<std::string> tags; // Save: tags to set on the file once the app has written it
  };
  [[nodiscard]] std::unique_ptr<Node> buildSaveCompact(float scale);
  void expandPicker();
  class Button* m_pickerPrimary = nullptr; // relabelled in place as the selection changes ("Open 2 Items")
  [[nodiscard]] std::string pickerPrimaryLabel() const;
  void rememberPickerFolder() const;
  std::optional<Picker> m_picker;
  void pickerAnswer(PickerPortal::Answer answer);
  void applyPickerFilter();
  [[nodiscard]] std::unique_ptr<Node> buildPickerFooter(float scale);
};

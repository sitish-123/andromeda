#pragma once

#include "vault_sheet.h"

#include "core/files/file_model.h"
#include "files/file_ops_client.h"
#include "picker_portal.h"
#include "trash.h"
#include "archive_listing.h"
#include "batch_rename_sheet.h"
#include "compress_sheet.h"
#include "folder_compare.h"
#include "extract_sheet.h"
#include "get_info_sheet.h"
#include "job_pill.h"
#include "link_share.h"
#include "service_menus.h"
#include "phone_page.h"
#include "storage_page.h"
#include "ui/files/file_browser.h"

#include <nlohmann/json_fwd.hpp>

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
  void openTagManager(); // the Tags sheet (mockup §17)
  [[nodiscard]] class TagManagerSheet* tagManager() const;
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
  void reopenPlace(const std::string& place); // a listing id saved by rememberPlace
  void toggleStar(); // Ctrl+D on the selection
  [[nodiscard]] bool selectionStarred() const;
  void openTerminalHere(); // the selected folder, else the current one
  void toggleTerminal(); // F4: the terminal pane under the view (mockup §16)
  void slideTerminal(float target);
  void selectByPattern(); // Ctrl+S sheet (mockup §16)
  // Smart folders (mockup §16): saved searches as sidebar places, run again on each visit.
  void newSmartFolder(bool fromSearch); // fromSearch: the current search fills the criteria (Save Search)
  void editSmartFolder();
  void removeSmartFolder();
  void goSmart(const std::string& name);
  [[nodiscard]] bool inSmartFolder() const;
  [[nodiscard]] bool canSaveSearch() const;
  [[nodiscard]] class SmartFolderSheet* smartSheet() const;
  [[nodiscard]] BatchRenameSheet* batchRenameSheet() const;
  [[nodiscard]] CompressSheet* compressSheet() const;
  [[nodiscard]] ExtractSheet* extractSheet() const;
  // Folder compare in split view (mockup §16): marks on both panes; Sync copies the active side's only-here and newer
  // items to the other pane through one copy job (conflicts ask as usual).
  void toggleCompare();
  void toggleSplitView() { toggleSplit(); } // F3
  void activatePane(std::size_t index) {
    if (m_panes[1] != nullptr && index < m_panes.size()) {
      setActivePane(index);
    }
  }
  void syncCompare();
  [[nodiscard]] bool canCompare() const noexcept { return m_panes[1] != nullptr; }
  [[nodiscard]] bool comparing() const noexcept { return m_compareOn; }
  [[nodiscard]] class SelectPatternSheet* patternSheet() const;
  [[nodiscard]] bool terminalFocused() const;
  [[nodiscard]] class TerminalPane* terminal() const { return m_terminal.get(); }
  void copyLocation(); // the selected paths, else the folder, as text on the clipboard
  void showItemMenu(std::size_t index, float sceneX, float sceneY); // right click on an item (mockup §4)
  // A menu row id: share.device:<id>, share.email, open.app:<desktop id>, or a command id.
  void runMenuCommand(const std::string& command, const std::vector<std::filesystem::path>& paths);
  [[nodiscard]] struct ContextMenuControlEntry shareEntry(std::vector<std::string>& ids);
  [[nodiscard]] struct ContextMenuControlEntry openWithEntry(const std::vector<std::filesystem::path>& paths,
                                                             std::vector<std::string>& ids);
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
  void rememberPlace();
  void restoreTabs(const nlohmann::json& saved); // the tabs rememberPlace saved
  // A real folder, not a listing (Trash, Recents, Search, Tag, Starred, archive): paste and new items land in it.
  [[nodiscard]] bool inFolder() const;
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
  void getInfo(std::vector<std::filesystem::path> paths = {}); // empty = the selection, else the folder
  void showStorage();
  void emptyTrash();    // no question: the caller confirmed
  void askEmptyTrash(); // the confirm sheet, then emptyTrash()
  // New › Document templates (~/Templates) and making a file from one, then renaming it.
  [[nodiscard]] static std::vector<std::filesystem::path> templates();
  void newFromTemplate(const std::filesystem::path& file);
  // Share › (mockup §4): each paired Link phone, Email…; the selection goes.
  [[nodiscard]] std::vector<LinkShare::Device> shareDevices() const;
  void shareTo(const std::string& deviceId);
  void sendToPhone(const std::string& deviceId, std::vector<std::filesystem::path> paths);
  // Phone page (FILES-UI-SPEC §20) for a paired Link phone, and its Import view.
  void showPhone(const std::string& deviceId);
  void showPhoneImport(std::shared_ptr<PhonePage> page = nullptr);
  void phonePick(std::size_t index);
  void phoneImport(bool all);
  void shareByEmail();
  // The Share rows as a menu at a scene point (the details pane Share button).
  void showShareMenu(float sceneX, float sceneY);
  void galleryAction(std::string_view action, float sceneX, float sceneY); // the Gallery actions capsule
  // Send to <phone> in the Gallery: the connected paired phone, else the first paired one.
  [[nodiscard]] std::optional<LinkShare::Device> galleryPhone() const;
  // View Options (FILES-UI-SPEC §16) under `anchor`, or under the header sort button (Ctrl+J, menus).
  void openViewOptions(const Node* anchor = nullptr);
  // Add to Sidebar / Remove from Sidebar for selected folders (FB3 pins, Finder ⌃⌘T).
  void togglePin();
  [[nodiscard]] bool selectionPinned() const;
  [[nodiscard]] bool selectionIsFolders() const;
  // Vaults (FB-VAULT): New Vault sheet, Unlock sheet for a locked vault, Lock from the sidebar eject button.
  void askNewVault(const std::string& name = {});
  void askUnlockVault(const std::string& name);
  void lockVault(const std::string& name);
  void submitSheetPassword(const std::string& password); // sheet.password: the open Unlock/New Vault sheet
  void presentVaultSheet(VaultSheet::Mode mode, const std::string& name);
  // Drives (FB-BASICS): mount an unmounted drive (password sheet for an encrypted one), Import Photos from a DCIM
  // tree, Format a removable drive.
  void mountDrive(const std::string& id);
  [[nodiscard]] std::filesystem::path importSource() const;
  void importPhotos();
  void importFiles(const std::filesystem::path& dcim, std::vector<std::filesystem::path> files);
  [[nodiscard]] std::string formatTarget() const;
  void askFormat();
  void eraseDrive(); // the Erase button of the open Format sheet
  void askDeleteSelection(); // Shift+Del: delete for good after a confirm, no Trash, no undo
  [[nodiscard]] bool trashHasItems() const noexcept { return !m_trash.empty(); }
  void extractSelection(bool here);
  [[nodiscard]] bool canExtract() const;
  [[nodiscard]] bool renaming() const noexcept { return m_browser->renaming(); }
  void selectAll();
  // Selects the rows with these names (E2E).
  void selectNames(const std::vector<std::string>& names);
  void showInGallery(const std::string& name);
  void setView(FileBrowser::View view);
  void toggleHidden();
  void groupBy(FileGroupBy group); // Tags first asks the tag index for a fresh walk
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
  std::shared_ptr<class TagManagerSheet> m_tagManager;
  std::unique_ptr<class TerminalPane> m_terminal; // kept while hidden, as Dolphin keeps its session
  // Tag renames and deletes on files run one at a time, each after a fresh walk, so a second edit sees the first.
  struct TagFileOp {
    std::string tag;
    std::vector<std::string> tags;
    std::string mode;
  };
  std::deque<TagFileOp> m_tagOps;
  bool m_tagOpRunning = false;
  bool m_terminalOpen = false;
  std::shared_ptr<class SelectPatternSheet> m_patternSheet;
  std::string m_lastPattern = "*";
  bool m_lastPatternRegex = false;
  bool m_compareOn = false;
  std::shared_ptr<class SmartFolderSheet> m_smartSheet;
  std::string m_smartShown;
  void presentSmartSheet(struct SmartFolder folder, std::string original);
  folder_compare::Result m_compare; // the last compare that landed
  std::uint64_t m_compareRun = 0;   // a newer run drops older answers
  std::deque<std::pair<std::filesystem::path, std::vector<std::filesystem::path>>> m_syncQueue; // into, sources
  void runSyncQueue();
  void refreshCompare();
  std::pair<std::filesystem::path, std::filesystem::path> m_comparedDirs; // the panes' folders at the last compare
  void runTagOps();
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
  std::vector<service_menus::Action> m_serviceActions; // the last item menu's Actions rows
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // D-Bus replies skip a closed window
  std::set<std::string> m_jobs; // started by this window, not finished
  std::map<std::string, FileOpsClient::Job> m_phoneJobs; // Link transfers this window started, by "link:<id>"
  std::set<std::string> m_trashReloaded; // other windows' finished jobs the Trash view reloaded for
  std::map<std::string, std::function<void(const std::string&)>> m_renameDone; // rename job -> its inline field
  struct wl_surface* m_dropSurface = nullptr;
  std::uint32_t m_dragMods = 0; // modifiers at the last drag motion
  std::unique_ptr<BatchRenameSheet> m_batchRename;
  std::unique_ptr<ArchiveListing> m_archive; // the archive open as a listing
  std::unique_ptr<CompressSheet> m_compress;
  std::unique_ptr<ExtractSheet> m_extract;
  std::string m_archivePassword; // the last password an extract was started with: a drag-out from it uses it
  struct DragExtract {
    std::filesystem::path dir;
    bool done = false;
    std::vector<std::function<void()>> waiting; // drops in this window wait for the extract
  };
  std::optional<DragExtract> m_dragExtract;
  [[nodiscard]] std::vector<std::filesystem::path> dragOutOfArchive(const std::vector<std::filesystem::path>& dragged);
  std::unique_ptr<GetInfoSheet> m_info;
  std::shared_ptr<class NetworkPage> m_network; // made on the first Network visit; discovery keeps running after
  std::shared_ptr<class ConnectSheet> m_connect;
  std::shared_ptr<class VaultSheet> m_vaultSheet;
  std::string m_formatName;
  std::string m_formatDevice; // set while the Format sheet is open
  std::vector<std::string> m_formatTypes;
  std::size_t m_formatType = 0;
  std::string m_formatError;
  bool m_formatBusy = false;
  std::shared_ptr<class AppChooserSheet> m_openWith;
  std::shared_ptr<class ConflictSheet> m_conflict;
  std::unique_ptr<class DetailsPane> m_details;
  bool m_detailsOpen = false;
  float m_detailsMix = 1.0F;    // the shown share of the details pane (slide)
  float m_detailsTarget = 1.0F;
  AnimationManager::Id m_detailsAnim = 0;
  class SlideClip* m_detailsClip = nullptr;
  float m_terminalMix = 1.0F;
  AnimationManager::Id m_terminalAnim = 0;
  class SlideClip* m_terminalClip = nullptr;
  float m_splitMix = 1.0F;
  AnimationManager::Id m_splitAnim = 0;
  Node* m_splitNode = nullptr;
  bool m_resizeJobs = false;
  std::string m_pathText;  // Go to Folder field
  std::string m_pathError;
  static constexpr std::size_t kPathMatchScan = 4096; // subfolders kept per completion; 16 times as many entries read at most
  static constexpr std::size_t kPathMatchesShown = 8;
  [[nodiscard]] std::string absolutePathText() const;
  [[nodiscard]] std::vector<std::string> pathMatches() const;
  // Go to Folder completion: the typed folder's subfolder names, listed on a worker (a hung mount must not freeze
  // typing); the sheet rebuilds when they are in.
  mutable std::filesystem::path m_pathListDir;
  mutable std::vector<std::string> m_pathListNames;
  mutable std::filesystem::path m_pathListAsked; // the newest folder asked for; a hung older listing is ignored
  void completePath();
  void submitPath();
  [[nodiscard]] std::unique_ptr<Node> buildPathSheet(float scale);
  std::shared_ptr<StoragePage> m_storage;
  std::shared_ptr<PhonePage> m_phone;
  bool m_dragHasUris = false; // the drag over this window offers text/uri-list
  struct PendingExtract {
    std::filesystem::path archive;
    ExtractSheet::Request request;
  };
  std::map<std::string, PendingExtract> m_extracts; // by job path: each extract keeps its own retry and follow-up
  std::unique_ptr<class DragIconSurface> m_dragIcon;
  std::unique_ptr<Node> m_dragImage;
  std::unique_ptr<desk::ForeignExport> m_export;
  std::unique_ptr<class GalleryVideo> m_galleryVideo; // created on the first video the Gallery plays
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

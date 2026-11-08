#pragma once

#include "app/desk_app.h"
#include "core/files/directory_watcher.h"
#include "core/timer_manager.h"
#include "render/core/thumbnail_service.h"
#include "files/file_ops_client.h"
#include "app_chooser_portal.h"
#include "app_chooser_sheet.h"
#include "file_manager1.h"
#include "index_search.h"
#include "link_share.h"
#include "picker_portal.h"
#include "portal_sheets.h"
#include "starred.h"
#include "vaults.h"
#include "drives.h"
#include "system_portals.h"
#include "tags.h"

#include <chrono>
#include <filesystem>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class FilesWindow;

// andromeda-files: one process per user (DeskApp single instance); every window shares the folder models, the
// watcher and the FileOps1 client. A launch with folders opens one window per folder.
class FilesApp final : public DeskApp {
public:
  struct Clipboard {
    std::vector<std::filesystem::path> paths;
    bool cut = false;
  };

  // picker: `andromeda-files --picker`, the FileChooser portal service (app id dev.andromeda.Files.Picker).
  // service: started by D-Bus activation of org.freedesktop.FileManager1; it opens no window of its own.
  FilesApp(bool picker, bool service = false);
  ~FilesApp() override;

  [[nodiscard]] DirectoryWatcher& watcher() noexcept { return m_watcher; }
  // Null without a session bus (operations are off then).
  [[nodiscard]] FileOpsClient* fileOps() noexcept { return m_fileOps.get(); }
  [[nodiscard]] Clipboard& clipboard() noexcept { return m_clipboard; }
  [[nodiscard]] ThumbnailService& thumbnails() noexcept { return m_thumbnails; }
  // One undo stack for the app (D10): finished undoable jobs of every window, newest last.
  [[nodiscard]] std::vector<std::string>& undoStack() noexcept { return m_undo; }

  FilesWindow& openWindow(const std::filesystem::path& location);
  // The FilesWindow of the active AppWindow, or null.
  [[nodiscard]] FilesWindow* activeFilesWindow() const;
  void refreshPlaces(); // sidebar drives and Trash count in every window
  // A Save picker chose tags: set them once the asking app has written the file (polled up to kTagWaitTime).
  void tagWhenWritten(const std::filesystem::path& file, std::vector<std::string> tags);
  // An eject finished: the active window shows an error; on success every window leaves the drive.
  void unmounted(const std::filesystem::path& mount, const std::string& error);
  [[nodiscard]] TagIndex& tags() { return *m_tags; }
  [[nodiscard]] Starred& starred() noexcept { return m_starred; }
  [[nodiscard]] Starred& pins() noexcept { return m_pins; } // folders added to the sidebar
  [[nodiscard]] Vaults& vaults() noexcept { return m_vaults; }
  [[nodiscard]] Drives* drives() noexcept { return m_drives.get(); } // null until shortly after the first window
  [[nodiscard]] PhotoImport& imports() noexcept { return m_imports; }
  // Search This PC: every window pane gets the answer (FILES-UI-SPEC §15).
  [[nodiscard]] IndexSearch& index() noexcept { return m_index; }
  [[nodiscard]] LinkShare* linkShare() noexcept { return m_linkShare.get(); } // null without a session bus
  void pickerAnswered(std::uint64_t id, const PickerPortal::Answer& answer);
  // An Open picker window of this app (not the portal): done gets the chosen files, empty when cancelled.
  void pickFiles(std::string title, std::string acceptLabel,
                 std::function<void(std::vector<std::filesystem::path>)> done, bool directory = false);
  void searchAnswered(const std::string& text, std::vector<std::shared_ptr<const FileInfo>> rows);

protected:
  void startup() override;
  void activate(const std::string& activationToken) override;
  void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
  void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) override;
  AppWindow& createWindow() override;
  void windowClosed(AppWindow& window) override;
  // A service start quits with its last window once it opened one (idle quit covers a start with no request).
  [[nodiscard]] bool quitWithLastWindow() const override { return !m_picker && (!m_service || m_hadWindow); }

private:
  void addCommands();
  void onJobChanged(const FileOpsClient::Job& job);
  void openPicker(std::uint64_t id, const PickerPortal::Request& request);
  void closePicker(std::uint64_t id);
  void openChooser(std::uint64_t id, const AppChooserPortal::ChooseRequest& request);
  void openAccess(std::uint64_t id, const AppChooserPortal::AccessRequest& request);
  void openPortalSheet(std::uint64_t id, const SystemPortals::Request& request);
  void dialogAction(const std::string& name, const std::vector<std::string>& parameter);
  void tagAction(FilesWindow& window, const std::string& name, const std::vector<std::string>& parameter);
  void scheduleIdleQuit();

  DirectoryWatcher m_watcher;
  ThumbnailService m_thumbnails;
  std::unique_ptr<FileOpsClient> m_fileOps;
  Clipboard m_clipboard;
  std::vector<std::string> m_undo;
  std::map<AppWindow*, std::unique_ptr<FilesWindow>> m_windows;
  std::filesystem::path m_nextLocation;
  bool m_picker = false;
  // The next window is a Photos window (action gallery): its own app id, title and saved size.
  bool m_nextPhotos = false;
  std::unique_ptr<PickerPortal> m_portal;
  std::unique_ptr<AppChooserPortal> m_chooserPortal;
  std::unique_ptr<SystemPortals> m_systemPortals;
  IndexSearch m_index;
  std::unique_ptr<FileManager1> m_fileManager;
  std::unique_ptr<LinkShare> m_linkShare;
  bool m_service = false;
  bool m_hadWindow = false;
  // pickFiles requests by id; ids start high so they never meet a portal request id.
  std::map<std::uint64_t, std::function<void(std::vector<std::filesystem::path>)>> m_localPicks;
  std::uint64_t m_nextLocalPick = std::uint64_t{1} << 62;
  void showFromFileManager(FileManager1::Show show, const std::vector<std::string>& uris, const std::string& startupId);
  struct Dialog { // an Open With or Access window and its portal request
    std::uint64_t id = 0;
    std::unique_ptr<AppChooserSheet> chooser;
    std::unique_ptr<AccessSheet> access;
    std::unique_ptr<PortalSheet> portal;
    bool answered = false;
  };
  std::map<AppWindow*, Dialog> m_dialogs;
  Timer m_idleQuit;
  struct PendingTags {
    std::filesystem::path file;
    std::vector<std::string> tags;
    std::chrono::steady_clock::time_point deadline;
    // The file before the app wrote it (Replace): tags wait for a new inode or mtime, never the old file.
    bool existed = false;
    std::uint64_t inode = 0;
    std::int64_t mtimeNs = 0;
  };
  std::vector<PendingTags> m_pendingTags;
  Timer m_tagPoll;
  static constexpr auto kTagWaitTime = std::chrono::minutes(2);
  std::shared_ptr<TagIndex> m_tags; // shared_from_this: the walk outlives a closed window
  Starred m_starred;
  Starred m_pins{"pins.json"};
  Vaults m_vaults;
  PhotoImport m_imports;
  std::unique_ptr<Drives> m_drives;
  Timer m_drivesStart;
  GUnixMountMonitor* m_mounts = nullptr; // mounts-changed refreshes the sidebar drives
  std::set<std::string> m_movesApplied; // jobs whose moves the starred and pinned paths already followed
};

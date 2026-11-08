#pragma once

#include "app/desk_app.h"
#include "core/files/directory_watcher.h"
#include "core/timer_manager.h"
#include "render/core/thumbnail_service.h"
#include "files/file_ops_client.h"
#include "app_chooser_portal.h"
#include "app_chooser_sheet.h"
#include "picker_portal.h"
#include "portal_sheets.h"
#include "starred.h"
#include "system_portals.h"
#include "tags.h"

#include <chrono>
#include <filesystem>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <map>
#include <memory>
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
  explicit FilesApp(bool picker);
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
  void pickerAnswered(std::uint64_t id, const PickerPortal::Answer& answer);

protected:
  void startup() override;
  void activate(const std::string& activationToken) override;
  void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
  void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) override;
  AppWindow& createWindow() override;
  void windowClosed(AppWindow& window) override;
  [[nodiscard]] bool quitWithLastWindow() const override { return !m_picker; }

private:
  void addCommands();
  void onJobChanged(const FileOpsClient::Job& job);
  void openPicker(std::uint64_t id, const PickerPortal::Request& request);
  void closePicker(std::uint64_t id);
  void openChooser(std::uint64_t id, const AppChooserPortal::ChooseRequest& request);
  void openAccess(std::uint64_t id, const AppChooserPortal::AccessRequest& request);
  void openPortalSheet(std::uint64_t id, const SystemPortals::Request& request);
  void dialogAction(const std::string& name, const std::vector<std::string>& parameter);
  void scheduleIdleQuit();

  DirectoryWatcher m_watcher;
  ThumbnailService m_thumbnails;
  std::unique_ptr<FileOpsClient> m_fileOps;
  Clipboard m_clipboard;
  std::vector<std::string> m_undo;
  std::map<AppWindow*, std::unique_ptr<FilesWindow>> m_windows;
  std::filesystem::path m_nextLocation;
  bool m_picker = false;
  std::unique_ptr<PickerPortal> m_portal;
  std::unique_ptr<AppChooserPortal> m_chooserPortal;
  std::unique_ptr<SystemPortals> m_systemPortals;
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
  GUnixMountMonitor* m_mounts = nullptr; // mounts-changed refreshes the sidebar drives
};

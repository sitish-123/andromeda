#pragma once

#include "app/desk_app.h"
#include "files/directory_watcher.h"
#include "core/timer_manager.h"
#include "files/file_ops_client.h"
#include "picker_portal.h"

#include <filesystem>
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

  FilesWindow& openWindow(const std::filesystem::path& location);
  // The FilesWindow of the active AppWindow, or null.
  [[nodiscard]] FilesWindow* activeFilesWindow() const;
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
  void scheduleIdleQuit();

  DirectoryWatcher m_watcher;
  std::unique_ptr<FileOpsClient> m_fileOps;
  Clipboard m_clipboard;
  std::map<AppWindow*, std::unique_ptr<FilesWindow>> m_windows;
  std::filesystem::path m_nextLocation;
  bool m_picker = false;
  std::unique_ptr<PickerPortal> m_portal;
  Timer m_idleQuit;
};

#pragma once

#include "core/files/file_model.h"
#include "files/file_ops_client.h"
#include "picker_portal.h"
#include "trash.h"
#include "ui/files/file_browser.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/timer_manager.h"

class AppWindow;
class Node;
class FilesApp;
struct KeyboardEvent;

// One Files window: a FileBrowser (location, history, views, selection) plus what Files adds around it: the Trash
// listing, jobs with conflicts and undo, the toast, and picker mode.
class FilesWindow {
public:
  FilesWindow(FilesApp& app, AppWindow& window, std::filesystem::path location);
  ~FilesWindow();

  FilesWindow(const FilesWindow&) = delete;
  FilesWindow& operator=(const FilesWindow&) = delete;

  void go(const std::filesystem::path& location);
  void goTrash();
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
  void trashSelection();
  void restoreSelection();
  void undo();
  [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
  void selectAll();
  // Selects the rows with these names (E2E).
  void selectNames(const std::vector<std::string>& names);
  void toggleHidden();
  void reload();
  void resolveConflict(FileOpsClient::Resolution resolution);
  void toggleApplyToAll() { m_applyToAll = !m_applyToAll; rebuild(); }
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
  void startJob(const char* verb, std::size_t count, std::function<void(FileOpsClient&, FileOpsClient::Started)> start);
  void showToast(std::string text, bool undo);
  void onBrowserChanged();
  [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;
  [[nodiscard]] std::unique_ptr<Node> buildContent();

  FilesApp& m_app;
  AppWindow& m_window;
  std::unique_ptr<FileBrowser> m_browser;
  std::vector<TrashItem> m_trash;
  FileBrowser::Listing m_trashListing; // built by loadTrash() while another folder shows
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // D-Bus replies skip a closed window
  std::set<std::string> m_jobs; // started by this window, not finished
  std::set<std::string> m_trashReloaded; // other windows' finished jobs the Trash view reloaded for
  std::vector<std::string> m_undo; // finished undoable jobs, newest last
  std::optional<std::string> m_conflictJob;
  bool m_applyToAll = false;
  std::string m_toast;
  bool m_toastUndo = false;
  Timer m_toastTimer;
  struct Picker {
    std::uint64_t id = 0;
    PickerPortal::Request request;
    std::string name;
    std::optional<std::size_t> filter;
    std::string confirmReplace; // the name the last Save found taken; a second Save of it replaces
  };
  std::optional<Picker> m_picker;
  void pickerAnswer(PickerPortal::Answer answer);
  void applyPickerFilter();
  [[nodiscard]] std::unique_ptr<Node> buildPickerFooter(float scale);
};

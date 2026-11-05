#pragma once

#include "files/file_model.h"
#include "files/file_ops_client.h"
#include "picker_portal.h"
#include "trash.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_list_view.h"

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

// One Files window: a location (a folder or the Trash), its history, selection, and the undo stack of the jobs it
// started (docs/apps.md §2 "per-window undo stack").
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
  [[nodiscard]] bool canBack() const noexcept { return !m_back.empty(); }
  [[nodiscard]] bool canForward() const noexcept { return !m_forward.empty(); }
  [[nodiscard]] bool inTrash() const noexcept { return m_inTrash; }
  [[nodiscard]] const std::filesystem::path& location() const noexcept { return m_location; }
  [[nodiscard]] float scale() const noexcept;

  void openSelection();
  void copySelection(bool cut);
  void paste();
  void trashSelection();
  void restoreSelection();
  void undo();
  [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
  void selectAll();
  // Selects the rows with these names (E2E and the type-ahead).
  void selectNames(const std::vector<std::string>& names);
  void toggleHidden();
  void reload();
  void resolveConflict(FileOpsClient::Resolution resolution);
  void toggleApplyToAll() { m_applyToAll = !m_applyToAll; rebuild(); }
  [[nodiscard]] bool hasSelection() const noexcept { return !m_selected.empty(); }
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
  class Adapter;

  void load();
  void rebuild();
  void startJob(const char* verb, std::size_t count, std::function<void(FileOpsClient&, FileOpsClient::Started)> start);
  void showToast(std::string text, bool undo);
  bool onKey(const KeyboardEvent& event);
  [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;
  [[nodiscard]] std::vector<std::shared_ptr<const FileInfo>> rows() const;
  [[nodiscard]] std::string title() const;
  void select(std::size_t index, std::uint32_t modifiers);

  FilesApp& m_app;
  AppWindow& m_window;
  std::filesystem::path m_location;
  bool m_inTrash = false;
  std::vector<std::pair<std::filesystem::path, bool>> m_back; // (location, trash)
  std::vector<std::pair<std::filesystem::path, bool>> m_forward;
  std::shared_ptr<FileModel> m_model;
  std::unique_ptr<FileModelView> m_view;
  std::vector<TrashItem> m_trash;
  std::vector<std::shared_ptr<const FileInfo>> m_trashRows; // parallel to m_trash
  std::set<std::string> m_selected; // row names (trash: the path under files/, unique across trash dirs)
  // Row names, not indices: a model change can move rows.
  std::optional<std::string> m_anchor; // start of a Shift range
  std::optional<std::string> m_cursor; // the row arrow keys move from
  std::vector<std::string> m_pendingSelect; // selectNames() before the listing is in
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // D-Bus replies skip a closed window
  [[nodiscard]] std::optional<std::size_t> indexOf(const std::string& name) const;
  std::optional<std::size_t> m_hovered;
  bool m_showHidden = false;
  std::set<std::string> m_jobs; // started by this window, not finished
  std::set<std::string> m_trashReloaded; // other windows' finished jobs the Trash view reloaded for
  std::vector<std::string> m_undo; // finished undoable jobs, newest last
  std::optional<std::string> m_conflictJob;
  bool m_applyToAll = false;
  std::string m_toast;
  bool m_toastUndo = false;
  Timer m_toastTimer;
  std::string m_typeAhead;
  std::chrono::steady_clock::time_point m_typeAheadAt;
  ScrollViewState m_scroll;
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
  std::unique_ptr<Adapter> m_adapter;
};

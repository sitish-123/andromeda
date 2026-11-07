#include "files_app.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "files_window.h"

#include <gio/gio.h>
#include <glib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files");

  // A chooser browses and picks; it never changes files.
  bool allowedInPicker(std::string_view id) {
    return (id.starts_with("go.") && id != "go.trash") || id.starts_with("view.") || id.starts_with("picker.")
        || id == "file.open" || id == "edit.select-all";
  }

  fs::path pathFromUri(const std::string& uri) {
    if (uri.find("://") == std::string::npos) {
      std::error_code ec;
      const fs::path abs = fs::absolute(uri, ec);
      return ec ? fs::path(uri) : abs.lexically_normal();
    }
    gchar* file = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
    fs::path out = file != nullptr ? file : "";
    g_free(file);
    return out;
  }
} // namespace

FilesApp::FilesApp(bool picker)
    : DeskApp({.id = picker ? "dev.andromeda.Files.Picker" : "dev.andromeda.Files", .name = "Files"}), m_picker(picker) {}

FilesApp::~FilesApp() = default;

void FilesApp::startup() {
  loop().addSource(&m_watcher);
  if (bus() != nullptr) {
    m_fileOps = std::make_unique<FileOpsClient>(*bus());
    m_fileOps->setJobChangedCallback([this](const FileOpsClient::Job& job) { onJobChanged(job); });
  } else {
    kLog.warn("no session bus: file operations are off");
  }
  addCommands();
  if (m_picker) {
    if (bus() == nullptr) {
      kLog.error("the picker needs the session bus");
      quit(1);
      return;
    }
    m_portal = std::make_unique<PickerPortal>(
        *bus(), [this](std::uint64_t id, const PickerPortal::Request& request) { openPicker(id, request); },
        [this](std::uint64_t id) { closePicker(id); }
    );
    scheduleIdleQuit();
  }
}

void FilesApp::scheduleIdleQuit() {
  // D-Bus activation starts the picker again for the next request.
  m_idleQuit.start(std::chrono::minutes(2), [this] {
    if (windows().empty() && m_portal->idle()) {
      quit(0);
    } else {
      scheduleIdleQuit();
    }
  });
}

void FilesApp::openPicker(std::uint64_t id, const PickerPortal::Request& request) {
  using Kind = PickerPortal::Request::Kind;
  const std::string title = !request.title.empty() ? request.title
      : request.kind == Kind::Open                    ? (request.directory ? "Select Folder" : "Open File")
                                                      : "Save File";
  AppWindow& window = addWindow({
      .appId = info().id,
      .title = title,
      .stateKey = "picker",
      .width = 800,
      .height = 520,
      .navigation = true,
      .sidebarWidth = 200.0F,
      .parentWindow = request.parentWindow,
  });
  std::error_code ec;
  const fs::path folder = !request.currentFolder.empty() && fs::is_directory(request.currentFolder, ec)
      ? request.currentFolder
      : fs::path(g_get_home_dir());
  auto& files = m_windows[&window] = std::make_unique<FilesWindow>(*this, window, folder);
  files->setPicker(id, request);
  if (!window.open()) {
    pickerAnswered(id, {.response = 2, .uris = {}, .filter = {}});
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::closePicker(std::uint64_t id) {
  for (auto& [appWindow, window] : m_windows) {
    if (window->pickerId() == id) {
      closeWindow(*appWindow);
    }
  }
}

void FilesApp::pickerAnswered(std::uint64_t id, const PickerPortal::Answer& answer) {
  if (m_portal != nullptr) {
    m_portal->answer(id, answer);
  }
}

AppWindow& FilesApp::createWindow() {
  AppWindow& window = addWindow({
      .appId = info().id,
      .title = info().name,
      .navigation = true,
      .sidebarWidth = 220.0F,
  });
  fs::path location = m_nextLocation.empty() ? fs::path(g_get_home_dir()) : m_nextLocation;
  m_nextLocation.clear();
  m_windows[&window] = std::make_unique<FilesWindow>(*this, window, std::move(location));
  return window;
}

void FilesApp::windowClosed(AppWindow& window) {
  if (const auto it = m_windows.find(&window); it != m_windows.end()) {
    if (const auto id = it->second->pickerId()) {
      // Closed from the window frame or Ctrl+W: the request ends as cancelled.
      pickerAnswered(*id, {.response = 1, .uris = {}, .filter = {}});
    }
    for (auto& [other, files] : m_windows) {
      if (other != &window && other->isOpen() && !files->isPicker()) {
        files->adoptJobs(it->second->takeJobs());
        break;
      }
    }
    m_windows.erase(it);
  }
}

FilesWindow& FilesApp::openWindow(const fs::path& location) {
  m_nextLocation = location;
  AppWindow& window = createWindow();
  window.open();
  return *m_windows.at(&window);
}

FilesWindow* FilesApp::activeFilesWindow() const {
  AppWindow* window = activeWindow();
  if (window == nullptr) {
    return nullptr;
  }
  const auto it = m_windows.find(window);
  return it != m_windows.end() ? it->second.get() : nullptr;
}

void FilesApp::activate(const std::string& activationToken) {
  if (!m_picker) {
    DeskApp::activate(activationToken);
  }
}

void FilesApp::open(const std::vector<std::string>& uris, const std::string& activationToken) {
  if (m_picker) {
    return;
  }
  for (const std::string& uri : uris) {
    fs::path path = pathFromUri(uri);
    if (path.empty()) {
      kLog.warn("cannot open {}", uri);
      continue;
    }
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
      // A file opens its folder; selecting it there is the reveal behaviour.
      const std::string name = path.filename().string();
      FilesWindow& w = openWindow(path.parent_path());
      w.selectNames({name});
    } else {
      openWindow(path);
    }
    if (windows().back()->isOpen()) {
      windows().back()->activate(activationToken);
    }
  }
  if (windows().empty()) {
    activate(activationToken);
  }
}

void FilesApp::action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) {
  FilesWindow* window = activeFilesWindow();
  if (name == "go" && window != nullptr && !parameter.empty()) {
    window->go(pathFromUri(parameter.front()));
  } else if (name == "trash" && window != nullptr && !window->isPicker()) {
    window->goTrash();
  } else if (name == "select" && window != nullptr) {
    window->selectNames(parameter);
  } else if (name == "picker.name" && window != nullptr && !parameter.empty()) {
    window->setPickerName(parameter.front());
  } else {
    DeskApp::action(name, parameter, activationToken);
    return;
  }
  if (AppWindow* w = activeWindow()) {
    w->activate(activationToken);
  }
}

void FilesApp::onJobChanged(const FileOpsClient::Job& job) {
  for (auto& [appWindow, window] : m_windows) {
    window->onJobChanged(job);
  }
}

void FilesApp::addCommands() {
  const auto chord = [](std::uint32_t sym, std::uint32_t mods) { return KeyChord{.sym = sym, .modifiers = mods}; };
  struct Spec {
    const char* id;
    const char* label;
    const char* group;
    std::optional<KeyChord> shortcut;
    bool (*enabled)(const FilesWindow&);
    void (*run)(FilesWindow&);
  };
  const auto always = [](const FilesWindow&) { return true; };
  const auto selection = [](const FilesWindow& w) { return w.hasSelection(); };
  const auto conflict = [](const FilesWindow& w) { return w.hasConflict(); };
  const Spec specs[] = {
      {"go.back", "Back", "Go", chord(XKB_KEY_Left, KeyMod::Alt), [](const FilesWindow& w) { return w.canBack(); },
       [](FilesWindow& w) { w.back(); }},
      {"go.forward", "Forward", "Go", chord(XKB_KEY_Right, KeyMod::Alt),
       [](const FilesWindow& w) { return w.canForward(); }, [](FilesWindow& w) { w.forward(); }},
      {"go.up", "Enclosing Folder", "Go", chord(XKB_KEY_Up, KeyMod::Alt), [](const FilesWindow& w) { return !w.inTrash(); },
       [](FilesWindow& w) { w.up(); }},
      {"go.home", "Home", "Go", chord(XKB_KEY_Home, KeyMod::Alt), always,
       [](FilesWindow& w) { w.go(g_get_home_dir()); }},
      {"go.trash", "Trash", "Go", std::nullopt, always, [](FilesWindow& w) { w.goTrash(); }},
      {"file.open", "Open", "File", chord(XKB_KEY_Return, 0), selection, [](FilesWindow& w) { w.openSelection(); }},
      {"file.restore", "Put Back", "File", std::nullopt,
       [](const FilesWindow& w) { return w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.restoreSelection(); }},
      {"edit.copy", "Copy", "Edit", chord(XKB_KEY_c, KeyMod::Ctrl), selection, [](FilesWindow& w) { w.copySelection(false); }},
      {"edit.cut", "Cut", "Edit", chord(XKB_KEY_x, KeyMod::Ctrl), selection, [](FilesWindow& w) { w.copySelection(true); }},
      {"edit.paste", "Paste", "Edit", chord(XKB_KEY_v, KeyMod::Ctrl), [](const FilesWindow& w) { return !w.inTrash(); },
       [](FilesWindow& w) { w.paste(); }},
      {"edit.trash", "Move to Trash", "Edit", chord(XKB_KEY_Delete, 0),
       [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.trashSelection(); }},
      {"edit.undo", "Undo", "Edit", chord(XKB_KEY_z, KeyMod::Ctrl), [](const FilesWindow& w) { return w.canUndo(); },
       [](FilesWindow& w) { w.undo(); }},
      {"edit.select-all", "Select All", "Edit", chord(XKB_KEY_a, KeyMod::Ctrl), always, [](FilesWindow& w) { w.selectAll(); }},
      {"view.hidden", "Show Hidden Files", "View", chord(XKB_KEY_h, KeyMod::Ctrl), always,
       [](FilesWindow& w) { w.toggleHidden(); }},
      {"view.reload", "Reload", "View", chord(XKB_KEY_F5, 0), always, [](FilesWindow& w) { w.reload(); }},
      {"picker.accept", "Choose", "Picker", std::nullopt, [](const FilesWindow& w) { return w.isPicker(); },
       [](FilesWindow& w) { w.pickerAccept(); }},
      {"picker.cancel", "Cancel", "Picker", chord(XKB_KEY_Escape, 0), [](const FilesWindow& w) { return w.isPicker(); },
       [](FilesWindow& w) { w.pickerCancel(); }},
      {"conflict.replace", "Replace", "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.resolveConflict(FileOpsClient::Resolution::Replace); }},
      {"conflict.skip", "Skip", "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.resolveConflict(FileOpsClient::Resolution::Skip); }},
      {"conflict.keep-both", "Keep Both", "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.resolveConflict(FileOpsClient::Resolution::KeepBoth); }},
      {"conflict.apply-all", "Apply to All", "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.toggleApplyToAll(); }},
  };
  for (const Spec& spec : specs) {
    commands().add({
        .id = spec.id,
        .label = spec.label,
        .group = spec.group,
        .shortcut = spec.shortcut,
        .enabled = [this, enabled = spec.enabled, chooser = allowedInPicker(spec.id)] {
          const FilesWindow* w = activeFilesWindow();
          return w != nullptr && (chooser || !w->isPicker()) && enabled(*w);
        },
        .run = [this, run = spec.run] {
          if (FilesWindow* w = activeFilesWindow()) {
            run(*w);
          }
        },
    });
  }
  commands().add({
      .id = "file.new-window",
      .label = "New Window",
      .group = "File",
      .shortcut = chord(XKB_KEY_n, KeyMod::Ctrl),
      .enabled = [this] { return !m_picker; },
      .run = [this] {
        const FilesWindow* w = activeFilesWindow();
        openWindow(w != nullptr && !w->inTrash() ? w->location() : fs::path(g_get_home_dir()));
      },
  });
}

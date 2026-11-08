#include "files_app.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "files_window.h"

#include <algorithm>
#include <gio/gio.h>
#include <sys/stat.h>
#include <gio/gunixmounts.h>
#include <glib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files");

  // A chooser browses and picks; it never changes files.
  // The compact Save sheet (mockup §6 dialog 456 wide): header, three field rows, buttons.
  constexpr std::uint32_t kSaveWidth = 456;
  constexpr std::uint32_t kSaveHeight = 272;
  // Open With (mockup §4) and the Access dialog (FILES-UI-SPEC §11).
  constexpr std::uint32_t kChooserWidth = 520;
  constexpr std::uint32_t kChooserHeight = 560;
  constexpr std::uint32_t kAccessWidth = 400;
  constexpr std::uint32_t kAccessHeight = 280;
  constexpr std::uint32_t kDialogMinHeight = 240;
  constexpr std::uint32_t kPortalHeight = 360;
  constexpr std::uint32_t kWallpaperWidth = 400;
  constexpr std::uint32_t kWallpaperHeight = 420;

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

FilesApp::~FilesApp() {
  if (m_mounts != nullptr) {
    g_signal_handlers_disconnect_by_data(m_mounts, this);
    g_object_unref(m_mounts);
  }
}

void FilesApp::startup() {
  loop().addSource(&m_watcher);
  if (bus() != nullptr) {
    m_fileOps = std::make_unique<FileOpsClient>(*bus());
    m_fileOps->setJobChangedCallback([this](const FileOpsClient::Job& job) { onJobChanged(job); });
  } else {
    kLog.warn("no session bus: file operations are off");
  }
  loop().addSource(&m_thumbnails);
  m_tags = std::make_shared<TagIndex>([this] {
    for (auto& [window, files] : m_windows) {
      files->tagsChanged();
    }
  });
  // The kernel mount table, watched locally: GVolumeMonitor would start gvfs monitors over D-Bus at startup.
  m_mounts = g_unix_mount_monitor_get();
  const auto onMounts = +[](GUnixMountMonitor*, gpointer self) { static_cast<FilesApp*>(self)->refreshPlaces(); };
  g_signal_connect(m_mounts, "mounts-changed", G_CALLBACK(onMounts), this);
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
    m_chooserPortal = std::make_unique<AppChooserPortal>(*bus(), AppChooserPortal::Handlers{
        .choose = [this](std::uint64_t id, const AppChooserPortal::ChooseRequest& r) { openChooser(id, r); },
        .updateChoices =
            [this](std::uint64_t id, const std::vector<std::string>& choices) {
              for (auto& [window, dialog] : m_dialogs) {
                if (dialog.id == id && dialog.chooser != nullptr) {
                  dialog.chooser->updateChoices(choices);
                }
              }
            },
        .access = [this](std::uint64_t id, const AppChooserPortal::AccessRequest& r) { openAccess(id, r); },
        .closed =
            [this](std::uint64_t id) {
              for (auto& [window, dialog] : m_dialogs) {
                if (dialog.id == id) {
                  dialog.answered = true;
                  closeWindow(*window);
                  return;
                }
              }
            },
    });
    m_systemPortals = std::make_unique<SystemPortals>(*bus(), SystemPortals::Handlers{
        .ask = [this](std::uint64_t id, const SystemPortals::Request& r) { openPortalSheet(id, r); },
        .closed =
            [this](std::uint64_t id) {
              for (auto& [window, dialog] : m_dialogs) {
                if (dialog.id == id && dialog.portal != nullptr) {
                  dialog.answered = true;
                  closeWindow(*window);
                  return;
                }
              }
            },
    });
    scheduleIdleQuit();
  }
}

void FilesApp::scheduleIdleQuit() {
  // D-Bus activation starts the picker again for the next request.
  m_idleQuit.start(std::chrono::minutes(2), [this] {
    if (windows().empty() && m_portal->idle() && m_chooserPortal->idle() && m_systemPortals->idle() && m_pendingTags.empty()) {
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
  // Save opens as the compact sheet (mockup §6); its chevron grows the window to the full browser.
  const bool save = request.kind == Kind::Save;
  AppWindow& window = addWindow({
      .appId = info().id,
      .title = title,
      .stateKey = save ? "picker-save" : "picker",
      .width = save ? kSaveWidth : FilesWindow::kPickerWidth,
      .height = save ? kSaveHeight : FilesWindow::kPickerHeight,
      .minWidth = save ? kSaveWidth : 480,
      .minHeight = save ? kSaveHeight : 320,
      .navigation = true,
      .sidebarWidth = 200.0F,
      .parentWindow = request.parentWindow,
  });
  std::error_code ec;
  const fs::path folder = !request.currentFolder.empty() && fs::is_directory(request.currentFolder, ec)
      ? request.currentFolder
      : fs::path(g_get_home_dir());
  if (save) {
    window.setSidebarVisible(false);
  }
  auto& files = m_windows[&window] = std::make_unique<FilesWindow>(*this, window, folder);
  files->setPicker(id, request);
  if (!window.open()) {
    pickerAnswered(id, {.response = 2, .uris = {}, .filter = {}});
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openChooser(std::uint64_t id, const AppChooserPortal::ChooseRequest& request) {
  AppWindow& window = addWindow({.appId = info().id, .title = "Open With", .stateKey = "open-with", .width = kChooserWidth,
                                 .height = kChooserHeight, .minWidth = kChooserWidth, .minHeight = kDialogMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  dialog.chooser = std::make_unique<AppChooserSheet>(
      request,
      [this, &window](std::uint32_t response, const std::string& choice) {
        if (const auto it = m_dialogs.find(&window); it != m_dialogs.end() && !it->second.answered) {
          it->second.answered = true;
          m_chooserPortal->answerChoose(it->second.id, response, choice);
          closeWindow(window);
        }
      },
      [&window] { window.requestRebuild(); });
  window.setContentBuilder([chooser = dialog.chooser.get(), &window](Renderer&, float, float) {
    return chooser->build(window.uiScale());
  });
  if (!window.open()) {
    m_chooserPortal->answerChoose(id, 2, {});
    m_dialogs.erase(&window);
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openPortalSheet(std::uint64_t id, const SystemPortals::Request& request) {
  const bool wide = request.kind == SystemPortals::Kind::Wallpaper;
  AppWindow& window = addWindow({.appId = info().id, .title = PortalSheet::titleFor(request), .stateKey = "portal",
                                 .width = wide ? kWallpaperWidth : kAccessWidth,
                                 .height = wide ? kWallpaperHeight : kPortalHeight,
                                 .minWidth = wide ? kWallpaperWidth : kAccessWidth, .minHeight = kDialogMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  dialog.portal = std::make_unique<PortalSheet>(request, [this, &window](std::uint32_t response, const std::string& name) {
    if (const auto it = m_dialogs.find(&window); it != m_dialogs.end() && !it->second.answered) {
      it->second.answered = true;
      m_systemPortals->answer(it->second.id, response, name);
      closeWindow(window);
    }
  });
  window.setContentBuilder([portal = dialog.portal.get(), &window](Renderer&, float, float) {
    return portal->build(window.uiScale());
  });
  if (!window.open()) {
    m_systemPortals->answer(id, 2);
    m_dialogs.erase(&window);
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openAccess(std::uint64_t id, const AppChooserPortal::AccessRequest& request) {
  AppWindow& window = addWindow({.appId = info().id, .title = request.title, .stateKey = "access", .width = kAccessWidth,
                                 .height = kAccessHeight, .minWidth = kAccessWidth, .minHeight = kDialogMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  dialog.access = std::make_unique<AccessSheet>(
      request, [this, &window](std::uint32_t response, const std::vector<AppChooserPortal::AccessChoice>& choices) {
        if (const auto it = m_dialogs.find(&window); it != m_dialogs.end() && !it->second.answered) {
          it->second.answered = true;
          m_chooserPortal->answerAccess(it->second.id, response, choices);
          closeWindow(window);
        }
      });
  window.setContentBuilder([access = dialog.access.get(), &window](Renderer&, float, float) {
    return access->build(window.uiScale());
  });
  if (!window.open()) {
    m_chooserPortal->answerAccess(id, 2, {});
    m_dialogs.erase(&window);
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

namespace {
  struct FileStamp {
    bool exists = false;
    std::uint64_t inode = 0;
    std::int64_t mtimeNs = 0;
  };
  FileStamp stampOf(const fs::path& file) {
    struct stat st {};
    if (::stat(file.c_str(), &st) != 0) {
      return {};
    }
    return {.exists = true, .inode = static_cast<std::uint64_t>(st.st_ino),
            .mtimeNs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
  }
} // namespace

void FilesApp::tagWhenWritten(const fs::path& file, std::vector<std::string> tags) {
  const FileStamp before = stampOf(file);
  m_pendingTags.push_back({.file = file, .tags = std::move(tags), .deadline = std::chrono::steady_clock::now() + kTagWaitTime,
                           .existed = before.exists, .inode = before.inode, .mtimeNs = before.mtimeNs});
  if (m_tagPoll.active()) {
    return;
  }
  m_tagPoll.startRepeating(std::chrono::milliseconds(500), [this] {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_pendingTags, [&](const PendingTags& p) {
      const FileStamp stamp = stampOf(p.file);
      const bool written = stamp.exists && (!p.existed || stamp.inode != p.inode || stamp.mtimeNs != p.mtimeNs);
      if (written && m_fileOps != nullptr) {
        m_fileOps->setTags({p.file}, p.tags, "add");
        return true;
      }
      return now > p.deadline;
    });
    if (m_pendingTags.empty()) {
      m_tagPoll.stop();
    }
  });
}

void FilesApp::refreshPlaces() {
  for (auto& [window, files] : m_windows) {
    files->refreshPlaces();
  }
}

void FilesApp::unmounted(const std::filesystem::path& mount, const std::string& error) {
  if (!error.empty()) {
    if (FilesWindow* active = activeFilesWindow()) {
      active->unmounted(mount, error);
    }
    return;
  }
  for (auto& [window, files] : m_windows) {
    files->unmounted(mount, error);
  }
}

AppWindow& FilesApp::createWindow() {
  AppWindow& window = addWindow({
      .appId = info().id,
      .title = info().name,
      .navigation = true,
      .sidebarWidth = 240.0F,
      .moreMenu = true,
  });
  fs::path location = m_nextLocation.empty() ? fs::path(g_get_home_dir()) : m_nextLocation;
  m_nextLocation.clear();
  m_windows[&window] = std::make_unique<FilesWindow>(*this, window, std::move(location));
  return window;
}

void FilesApp::windowClosed(AppWindow& window) {
  if (const auto it = m_dialogs.find(&window); it != m_dialogs.end()) {
    // Closed from the frame: the request ends as cancelled.
    if (!it->second.answered && it->second.portal != nullptr) {
      m_systemPortals->answer(it->second.id, 1);
    } else if (!it->second.answered && m_chooserPortal != nullptr) {
      if (it->second.chooser != nullptr) {
        m_chooserPortal->answerChoose(it->second.id, 1, {});
      } else {
        m_chooserPortal->answerAccess(it->second.id, 1, {});
      }
    }
    m_dialogs.erase(it);
    return;
  }
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
  } else if (name.starts_with("chooser.") || name.starts_with("access.") || name.starts_with("portal.")) {
    dialogAction(name, parameter);
    return;
  } else {
    DeskApp::action(name, parameter, activationToken);
    return;
  }
  if (AppWindow* w = activeWindow()) {
    w->activate(activationToken);
  }
}

void FilesApp::dialogAction(const std::string& name, const std::vector<std::string>& parameter) {
  // The active dialog, else the newest: a headless test has no keyboard focus.
  Dialog* dialog = nullptr;
  if (const auto it = m_dialogs.find(activeWindow()); it != m_dialogs.end()) {
    dialog = &it->second;
  } else if (!m_dialogs.empty()) {
    dialog = &std::ranges::max_element(m_dialogs, {}, [](const auto& d) { return d.second.id; })->second;
  }
  FilesWindow* files = activeFilesWindow();
  AppChooserSheet* sheet = files != nullptr ? files->openWithSheet() : nullptr;
  if (dialog == nullptr && sheet == nullptr) {
    return;
  }
  if (AppChooserSheet* chooser = dialog != nullptr ? dialog->chooser.get() : sheet) {
    if (name == "chooser.select" && !parameter.empty()) {
      chooser->select(parameter.front());
    } else if (name == "chooser.accept") {
      chooser->accept();
    } else if (name == "chooser.cancel") {
      chooser->cancel();
    }
  } else if (PortalSheet* portal = dialog != nullptr ? dialog->portal.get() : nullptr) {
    if (name == "portal.accept") {
      portal->accept();
    } else if (name == "portal.cancel") {
      portal->cancel();
    } else if (name == "portal.name" && !parameter.empty()) {
      portal->setName(parameter.front());
    }
  } else if (AccessSheet* access = dialog != nullptr ? dialog->access.get() : nullptr) {
    if (name == "access.grant" || name == "access.deny") {
      access->answer(name == "access.grant");
    }
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
      {"file.open-with", "Open With…", "File", std::nullopt, [](const FilesWindow& w) { return w.canOpenWith(); },
       [](FilesWindow& w) { w.openWith(); }},
      {"file.star", "Add to Starred", "File", chord(XKB_KEY_d, KeyMod::Ctrl),
       [](const FilesWindow& w) { return !w.isPicker() && !w.inTrash() && w.hasSelection(); },
       [](FilesWindow& w) { w.toggleStar(); }},
      {"edit.copy-location", "Copy Location", "Edit", chord(XKB_KEY_c, KeyMod::Ctrl | KeyMod::Alt),
       [](const FilesWindow& w) { return !w.isPicker(); }, [](FilesWindow& w) { w.copyLocation(); }},
      {"file.open-terminal", "Open Terminal Here", "File", chord(XKB_KEY_F4, KeyMod::Shift),
       [](const FilesWindow& w) { return !w.isPicker() && !w.inTrash(); }, [](FilesWindow& w) { w.openTerminalHere(); }},
      {"file.restore", "Put Back", "File", std::nullopt,
       [](const FilesWindow& w) { return w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.restoreSelection(); }},
      {"file.new-folder", "New Folder", "File", chord(XKB_KEY_N, KeyMod::Ctrl | KeyMod::Shift),
       [](const FilesWindow& w) { return !w.inTrash(); }, [](FilesWindow& w) { w.newFolder(); }},
      {"edit.find", "Find", "Edit", chord(XKB_KEY_f, KeyMod::Ctrl), [](const FilesWindow&) { return true; },
       [](FilesWindow& w) { w.focusSearch(); }},
      {"go.connect", "Connect to Server…", "Go", chord(XKB_KEY_k, KeyMod::Ctrl), [](const FilesWindow&) { return true; },
       [](FilesWindow& w) { w.openConnect(""); }},
      {"go.path", "Go to Folder…", "Go", chord(XKB_KEY_l, KeyMod::Ctrl), always, [](FilesWindow& w) { w.openPathField(""); }},
      {"file.info", "Get Info", "File", chord(XKB_KEY_i, KeyMod::Ctrl), [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); },
       [](FilesWindow& w) { w.getInfo(); }},
      {"trash.empty", "Empty Trash…", "File", chord(XKB_KEY_Delete, KeyMod::Ctrl | KeyMod::Shift),
       [](const FilesWindow& w) { return w.trashHasItems(); }, [](FilesWindow& w) { w.askEmptyTrash(); }},
      {"file.compress", "Compress…", "File", std::nullopt, [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); },
       [](FilesWindow& w) { w.compressSelection(); }},
      {"file.extract-here", "Extract Here", "File", std::nullopt, [](const FilesWindow& w) { return w.canExtract(); },
       [](FilesWindow& w) { w.extractSelection(true); }},
      {"file.extract-to", "Extract To…", "File", std::nullopt, [](const FilesWindow& w) { return w.canExtract(); },
       [](FilesWindow& w) { w.extractSelection(false); }},
      {"file.rename", "Rename", "File", chord(XKB_KEY_F2, 0),
       [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.renameSelection(); }},
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
      {"view.details", "Show Details", "View", chord(XKB_KEY_p, KeyMod::Alt), [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.toggleDetails(); }},
      // Ctrl+T and Ctrl+(Shift+)Tab live in FileBrowser::onKey; these entries put the tabs in the More menu.
      {"file.new-tab", "New Tab", "File", std::nullopt, [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.newTab(); }},
      {"view.next-tab", "Show Next Tab", "View", std::nullopt, [](const FilesWindow& w) { return w.tabCount() > 1; },
       [](FilesWindow& w) { w.stepTab(1); }},
      {"view.previous-tab", "Show Previous Tab", "View", std::nullopt,
       [](const FilesWindow& w) { return w.tabCount() > 1; }, [](FilesWindow& w) { w.stepTab(-1); }},
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
        .enabled = [this, enabled = spec.enabled, chooser = allowedInPicker(spec.id),
                    edit = std::string_view(spec.group) == "Edit"] {
          const FilesWindow* w = activeFilesWindow();
          // The inline rename field keeps Ctrl+A/C/X/V/Z for its text.
          return w != nullptr && (chooser || !w->isPicker()) && !(edit && w->renaming()) && enabled(*w);
        },
        .run = [this, run = spec.run] {
          if (FilesWindow* w = activeFilesWindow()) {
            run(*w);
          }
        },
    });
  }  // Ctrl+W closes the current tab first, the window only with one tab left (Finder, Dolphin).
  commands().add({
      .id = "window.close",
      .label = "Close Tab",
      .group = "Window",
      .shortcut = KeyChord{.sym = XKB_KEY_w, .modifiers = KeyMod::Ctrl},
      .run = [this] {
        FilesWindow* w = activeFilesWindow();
        if (w != nullptr && w->closeTab()) {
          return;
        }
        if (AppWindow* window = activeWindow()) {
          closeWindow(*window);
        }
      },
  });

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

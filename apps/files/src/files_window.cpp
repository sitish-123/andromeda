#include "files_window.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "ui/files/file_browser.h"
#include "files_app.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <utility>
#include <format>
#include <gio/gio.h>
#include <glib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  bool isDir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
  }
} // namespace

namespace {
  constexpr Logger kLog("files");
  constexpr auto kToastTime = std::chrono::seconds(5);
  constexpr float kToastRow = 32.0F;

  std::string uriFor(const fs::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = uri != nullptr ? uri : "";
    g_free(uri);
    return out;
  }

  std::string noun(std::size_t count) { return count == 1 ? "1 item" : std::format("{} items", count); }
} // namespace

FilesWindow::FilesWindow(FilesApp& app, AppWindow& window, fs::path location) : m_app(app), m_window(window) {
  FileBrowser::Host host{
      .invalidate = [this] { m_window.invalidate(); },
      .rebuild = [this] { m_window.requestRebuild(); },
      .openFile =
          [this](const fs::path& path) {
            if (m_picker.has_value()) {
              pickerAccept();
              return;
            }
            GError* error = nullptr;
            if (!g_app_info_launch_default_for_uri(uriFor(path).c_str(), nullptr, &error)) {
              showToast(std::format("Can’t open “{}”", path.filename().string()), false);
              kLog.warn("open {}: {}", path.string(), error != nullptr ? error->message : "?");
              g_clear_error(&error);
            }
          },
      .changed = [this] { onBrowserChanged(); },
      .modifiers = [this] { return m_app.loop().wayland().keyboardModifiers(); },
      .menu = {},
  };
  m_browser = std::make_unique<FileBrowser>(m_app.watcher(), m_window.animations(), std::move(host),
                                            FileBrowser::Options{.scale = m_window.uiScale()});
  m_window.setSidebarBuilder([this](Renderer&, float width, float) {
    m_browser->setScale(m_window.uiScale());
    return m_browser->buildSidebar(width);
  });
  m_window.setHeaderAccessoryBuilder([this] { return m_browser->buildHeaderAccessory(); });
  m_window.setContentBuilder([this](Renderer&, float, float) { return buildContent(); });
  m_window.setKeyHandler([this](const KeyboardEvent& event) { return m_browser->onKey(event); });
  if (!m_picker.has_value()) {
    loadTrash();
  }
  m_browser->jump(location);
}

std::unique_ptr<Node> FilesWindow::buildContent() {
  const float s = m_window.uiScale();
  m_browser->setScale(s);
  auto content = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  if (m_conflictJob.has_value()) {
    if (const FileOpsClient::Job* job = m_app.fileOps() != nullptr ? m_app.fileOps()->job(*m_conflictJob) : nullptr;
        job != nullptr && job->conflict.has_value()) {
      const auto& [source, target] = *job->conflict;
      auto sheet = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * s, .padding = Style::spaceMd * s,
                               .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Style::scaledRadiusMd(s)});
      sheet->addChild(ui::label({
          .text = std::format("“{}” already exists in “{}”.", target.filename().string(),
                              target.parent_path().filename().string()),
          .fontSize = Style::fontSizeBody * s,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label1),
      }));
      auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s});
      buttons->addChild(ui::button({
          .text = std::string("Apply to All"),
          .selected = m_applyToAll,
          .variant = m_applyToAll ? ButtonVariant::TabActive : ButtonVariant::Ghost,
          .onClick = [this] { toggleApplyToAll(); },
      }));
      buttons->addChild(ui::node({.flexGrow = 1.0F}));
      buttons->addChild(ui::button({.text = std::string("Skip"), .onClick = [this] { resolveConflict(FileOpsClient::Resolution::Skip); }}));
      buttons->addChild(ui::button({.text = std::string("Keep Both"), .onClick = [this] { resolveConflict(FileOpsClient::Resolution::KeepBoth); }}));
      buttons->addChild(ui::button({.text = std::string("Replace"), .variant = ButtonVariant::Primary,
                                    .onClick = [this] { resolveConflict(FileOpsClient::Resolution::Replace); }}));
      sheet->addChild(std::move(buttons));
      auto pad = ui::column({.align = FlexAlign::Stretch, .padding = Style::spaceSm * s});
      pad->addChild(std::move(sheet));
      content->addChild(std::move(pad));
    }
  }
  content->addChild(m_browser->buildContent());
  if (m_picker.has_value()) {
    content->addChild(ui::separator({}));
    content->addChild(buildPickerFooter(s));
  } else if (!m_toast.empty()) {
    auto toast = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                          .height = kToastRow * s});
    toast->addChild(ui::label({
        .text = m_toast,
        .fontSize = Style::fontSizeCaption * s,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .flexGrow = 1.0F,
    }));
    if (m_toastUndo) {
      toast->addChild(ui::button({.text = std::string("Undo"), .variant = ButtonVariant::Ghost, .onClick = [this] { undo(); }}));
    }
    content->addChild(ui::separator({}));
    content->addChild(std::move(toast));
  }
  return content;
}

FilesWindow::~FilesWindow() {
  // No window is left to answer these: a conflict would hang the job in the daemon.
  if (FileOpsClient* ops = m_app.fileOps()) {
    for (const std::string& job : m_jobs) {
      ops->cancel(job);
    }
  }
}

std::set<std::string> FilesWindow::takeJobs() {
  m_conflictJob.reset();
  return std::exchange(m_jobs, {});
}

void FilesWindow::adoptJobs(const std::set<std::string>& jobs) {
  FileOpsClient* ops = m_app.fileOps();
  for (const std::string& path : jobs) {
    m_jobs.insert(path);
    if (const FileOpsClient::Job* job = ops != nullptr ? ops->job(path) : nullptr) {
      onJobChanged(*job);
    }
  }
}

float FilesWindow::scale() const noexcept { return m_window.uiScale(); }

bool FilesWindow::canBack() const noexcept { return m_browser->canBack(); }
bool FilesWindow::canForward() const noexcept { return m_browser->canForward(); }
bool FilesWindow::inTrash() const noexcept { return m_browser->listingId() == "trash"; }
const fs::path& FilesWindow::location() const noexcept { return m_browser->location(); }
bool FilesWindow::hasSelection() const noexcept { return m_browser->selectionCount() > 0; }

void FilesWindow::onBrowserChanged() {
  m_window.setTitle(m_browser->title());
  m_window.refreshNavigation();
}

void FilesWindow::loadTrash() {
  m_trash.clear();
  FileBrowser::Listing listing{.id = "trash", .title = "Trash", .rows = {}};
  for (const fs::path& dir : trashDirs()) {
    for (TrashItem& item : listTrash(dir)) {
      FileInfo info;
      info.name = (item.trashDir / "files" / item.name).string();
      info.displayName = item.originalPath.filename().string();
      gchar* key = g_utf8_collate_key_for_filename(info.displayName.c_str(), -1);
      info.collateKey = key != nullptr ? key : "";
      g_free(key);
      info.isDir = item.isDir;
      info.kind = item.isDir ? FileKind::Directory : FileKind::File;
      info.size = item.size;
      info.contentType = item.isDir ? "inode/directory" : "application/octet-stream";
      listing.rows.push_back(std::make_shared<const FileInfo>(std::move(info)));
      m_trash.push_back(std::move(item));
    }
  }
  const std::size_t count = m_trash.size();
  m_browser->setExtraPlaces({{.label = "Trash", .glyph = count > 0 ? "trash-x" : "trash", .path = {},
                              .caption = count > 0 ? std::to_string(count) : std::string(),
                              .onClick = [this] { goTrash(); }, .id = "trash"}});
  if (inTrash()) {
    m_browser->showListing(std::move(listing));
  } else {
    m_trashListing = std::move(listing);
  }
}

void FilesWindow::rebuild() { m_window.requestRebuild(); }

void FilesWindow::go(const fs::path& location) { m_browser->go(location); }

void FilesWindow::goTrash() {
  loadTrash();
  if (!inTrash()) {
    m_browser->showListing(std::move(m_trashListing));
  }
}

void FilesWindow::back() { m_browser->back(); }
void FilesWindow::forward() { m_browser->forward(); }
void FilesWindow::up() { m_browser->up(); }
void FilesWindow::selectAll() { m_browser->selectAll(); }
void FilesWindow::selectNames(const std::vector<std::string>& names) { m_browser->selectNames(names); }
void FilesWindow::toggleHidden() { m_browser->setShowHidden(!m_browser->showHidden()); }

void FilesWindow::reload() {
  if (inTrash()) {
    loadTrash();
  } else {
    m_browser->reload();
  }
}

std::vector<fs::path> FilesWindow::selectedPaths() const { return m_browser->selectedPaths(); }

void FilesWindow::openSelection() {
  if (inTrash()) {
    return;
  }
  const auto paths = selectedPaths();
  if (paths.size() == 1 && isDir(paths.front())) {
    go(paths.front());
    return;
  }
  if (m_picker.has_value()) {
    pickerAccept();
    return;
  }
  for (const fs::path& path : paths) {
    GError* error = nullptr;
    if (!g_app_info_launch_default_for_uri(uriFor(path).c_str(), nullptr, &error)) {
      showToast(std::format("Can’t open “{}”", path.filename().string()), false);
      kLog.warn("open {}: {}", path.string(), error != nullptr ? error->message : "?");
      g_clear_error(&error);
    }
  }
}

void FilesWindow::copySelection(bool cut) {
  if (inTrash() || !hasSelection()) {
    return;
  }
  m_app.clipboard() = {.paths = selectedPaths(), .cut = cut};
  showToast(std::format("{} {}", cut ? "Cut" : "Copied", noun(m_app.clipboard().paths.size())), false);
}

void FilesWindow::startJob(const char* verb, std::size_t count, std::function<void(FileOpsClient&, FileOpsClient::Started)> start) {
  FileOpsClient* ops = m_app.fileOps();
  if (ops == nullptr) {
    showToast("File operations are not available", false);
    return;
  }
  showToast(std::format("{} {}…", verb, noun(count)), false);
  start(*ops, [this, alive = std::weak_ptr<bool>(m_alive), verb = std::string(verb)](const std::string& job,
                                                                                       const std::string& error) {
    if (alive.expired()) {
      return;
    }
    if (job.empty()) {
      showToast(std::format("{} failed: {}", verb, error), false);
      return;
    }
    m_jobs.insert(job);
  });
}

void FilesWindow::paste() {
  auto clip = m_app.clipboard();
  if (inTrash() || clip.paths.empty()) {
    return;
  }
  if (clip.cut) {
    m_app.clipboard() = {};
    startJob("Moving", clip.paths.size(), [&](FileOpsClient& ops, auto done) { ops.move(clip.paths, location(), done); });
  } else {
    startJob("Copying", clip.paths.size(), [&](FileOpsClient& ops, auto done) { ops.copy(clip.paths, location(), done); });
  }
}

void FilesWindow::trashSelection() {
  const auto paths = selectedPaths();
  if (inTrash() || paths.empty()) {
    return;
  }
  startJob("Moving to Trash", paths.size(), [&](FileOpsClient& ops, auto done) { ops.trash(paths, done); });
}

void FilesWindow::restoreSelection() {
  if (!inTrash() || !hasSelection()) {
    return;
  }
  std::set<std::string> selected;
  for (const auto& row : m_browser->selectedRows()) {
    selected.insert(row->name);
  }
  std::vector<std::pair<fs::path, std::string>> items;
  for (const TrashItem& item : m_trash) {
    if (selected.contains((item.trashDir / "files" / item.name).string())) {
      items.emplace_back(item.trashDir, item.name);
    }
  }
  startJob("Restoring", items.size(), [&](FileOpsClient& ops, auto done) { ops.restore(items, done); });
}

void FilesWindow::undo() {
  FileOpsClient* ops = m_app.fileOps();
  if (ops == nullptr || m_undo.empty()) {
    return;
  }
  const std::string job = m_undo.back();
  m_undo.pop_back();
  startJob("Undoing", 1, [&](FileOpsClient& c, auto done) { c.undo(job, done); });
}

void FilesWindow::resolveConflict(FileOpsClient::Resolution resolution) {
  if (!m_conflictJob.has_value() || m_app.fileOps() == nullptr) {
    return;
  }
  m_app.fileOps()->resolveConflict(*m_conflictJob, resolution, m_applyToAll);
  m_conflictJob.reset();
  m_applyToAll = false;
  rebuild();
}

void FilesWindow::onJobChanged(const FileOpsClient::Job& job) {
  if (!m_jobs.contains(job.path)) {
    // Another window's job can change the Trash, which has no watcher.
    const bool touchesTrash = job.kind == "trash" || job.kind == "restore" || job.kind == "undo" || job.kind == "delete";
    // The Trash listing and its sidebar count follow it.
    if (touchesTrash && job.finished() && m_trashReloaded.insert(job.path).second) {
      loadTrash();
    }
    return;
  }
  if (job.state == "conflict" && job.conflict.has_value()) {
    m_conflictJob = job.path;
    rebuild();
    return;
  }
  if (!job.finished()) {
    if (job.totalFiles > 1) {
      showToast(std::format("{} of {}…", job.doneFiles, job.totalFiles), false);
    }
    return;
  }
  m_jobs.erase(job.path);
  kLog.debug("job {} {}{}", job.kind, job.state, job.undoable ? " undoable" : "");
  if (m_conflictJob == job.path) {
    m_conflictJob.reset();
  }
  if (job.state == "done") {
    const bool undoable = job.undoable && job.kind != "undo";
    if (undoable) {
      m_undo.push_back(job.path);
    }
    const std::string what = job.kind == "trash" ? "Moved to Trash"
        : job.kind == "copy"                    ? "Copied"
        : job.kind == "move"                    ? "Moved"
        : job.kind == "restore"                 ? "Restored"
        : job.kind == "undo"                    ? "Undone"
                                                : "Done";
    showToast(std::format("{} {}", what, noun(job.doneFiles > 0 ? job.doneFiles : 1)), undoable);
  } else if (job.state == "failed") {
    showToast(job.error.empty() ? std::string("The operation failed") : job.error, false);
  }
  if (job.kind == "trash" || job.kind == "restore" || job.kind == "undo" || job.kind == "delete") {
    loadTrash();
  }
}

void FilesWindow::showToast(std::string text, bool undo) {
  m_toast = std::move(text);
  m_toastUndo = undo;
  m_toastTimer.start(kToastTime, [this] {
    m_toast.clear();
    m_toastUndo = false;
    rebuild();
  });
  rebuild();
}

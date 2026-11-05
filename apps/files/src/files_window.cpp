#include "files_window.h"

#include "app/app_window.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "file_row.h"
#include "files_app.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
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
  constexpr auto kTypeAheadReset = std::chrono::milliseconds(800);
  constexpr float kSidebarRow = 32.0F;
  constexpr float kPathBar = 28.0F;

  struct Place {
    const char* label;
    const char* glyph;
    fs::path path;
  };

  std::vector<Place> places() {
    std::vector<Place> out;
    const char* home = g_get_home_dir();
    out.push_back({"Home", "home", home});
    const std::pair<GUserDirectory, std::pair<const char*, const char*>> dirs[] = {
        {G_USER_DIRECTORY_DESKTOP, {"Desktop", "device-desktop"}},
        {G_USER_DIRECTORY_DOCUMENTS, {"Documents", "file-text"}},
        {G_USER_DIRECTORY_DOWNLOAD, {"Downloads", "download"}},
        {G_USER_DIRECTORY_PICTURES, {"Pictures", "photo"}},
        {G_USER_DIRECTORY_MUSIC, {"Music", "music"}},
        {G_USER_DIRECTORY_VIDEOS, {"Videos", "movie"}},
    };
    for (const auto& [dir, label] : dirs) {
      // An unset XDG user dir falls back to $HOME, which is already listed.
      if (const char* path = g_get_user_special_dir(dir); path != nullptr && fs::path(path) != fs::path(home)
          && isDir(path)) {
        out.push_back({label.first, label.second, path});
      }
    }
    return out;
  }

  std::string uriFor(const fs::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = uri != nullptr ? uri : "";
    g_free(uri);
    return out;
  }

  std::string noun(std::size_t count) { return count == 1 ? "1 item" : std::format("{} items", count); }
} // namespace

class FilesWindow::Adapter final : public VirtualListAdapter {
public:
  explicit Adapter(FilesWindow& owner) : m_owner(owner) {}

  void setRows(std::vector<std::shared_ptr<const FileInfo>> rows) { m_rows = std::move(rows); }
  [[nodiscard]] const std::vector<std::shared_ptr<const FileInfo>>& rows() const noexcept { return m_rows; }

  [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
  [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override {
    return std::hash<std::string>{}(m_rows[index]->name);
  }
  [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override {
    // A new FileInfo (an update) or a selection change rebinds the row.
    const auto info = reinterpret_cast<std::uintptr_t>(m_rows[index].get());
    return info * 2 + (m_owner.m_selected.contains(m_rows[index]->name) ? 1 : 0);
  }
  [[nodiscard]] float measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) override {
    return FileRow::height(m_owner.scale());
  }
  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<FileRow>(
        m_owner.scale(),
        FileRow::Callbacks{
            .select = [this](std::size_t index, std::uint32_t mods) { m_owner.select(index, mods); },
            .open =
                [this](std::size_t index) {
                  m_owner.select(index, 0);
                  m_owner.openSelection();
                },
            .menu = {},
        }
    );
  }
  void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override {
    static_cast<FileRow&>(item).bind(
        renderer, *m_rows[index], index, width, m_owner.m_selected.contains(m_rows[index]->name), hovered
    );
  }

private:
  FilesWindow& m_owner;
  std::vector<std::shared_ptr<const FileInfo>> m_rows;
};

FilesWindow::FilesWindow(FilesApp& app, AppWindow& window, fs::path location)
    : m_app(app), m_window(window), m_location(std::move(location)), m_adapter(std::make_unique<Adapter>(*this)) {
  m_window.setSidebarBuilder([this](Renderer&, float width, float) {
    const float s = m_window.uiScale();
    auto list = ui::column({.align = FlexAlign::Stretch, .gap = 2.0F * s, .padding = Style::spaceSm * s, .width = width});
    const auto row = [&](const std::string& label, const char* glyph, bool current, std::function<void()> onClick) {
      list->addChild(ui::button({
          .text = label,
          .glyph = std::string(glyph),
          .glyphSize = 16.0F * s,
          .selected = current,
          .contentAlign = ButtonContentAlign::Start,
          .variant = current ? ButtonVariant::TabActive : ButtonVariant::Ghost,
          .radius = Style::scaledRadiusSm(s),
          .height = kSidebarRow * s,
          .onClick = std::move(onClick),
      }));
    };
    for (const Place& place : places()) {
      row(place.label, place.glyph, !m_inTrash && place.path == m_location, [this, path = place.path] { go(path); });
    }
    if (!m_picker.has_value()) {
      list->addChild(ui::separator({}));
      row("Trash", "trash", m_inTrash, [this] { goTrash(); });
    }
    return list;
  });
  m_window.setContentBuilder([this](Renderer& /*renderer*/, float width, float height) {
    const float s = m_window.uiScale();
    auto content = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
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

    const auto rowsNow = rows();
    const FileModel::Status status = m_inTrash ? FileModel::Status::Ready : m_model->status();
    if (status != FileModel::Status::Ready || rowsNow.empty()) {
      const char* text = status == FileModel::Status::Loading ? ""
          : status == FileModel::Status::Missing             ? "This folder no longer exists"
          : status == FileModel::Status::Denied              ? "You don’t have permission to open this folder"
          : status == FileModel::Status::Error               ? "This folder can’t be read"
          : m_inTrash                                        ? "Trash is empty"
                                                             : "This folder is empty";
      auto empty = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                               .flexGrow = 1.0F});
      if (text[0] != '\0') {
        empty->addChild(ui::glyph({.glyph = std::string(m_inTrash ? "trash" : "folder"), .glyphSize = 48.0F * s,
                                   .color = colorSpecFromRole(ColorRole::Label3)}));
        empty->addChild(ui::label({.text = std::string(text), .fontSize = Tokens::typeTitle3.size * s,
                                   .color = colorSpecFromRole(ColorRole::Label1)}));
      }
      content->addChild(std::move(empty));
    } else {
      m_adapter->setRows(rowsNow);
      content->addChild(ui::virtualListView({
          .state = &m_scroll,
          .contentScale = s,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
      }));
    }

    if (m_picker.has_value()) {
      content->addChild(ui::separator({}));
      content->addChild(buildPickerFooter(s));
      return content;
    }
    auto footer = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                           .height = kPathBar * s});
    footer->addChild(ui::label({
        .text = m_toast.empty() ? (m_inTrash ? std::string("Trash") : m_location.string()) : m_toast,
        .fontSize = Style::fontSizeCaption * s,
        .color = colorSpecFromRole(m_toast.empty() ? ColorRole::Label2 : ColorRole::Label1),
        .maxLines = 1,
        .ellipsize = TextEllipsize::Start,
        .flexGrow = 1.0F,
    }));
    if (!m_toast.empty() && m_toastUndo) {
      footer->addChild(ui::button({.text = std::string("Undo"), .variant = ButtonVariant::Ghost, .onClick = [this] { undo(); }}));
    }
    content->addChild(ui::separator({}));
    content->addChild(std::move(footer));
    return content;
  });
  m_window.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
  load();
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

std::string FilesWindow::title() const {
  if (m_inTrash) {
    return "Trash";
  }
  if (m_location == fs::path(g_get_home_dir())) {
    return "Home";
  }
  return m_location.has_filename() ? m_location.filename().string() : m_location.string();
}

void FilesWindow::load() {
  m_selected.clear();
  m_anchor.reset();
  m_cursor.reset();
  m_pendingSelect.clear();
  m_scroll = {};
  if (m_inTrash) {
    m_model.reset();
    m_view.reset();
    m_trash.clear();
    m_trashRows.clear();
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
        info.contentType = std::format("From {}", item.originalPath.parent_path().string());
        m_trashRows.push_back(std::make_shared<const FileInfo>(std::move(info)));
        m_trash.push_back(std::move(item));
      }
    }
  } else {
    m_model = FileModel::open(m_location, m_app.watcher());
    m_view = std::make_unique<FileModelView>(m_model);
    m_view->setShowHidden(m_showHidden);
    applyPickerFilter();
    m_view->setChangedCallback([this] {
      // Drop selected names that are gone.
      std::erase_if(m_selected, [this](const std::string& name) { return m_model->find(name) == nullptr; });
      if (!m_pendingSelect.empty() && m_model->status() != FileModel::Status::Loading) {
        selectNames(std::exchange(m_pendingSelect, {}));
        return;
      }
      rebuild();
    });
  }
  m_window.setTitle(title());
  rebuild();
}

void FilesWindow::rebuild() { m_window.requestRebuild(); }

std::vector<std::shared_ptr<const FileInfo>> FilesWindow::rows() const {
  if (m_inTrash) {
    auto out = m_trashRows;
    std::ranges::sort(out, [](const auto& a, const auto& b) { return FileModelView::before(*a, *b, FileSort{}); });
    return out;
  }
  return m_view != nullptr ? m_view->rows() : std::vector<std::shared_ptr<const FileInfo>>{};
}

void FilesWindow::go(const fs::path& location) {
  if (!m_inTrash && location == m_location) {
    return;
  }
  m_back.emplace_back(m_location, m_inTrash);
  m_forward.clear();
  m_location = location;
  m_inTrash = false;
  load();
}

void FilesWindow::goTrash() {
  if (m_inTrash) {
    return;
  }
  m_back.emplace_back(m_location, m_inTrash);
  m_forward.clear();
  m_inTrash = true;
  load();
}

void FilesWindow::back() {
  if (m_back.empty()) {
    return;
  }
  m_forward.emplace_back(m_location, m_inTrash);
  std::tie(m_location, m_inTrash) = m_back.back();
  m_back.pop_back();
  load();
}

void FilesWindow::forward() {
  if (m_forward.empty()) {
    return;
  }
  m_back.emplace_back(m_location, m_inTrash);
  std::tie(m_location, m_inTrash) = m_forward.back();
  m_forward.pop_back();
  load();
}

void FilesWindow::up() {
  if (!m_inTrash && m_location.has_parent_path() && m_location != m_location.root_path()) {
    go(m_location.parent_path());
  }
}

void FilesWindow::select(std::size_t index, std::uint32_t modifiers) {
  const auto list = rows();
  if (index >= list.size()) {
    return;
  }
  const std::string& name = list[index]->name;
  const std::optional<std::size_t> anchor = m_anchor.has_value() ? indexOf(*m_anchor) : std::nullopt;
  if ((modifiers & KeyMod::Shift) != 0 && anchor.has_value()) {
    if ((modifiers & KeyMod::Ctrl) == 0) {
      m_selected.clear();
    }
    const auto [lo, hi] = std::minmax(*anchor, index);
    for (std::size_t i = lo; i <= hi && i < list.size(); ++i) {
      m_selected.insert(list[i]->name);
    }
  } else if ((modifiers & KeyMod::Ctrl) != 0) {
    if (!m_selected.erase(name)) {
      m_selected.insert(name);
    }
    m_anchor = name;
  } else {
    m_selected = {name};
    m_anchor = name;
  }
  m_cursor = name;
  if (m_picker.has_value() && m_picker->request.kind == PickerPortal::Request::Kind::Save && !list[index]->isDir) {
    m_picker->name = list[index]->displayName;
  }
  rebuild();
}

std::optional<std::size_t> FilesWindow::indexOf(const std::string& name) const {
  const auto list = rows();
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (list[i]->name == name) {
      return i;
    }
  }
  return std::nullopt;
}

void FilesWindow::selectAll() {
  for (const auto& row : rows()) {
    m_selected.insert(row->name);
  }
  rebuild();
}

void FilesWindow::selectNames(const std::vector<std::string>& names) {
  if (!m_inTrash && m_model->status() == FileModel::Status::Loading) {
    m_pendingSelect = names;
    return;
  }
  m_selected.clear();
  for (const auto& row : rows()) {
    // Trash rows match by their original name too.
    if (std::ranges::contains(names, row->name) || (m_inTrash && std::ranges::contains(names, row->displayName))) {
      m_selected.insert(row->name);
      m_anchor = m_cursor = row->name;
    }
  }
  kLog.debug("selected {} of {} in {}", m_selected.size(), names.size(), m_inTrash ? "trash" : m_location.string());
  rebuild();
}

void FilesWindow::toggleHidden() {
  m_showHidden = !m_showHidden;
  if (m_view != nullptr) {
    m_view->setShowHidden(m_showHidden);
  }
}

void FilesWindow::reload() {
  if (m_model != nullptr) {
    m_model->reload();
  } else {
    load();
  }
}

std::vector<fs::path> FilesWindow::selectedPaths() const {
  std::vector<fs::path> out;
  for (const auto& row : rows()) {
    if (m_selected.contains(row->name)) {
      out.push_back(m_location / row->name);
    }
  }
  return out;
}

void FilesWindow::openSelection() {
  if (m_inTrash) {
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
  if (m_inTrash || m_selected.empty()) {
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
  if (m_inTrash || clip.paths.empty()) {
    return;
  }
  if (clip.cut) {
    m_app.clipboard() = {};
    startJob("Moving", clip.paths.size(), [&](FileOpsClient& ops, auto done) { ops.move(clip.paths, m_location, done); });
  } else {
    startJob("Copying", clip.paths.size(), [&](FileOpsClient& ops, auto done) { ops.copy(clip.paths, m_location, done); });
  }
}

void FilesWindow::trashSelection() {
  const auto paths = selectedPaths();
  if (m_inTrash || paths.empty()) {
    return;
  }
  startJob("Moving to Trash", paths.size(), [&](FileOpsClient& ops, auto done) { ops.trash(paths, done); });
}

void FilesWindow::restoreSelection() {
  if (!m_inTrash || m_selected.empty()) {
    return;
  }
  std::vector<std::pair<fs::path, std::string>> items;
  for (const TrashItem& item : m_trash) {
    if (m_selected.contains((item.trashDir / "files" / item.name).string())) {
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
    if (m_inTrash && touchesTrash && job.finished() && m_trashReloaded.insert(job.path).second) {
      load();
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
  if (m_inTrash) {
    load();
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

bool FilesWindow::onKey(const KeyboardEvent& event) {
  if (!event.pressed) {
    return false;
  }
  const auto list = rows();
  const std::optional<std::size_t> cursor = m_cursor.has_value() ? indexOf(*m_cursor) : std::nullopt;
  const std::size_t current = cursor.value_or(0);
  if ((event.sym == XKB_KEY_Down || event.sym == XKB_KEY_Up) && !list.empty()) {
    const std::size_t next = event.sym == XKB_KEY_Down ? std::min(current + (cursor ? 1 : 0), list.size() - 1)
                                                        : (current == 0 ? 0 : current - 1);
    select(next, event.modifiers & KeyMod::Shift);
    return true;
  }
  if (event.utf32 >= 0x20 && event.utf32 != 0x7F && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt)) == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_typeAheadAt > kTypeAheadReset) {
      m_typeAhead.clear();
    }
    m_typeAheadAt = now;
    gchar buf[8] = {};
    m_typeAhead.append(buf, static_cast<std::size_t>(g_unichar_to_utf8(event.utf32, buf)));
    gchar* needle = g_utf8_casefold(m_typeAhead.c_str(), -1);
    for (std::size_t i = 0; i < list.size(); ++i) {
      gchar* name = g_utf8_casefold(list[i]->displayName.c_str(), -1);
      const bool hit = g_str_has_prefix(name, needle);
      g_free(name);
      if (hit) {
        select(i, 0);
        break;
      }
    }
    g_free(needle);
    return true;
  }
  return false;
}

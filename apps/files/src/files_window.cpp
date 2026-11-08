#include "files_window.h"
#include "server_url.h"
#include "app_chooser_sheet.h"
#include "conflict_sheet.h"
#include "connect_sheet.h"
#include "core/input/key_chord.h"
#include "details_pane.h"
#include "tags.h"
#include "terminal.h"
#include "network_page.h"

#include "core/files/volumes.h"
#include "render/core/color.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "app/foreign_export.h"
#include "dbus/session_bus.h"
#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/input/key_modifiers.h"
#include "ui/files/file_format.h"
#include "ui/files/type_ahead.h"
#include "ui/files/this_pc.h"
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
#include "wayland/drag_and_drop.h"
#include "wayland/drag_icon_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <format>
#include <sstream>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  constexpr std::size_t kTagEntryBase = 1000; // menu entry ids of the Tags submenu rows
  constexpr float kDetailsMinWidth = 560.0F; // FILES-UI-SPEC §10 Narrow: the pane closes below this
  constexpr std::size_t kRecentsMax = 200; // recently-used.xbel from other tools can hold thousands
  bool isDir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
  }
} // namespace

namespace {
  constexpr Logger kLog("files");
  constexpr auto kToastTime = std::chrono::seconds(5);

  std::string uriFor(const fs::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = uri != nullptr ? uri : "";
    g_free(uri);
    return out;
  }

  std::string noun(std::size_t count) { return count == 1 ? "1 item" : std::format("{} items", count); }

  std::vector<fs::path> parseUriList(const std::string& data) {
    std::vector<fs::path> out;
    std::size_t start = 0;
    while (start < data.size()) {
      std::size_t end = data.find('\n', start);
      if (end == std::string::npos) {
        end = data.size();
      }
      std::string line = data.substr(start, end - start);
      start = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) {
        line.pop_back();
      }
      if (line.empty() || line.front() == '#') {
        continue;
      }
      if (gchar* file = g_filename_from_uri(line.c_str(), nullptr, nullptr)) {
        out.emplace_back(file);
        g_free(file);
      }
    }
    return out;
  }

  bool sameDevice(const fs::path& a, const fs::path& b) {
    struct stat sa {};
    struct stat sb {};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
  }
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
            if (ArchiveListing::isArchive(path)) {
              openArchive(path); // archives open like folders (mockup section 8)
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
      .menu = [this](std::size_t index, float x, float y) { showItemMenu(index, x, y); },
      .startDrag =
          [this](const std::vector<fs::path>& paths) {
            DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop();
            if (dnd == nullptr || m_window.wlSurface() == nullptr) {
              return false;
            }
            std::string uris;
            std::string text;
            for (const fs::path& p : paths) {
              uris += uriFor(p) + "\r\n";
              text += (text.empty() ? "" : "\n") + p.string();
            }
            m_dragIcon.reset();
            m_dragImage = m_browser->buildDragImage();
            const auto size = static_cast<std::uint32_t>(std::ceil(m_browser->dragImageSize()));
            auto icon = std::make_unique<DragIconSurface>(m_app.loop().wayland(), size, size,
                                                          std::max(1, static_cast<int>(std::ceil(m_window.uiScale()))));
            icon->setRenderContext(&m_app.loop().renderContext());
            icon->setPrepareFrameCallback([this, raw = icon.get()](bool, bool) {
              m_app.loop().renderContext().makeCurrent(raw->renderTarget());
              if (m_dragImage != nullptr) {
                m_dragImage->layout(raw->renderTarget().renderer());
              }
            });
            icon->setSceneRoot(m_dragImage.get());
            if (icon->initialize()) {
              m_dragIcon = std::move(icon);
            }
            return dnd->startDrag(m_window.wlSurface(), m_app.loop().wayland().lastInputSerial(),
                                  {.mimeTypes = {"text/uri-list", "text/plain;charset=utf-8", "text/plain"},
                                   .data = [uris, text](const std::string& mime) { return mime == "text/uri-list" ? uris : text; },
                                   .actions = DragAndDrop::Copy | DragAndDrop::Move | DragAndDrop::Ask,
                                   .icon = m_dragIcon != nullptr ? m_dragIcon->wlSurface() : nullptr,
                                   .finished =
                                       [this, alive = std::weak_ptr<bool>(m_alive)](std::uint32_t, bool) {
                                         if (!alive.expired()) {
                                           m_browser->dragEnded();
                                           // The compositor may still draw the icon this frame: drop it after.
                                           DeferredCall::callLater([this, alive] {
                                             if (!alive.expired()) {
                                               m_dragIcon.reset();
                                               m_dragImage.reset();
                                             }
                                           });
                                         }
                                       }});
          },
      .focus = [this](InputArea* area) { m_window.input().setFocus(area); },
      .rename =
          [this](const fs::path& path, const std::string& name, std::function<void(const std::string&)> done) {
            FileOpsClient* ops = m_app.fileOps();
            if (ops == nullptr) {
              done("File operations are not available.");
              return;
            }
            ops->rename(path, name, [this, alive = std::weak_ptr<bool>(m_alive), done, path, name](const std::string& job,
                                                                                     const std::string& error) {
              if (alive.expired()) {
                return;
              }
              if (job.empty()) {
                done(error.empty() ? std::string("The name could not be changed.") : error);
                return;
              }
              m_jobs.insert(job);
              m_renameDone[job] = [this, done, from = path, to = path.parent_path() / name](const std::string& err) {
                if (err.empty()) {
                  m_app.starred().moved(from, to);
                }
                done(err);
              };
            });
          },
      .quickLook = [this](const fs::path& path) { quickLook(path, true); },
      // The Gallery is full-bleed (mockup §15): the sidebar folds away while it shows.
      .viewChanged = [this](FileBrowser::View view) { m_window.setSidebarVisible(view != FileBrowser::View::Gallery); },
      .headerButtons =
          [this](Flex& row, float s) {
            if (m_picker.has_value()) {
              return;
            }
            // Mockup .ib.fill: a 32 circle, fill_1 while the pane shows.
            const Button::ButtonStateColors off{.bg = clearColorSpec(), .border = clearColorSpec(),
                                                .label = colorSpecFromRole(ColorRole::Label1)};
            Button::ButtonStateColors on = off;
            on.bg = colorSpecFromRole(ColorRole::Fill1);
            Button::ButtonStateColors pressed = off;
            pressed.bg = colorSpecFromRole(ColorRole::Fill2);
            // Mockup order: split, then info.
            row.addChild(ui::button({
                .glyph = std::string("layout-columns"),
                .glyphSize = Tokens::sizeSymbol * s,
                .variant = ButtonVariant::Ghost,
                .customPalette = Button::ButtonPalette{.normal = m_panes[1] != nullptr ? on : off, .hover = on,
                                                       .pressed = pressed, .disabled = off},
                .tooltip = std::string("Split View (F3)"),
                .radius = Tokens::sizeControlM * s / 2.0F,
                .width = Tokens::sizeControlM * s,
                .height = Tokens::sizeControlM * s,
                .onClick = [this] { DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
                  if (!alive.expired()) {
                    toggleSplit();
                  }
                }); },
            }));
            const Button::ButtonStateColors& rest = m_detailsOpen ? on : off;
            row.addChild(ui::button({
                .glyph = std::string("info-circle"),
                .glyphSize = Tokens::sizeSymbol * s,
                .variant = ButtonVariant::Ghost,
                .customPalette = Button::ButtonPalette{.normal = rest, .hover = on, .pressed = pressed, .disabled = off},
                .tooltip = std::string("Details (Alt+P)"),
                .radius = Tokens::sizeControlM * s / 2.0F,
                .width = Tokens::sizeControlM * s,
                .height = Tokens::sizeControlM * s,
                .onClick = [this] { toggleDetails(); },
            }));
          },
  };
  m_host = std::move(host);
  makePane(0);
  m_browser = m_panes[0].get();
  m_window.setSidebarBuilder([this](Renderer&, float width, float) {
    m_browser->setScale(m_window.uiScale());
    return m_browser->buildSidebar(width);
  });
  m_window.setHeaderAccessoryBuilder([this] { return m_browser->buildHeaderAccessory(); });
  m_window.setContentBuilder([this](Renderer& renderer, float w, float h) { return buildContent(renderer, w, h); });
  m_window.setKeyHandler([this](const KeyboardEvent& event) {
    // "/" or "~" typed in the list starts a path (Finder, GTK); search and rename fields take the key before this.
    if (event.pressed && (event.utf32 == '/' || event.utf32 == '~') && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt)) == 0 &&
        !renaming()) {
      openPathField(event.utf32 == '/' ? std::string("/") : std::string("~/"));
      return true;
    }
    if (event.pressed && !event.repeat && event.sym == XKB_KEY_F3 && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Shift)) == 0) {
      toggleSplit();
      return true;
    }
    return m_browser->onKey(event);
  });
  m_window.setActiveCallback([this](bool active) { m_browser->setActive(active); });
  if (!m_picker.has_value()) {
    loadTrash();
  }
  m_browser->jump(location);
}

void FilesWindow::registerDropTarget() {
  wl_surface* surface = m_window.wlSurface();
  DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop();
  if (surface == nullptr || dnd == nullptr || surface == m_dropSurface) {
    return;
  }
  m_dropSurface = surface;
  dnd->setAcceptedTypes({"text/uri-list", "image/png", "text/plain;charset=utf-8", "text/plain"});
  dnd->setTarget(surface, {
      .enter = {},
      .motion =
          [this](float x, float y) -> std::uint32_t {
            if (m_picker.has_value()) {
              return DragAndDrop::None;
            }
            const auto target = dragOver(x, y);
            if (!target.has_value()) {
              return DragAndDrop::None;
            }
            // The drop reads its data after the release, when the keys may be up: keep what the cue showed.
            m_dragMods = m_app.loop().wayland().keyboardModifiers();
            switch (dropOp(*target, draggedPaths(), m_dragMods)) {
            case DropOp::Copy:
              return DragAndDrop::Copy;
            case DropOp::Link:
              return DragAndDrop::Ask;
            case DropOp::Move:
            case DropOp::Trash:
              return DragAndDrop::Move;
            }
            return DragAndDrop::None;
          },
      .leave =
          [this] {
            for (const auto& pane : m_panes) {
              if (pane != nullptr) {
                pane->dragLeave();
              }
            }
          },
      .drop =
          [this](float x, float y, std::uint32_t action, const std::string& mime, std::string data) {
            const auto target = dragOver(x, y);
            if (target.has_value() && mime == "text/uri-list") {
              drop(*target, data, action);
            } else if (target.has_value() && !target->trash) {
              dropData(target->folder, mime, data);
            }
            for (const auto& pane : m_panes) {
              if (pane != nullptr) {
                pane->dragEnded();
              }
            }
          },
  });
}

FilesWindow::DropOp FilesWindow::dropOp(const FileBrowser::DropTarget& target, const std::vector<fs::path>& sources,
                                        std::uint32_t modifiers) {
  if (target.trash) {
    return DropOp::Trash;
  }
  const std::uint32_t mods = modifiers & (KeyMod::Ctrl | KeyMod::Shift);
  if (mods == (KeyMod::Ctrl | KeyMod::Shift)) {
    return DropOp::Link;
  }
  if (mods == KeyMod::Ctrl) {
    return DropOp::Copy;
  }
  if (mods == KeyMod::Shift) {
    return DropOp::Move;
  }
  // Other apps' drags show their sources only on drop: copy is the safe guess until then.
  return !sources.empty() && sameDevice(sources.front(), target.folder) ? DropOp::Move : DropOp::Copy;
}

void FilesWindow::drop(const FileBrowser::DropTarget& target, const std::string& uriList, std::uint32_t action) {
  const std::vector<fs::path> sources = parseUriList(uriList);
  if (sources.empty()) {
    return;
  }
  // The negotiated action is what the cursor showed; recomputing with the now-known sources could turn Copy into Move.
  const DropOp op = target.trash                       ? DropOp::Trash
      : action == DragAndDrop::Move                     ? DropOp::Move
      : action == DragAndDrop::Copy                     ? DropOp::Copy
      : action == DragAndDrop::Ask                      ? DropOp::Link
                                                        : dropOp(target, sources, m_dragMods);
  kLog.debug("drop {} items op {} into {}", sources.size(), static_cast<int>(op),
             target.trash ? std::string("trash") : target.folder.string());
  switch (op) {
  case DropOp::Trash:
    startJob("Moving to Trash", sources.size(), [&](FileOpsClient& ops, auto done) { ops.trash(sources, done); });
    break;
  case DropOp::Move:
    startJob("Moving", sources.size(), [&](FileOpsClient& ops, auto done) { ops.move(sources, target.folder, done); });
    break;
  case DropOp::Copy:
    startJob("Copying", sources.size(), [&](FileOpsClient& ops, auto done) { ops.copy(sources, target.folder, done); });
    break;
  case DropOp::Link:
    startJob("Linking", sources.size(), [&](FileOpsClient& ops, auto done) { ops.link(sources, target.folder, done); });
    break;
  }
}

void FilesWindow::quickLook(const fs::path& path, bool toggle) {
  SessionBus* bus = m_app.bus();
  if (bus == nullptr) {
    return;
  }
  if (m_preview == nullptr) {
    try {
      m_preview = sdbus::createProxy(bus->connection(), sdbus::ServiceName{"org.gnome.NautilusPreviewer"},
                                     sdbus::ObjectPath{"/org/gnome/NautilusPreviewer"});
      // The preview's arrow keys walk our selection; we answer with the new item (FILES-UX 3.6).
      m_preview->uponSignal("SelectionEvent")
          .onInterface("org.gnome.NautilusPreviewer2")
          .call([this, alive = std::weak_ptr<bool>(m_alive)](std::int32_t direction) {
            if (alive.expired() || !m_previewOpen) {
              return;
            }
            m_browser->walk(direction < 0 ? -1 : 1);
            if (const auto next = m_browser->cursorPath()) {
              quickLook(*next, false);
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("quick look: {}", e.what());
      m_preview.reset();
      return;
    }
  }
  if (m_export == nullptr) {
    m_export = std::make_unique<desk::ForeignExport>(m_app.loop().wayland().display());
  }
  const std::string uri = uriFor(path);
  m_export->handle(m_window.wlSurface(), [this, uri, toggle, alive = std::weak_ptr<bool>(m_alive)](const std::string& handle) {
    if (alive.expired() || m_preview == nullptr) {
      return;
    }
    m_previewOpen = !toggle || !m_previewOpen;
    kLog.debug("quick look {} {}", m_previewOpen ? "show" : "close", uri);
    m_preview->callMethodAsync("ShowFile")
        .onInterface("org.gnome.NautilusPreviewer2")
        .withArguments(uri, handle, toggle)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error.has_value()) {
            kLog.warn("quick look: {}", error->getMessage());
          }
        });
  });
}

void FilesWindow::renameSelection() {
  const auto rows = m_browser->selectedRows();
  if (inTrash() || rows.empty()) {
    return;
  }
  if (rows.size() == 1) {
    m_browser->beginRename();
    return;
  }
  std::vector<BatchRenameItem> items;
  for (const auto& row : rows) {
    items.push_back({.name = row->displayName,
                     .modified = std::chrono::system_clock::time_point(
                         std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(row->mtimeNs))),
                     .taken = std::nullopt});
  }
  std::set<std::string> existing;
  for (const auto& row : m_browser->rows()) {
    existing.insert(row->displayName);
  }
  m_batchRename = std::make_unique<BatchRenameSheet>(
      location(), std::move(items), std::move(existing),
      [this](std::vector<std::pair<fs::path, std::string>> pairs) {
        const std::size_t count = pairs.size();
        startJob("Renaming", count, [&](FileOpsClient& ops, auto done) { ops.renameMany(pairs, done); });
      },
      [this] { m_window.dismissSheet(); },
      [this] { m_window.requestRebuild(); });
  kLog.debug("batch rename {} items", rows.size());
  m_window.presentSheet([this](float scale) { return m_batchRename->build(scale); },
                        [this] { m_window.dismissSheet(); });
}

void FilesWindow::dropData(const fs::path& folder, const std::string& mime, const std::string& data) {
  // Raw data from another app becomes a new file with a free name, then opens in rename (FILES-UX §2.3).
  const bool image = mime == "image/png";
  const std::string stem = image ? "Dropped Image" : "Dropped Text";
  const std::string ext = image ? ".png" : ".txt";
  std::string name = stem + ext;
  std::error_code ec;
  for (int n = 2; fs::exists(folder / name, ec); ++n) {
    name = std::format("{} {}{}", stem, n, ext);
  }
  FILE* out = std::fopen((folder / name).c_str(), "wbx");
  if (out == nullptr || std::fwrite(data.data(), 1, data.size(), out) != data.size()) {
    if (out != nullptr) {
      std::fclose(out);
    }
    showToast(std::format("Can’t save the dropped {}", image ? "image" : "text"), false);
    return;
  }
  std::fclose(out);
  kLog.debug("dropped data {}", (folder / name).string());
  if (folder == location()) {
    m_browser->renameWhenListed(name);
  }
}

void FilesWindow::newFolder() {
  if (m_browser->listingId() == "trash" || location().empty()) {
    return;
  }
  // The name is free now; the daemon picks another one only if something takes it first.
  std::string name = "New Folder";
  std::error_code ec;
  for (int n = 2; fs::exists(location() / name, ec); ++n) {
    name = std::format("New Folder {}", n);
  }
  m_browser->renameWhenListed(name);
  startJob("Creating", 1, [&](FileOpsClient& ops, auto done) { ops.newFolder(location(), name, done); });
}

std::unique_ptr<Node> FilesWindow::buildContent(Renderer& renderer, float width, float height) {
  m_pickerPrimary = nullptr; // the old footer goes with this rebuild
  registerDropTarget();
  const float s = m_window.uiScale();
  m_browser->setScale(s);
  if (m_picker.has_value() && m_picker->request.kind == PickerPortal::Request::Kind::Save && !m_picker->expanded) {
    return buildSaveCompact(s);
  }
  auto content = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  if (m_detailsOpen && !m_picker.has_value() && width >= kDetailsMinWidth * s) {
    if (m_details == nullptr) {
      m_details = std::make_unique<DetailsPane>(&m_app.thumbnails(), DetailsPane::Actions{
                                                                          .open = [this] { openSelection(); },
                                                                          .openWith = [this] { openWith(); },
                                                                      });
    }
    auto split = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    auto browser = buildPanes(s);
    browser->setFlexGrow(1.0F);
    split->addChild(std::move(browser));
    split->addChild(m_details->build(s));
    m_details->update(location(), selectedPaths());
    content->addChild(std::move(split));
  } else {
    content->addChild(buildPanes(s));
  }
  if (m_picker.has_value()) {
    content->addChild(ui::separator({}));
    content->addChild(buildPickerFooter(s));
  } else if (!m_toast.empty()) {
    // Mockup toast: a capsule floating 44 above the bottom edge, centred, menu material.
    const float h = Tokens::componentToastHeight * s;
    auto toast = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .fill = colorSpecFromRole(ColorRole::Popover),
                          .radius = h / 2.0F, .maxWidth = width - Tokens::spaceP32 * s, .height = h,
                          .participatesInLayout = false});
    toast->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP16 * s);
    toast->addChild(ui::label({
        .text = m_toast,
        .fontSize = Style::fontSizeBody * s,
        .fontWeight = FontWeight::Medium,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    }));
    if (m_toastUndo) {
      toast->addChild(ui::button({.text = std::string("Undo"), .variant = ButtonVariant::Default,
                                  .radius = Tokens::sizeControlS * s / 2.0F, .height = Tokens::sizeControlS * s,
                                  .onClick = [this] { undo(); }}));
    }
    toast->layout(renderer);
    toast->setPosition(std::round((width - toast->width()) / 2.0F), std::round(height - (Tokens::spaceP40 + Tokens::spaceP4) * s - h));
    toast->setZIndex(30);
    content->addChild(std::move(toast));
  }
  return content;
}

FilesWindow::~FilesWindow() {
  if (m_dropSurface != nullptr) {
    if (DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop()) {
      dnd->clearTarget(m_dropSurface);
    }
  }
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
  if (m_picker.has_value() && m_pickerPrimary != nullptr && m_picker->confirmReplace.empty()) {
    m_pickerPrimary->setText(pickerPrimaryLabel());
  }
  if (m_picker.has_value() && m_picker->request.directory && m_picker->request.title.empty()) {
    m_window.refreshNavigation(); // the "Choose a Folder for …" title stays
    return;
  }
  m_window.setTitle(m_browser->title());
  m_window.refreshNavigation();
  refreshSplitStatus();
  if (m_details != nullptr && m_detailsOpen) {
    m_details->update(location(), selectedPaths());
    m_window.invalidate();
  }
}

void FilesWindow::toggleDetails() {
  m_detailsOpen = !m_detailsOpen;
  kLog.debug("details pane {}", m_detailsOpen ? "on" : "off");
  m_window.requestRebuild();
}

void FilesWindow::makePane(std::size_t index) {
  FileBrowser::Host host = m_host;
  // A press in the other pane makes it active before the press acts (menu, drag, open use the active pane). The
  // pane finds its slot by address: closing the left pane moves the right one to slot 0.
  auto self = std::make_shared<FileBrowser*>(nullptr);
  host.pressed = [this, self] {
    if (m_panes[1] != nullptr && *self != m_browser) {
      setActivePane(*self == m_panes[0].get() ? 0 : 1);
    }
  };
  m_panes[index] = std::make_unique<FileBrowser>(m_app.watcher(), m_window.animations(), std::move(host),
                                                 FileBrowser::Options{.scale = m_window.uiScale()});
  *self = m_panes[index].get();
}

void FilesWindow::toggleSplit() {
  if (m_picker.has_value()) {
    return;
  }
  if (m_panes[1] == nullptr) {
    makePane(1);
    m_panes[1]->jump(m_browser->location());
    loadTrash(); // the sidebar places, for when the new pane becomes active
    kLog.debug("split view on");
    setActivePane(1);
    return;
  }
  // F3 again closes the inactive pane (spec §13): the active one stays, in slot 0.
  m_browser->releaseChrome();
  if (m_activePane == 1) {
    std::swap(m_panes[0], m_panes[1]);
  }
  // The scene holds the closed pane's nodes until the rebuild: it lives until the next close.
  m_retiredPane = std::move(m_panes[1]);
  m_activePane = 0;
  m_browser = m_panes[0].get();
  m_browser->setStatusSuffix({});
  m_browser->setCrumbAccent(false);
  kLog.debug("split view off");
  m_window.requestRebuild();
}

void FilesWindow::setActivePane(std::size_t index) {
  m_browser->releaseChrome();
  m_browser->setActive(false);
  m_activePane = index;
  m_browser = m_panes[index].get();
  m_browser->setActive(true);
  for (std::size_t i = 0; i < m_panes.size(); ++i) {
    if (m_panes[i] != nullptr) {
      m_panes[i]->setCrumbAccent(i == index);
    }
  }
  kLog.debug("split active pane {}", index == 0 ? "left" : "right");
  onBrowserChanged();
  m_window.requestRebuild(); // the sidebar, header accessory and path bar follow the active pane
}

void FilesWindow::refreshSplitStatus() {
  if (m_panes[1] == nullptr) {
    return;
  }
  const FileBrowser& other = *m_panes[m_activePane == 0 ? 1 : 0];
  std::string suffix = std::format("{} pane: {}", m_activePane == 0 ? "Right" : "Left", other.title());
  struct statvfs st {};
  if (!other.location().empty() && ::statvfs(other.location().c_str(), &st) == 0) {
    suffix += std::format(" · {} free", formatFileSize(static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize));
  }
  m_browser->setStatusSuffix(std::move(suffix));
  m_panes[m_activePane == 0 ? 1 : 0]->setStatusSuffix({});
}

std::unique_ptr<Node> FilesWindow::buildPanes(float scale) {
  if (m_panes[1] == nullptr) {
    return m_browser->buildContent();
  }
  // Spec §13: two panes, flex 1 each, a 0.5 px hairline between; only the active pane has the path bar.
  auto row = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  for (std::size_t i = 0; i < m_panes.size(); ++i) {
    if (i > 0) {
      row->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator), .width = 0.5F * scale}));
    }
    auto pane = m_panes[i]->buildContent(i == m_activePane, true);
    pane->setFlexGrow(1.0F);
    row->addChild(std::move(pane));
  }
  return row;
}

std::optional<FileBrowser::DropTarget> FilesWindow::dragOver(float x, float y) {
  // The inactive pane first: the active one owns the sidebar too, so it would match its rows.
  FileBrowser* other = m_panes[1] != nullptr ? m_panes[m_activePane == 0 ? 1 : 0].get() : nullptr;
  if (other != nullptr) {
    if (auto target = other->dragOver(x, y)) {
      m_browser->dragLeave();
      return target;
    }
    other->dragLeave();
  }
  return m_browser->dragOver(x, y);
}

const std::vector<fs::path>& FilesWindow::draggedPaths() const {
  for (const auto& pane : m_panes) {
    if (pane != nullptr && !pane->draggedPaths().empty()) {
      return pane->draggedPaths();
    }
  }
  return m_browser->draggedPaths();
}

void FilesWindow::loadTrash() {
  m_trash.clear();
  FileBrowser::Listing listing{.id = "trash", .title = "Trash", .rows = {}, .activatable = false};
  // The Trash bar (Finder): what the Trash holds and Empty…, which asks first.
  listing.banner = [this](float s) {
    auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                        .height = Tokens::sizeRowCompact * s + Tokens::spaceP8 * s});
    bar->addChild(ui::label({.text = m_trash.empty() ? std::string("The Trash is empty") : std::format("{} in the Trash", noun(m_trash.size())),
                             .fontSize = Style::fontSizeCaption * s, .color = colorSpecFromRole(ColorRole::Label2),
                             .maxLines = 1, .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
    bar->addChild(ui::button({.text = std::string("Empty…"), .enabled = !m_trash.empty(),
                              .variant = ButtonVariant::Default, .onClick = [this] { askEmptyTrash(); }}));
    return bar;
  };
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
  if (m_picker.has_value()) {
    return;
  }
  std::vector<FileBrowser::Place> places{{.label = "Recents", .glyph = "clock", .path = {}, .caption = {},
                                          .onClick = [this] { goRecents(); }, .id = "recents", .art = "recents",
                                          .group = FileBrowser::Place::Group::Favourites},
                                         {.label = "Starred", .glyph = "star", .path = {}, .caption = {},
                                          .onClick = [this] { goStarred(); }, .id = "starred", .art = "starred",
                                          .group = FileBrowser::Place::Group::Favourites},
                                         {.label = "This PC", .glyph = "device-desktop", .path = {}, .caption = {},
                                          .onClick = [this] { goThisPc(); }, .id = "this-pc", .art = "computer"}};
  // Sidebar Locations (mockup §1): the root drive's usage on This PC, then each removable drive with eject.
  for (const Volume& v : listVolumes()) {
    const float used = v.totalBytes > 0
                           ? static_cast<float>(v.totalBytes - v.freeBytes) / static_cast<float>(v.totalBytes)
                           : 0.0F;
    if (v.mountPoint == "/") {
      places[2].usage = used;
    } else if (v.removable) {
      places.push_back({.label = v.name, .glyph = "usb", .path = v.mountPoint, .art = "usb", .eject = true,
                        .onEject = [this, mount = v.mountPoint] { eject(mount); }});
    }
  }
  // Network closes the Locations group, after the drives (mockup §1).
  places.push_back({.label = "Network", .glyph = "network", .onClick = [this] { goNetwork(); }, .id = "network",
                    .art = "network"});
  for (const SidebarTag& tag : m_app.tags().sidebar()) {
    places.push_back({.label = tag.name, .glyph = "tag", .onClick = [this, name = tag.name] { goTag(name); },
                      .id = "tag:" + tag.name, .group = FileBrowser::Place::Group::Tags, .dot = rgbHex(tag.rgb)});
  }
  places.push_back({.label = "Trash", .glyph = count > 0 ? "trash-x" : "trash", .path = {},
                    .caption = count > 0 ? std::to_string(count) : std::string(), .onClick = [this] { goTrash(); },
                    .id = "trash", .dropTrash = true, .art = "trash", .group = FileBrowser::Place::Group::Last});
  if (m_panes[1] != nullptr) {
    m_panes[m_activePane == 0 ? 1 : 0]->setExtraPlaces(places);
  }
  m_browser->setExtraPlaces(std::move(places));
  if (inTrash()) {
    m_browser->showListing(std::move(listing));
  } else {
    m_trashListing = std::move(listing);
  }
}

void FilesWindow::rebuild() { m_window.requestRebuild(); }

void FilesWindow::goTag(const std::string& tag) {
  // Walks home on a worker the first time (cached 10 min); the listing fills in through tagsChanged().
  m_tagShown = tag;
  m_app.tags().refresh();
  m_browser->showListing(m_app.tags().listing(tag));
}

void FilesWindow::tagsChanged() {
  loadTrash();
  if (!m_tagShown.empty() && m_browser->listingId() == "tag:" + m_tagShown) {
    m_browser->showListing(m_app.tags().listing(m_tagShown));
  }
}

void FilesWindow::eject(const fs::path& mount) {
  // GIO finds the drive behind the mount and ejects it (udisks); a mount that cannot eject is unmounted.
  GFile* file = g_file_new_for_path(mount.c_str());
  GMount* gmount = g_file_find_enclosing_mount(file, nullptr, nullptr);
  g_object_unref(file);
  if (gmount == nullptr) {
    return;
  }
  // The app outlives every window; the callback reaches whichever windows are still open.
  struct Ctx {
    FilesApp* app;
    fs::path mount;
    bool eject;
  };
  const bool canEject = g_mount_can_eject(gmount) != 0;
  auto* ctx = new Ctx{&m_app, mount, canEject};
  const auto done = [](GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Ctx> c(static_cast<Ctx*>(data));
    GError* error = nullptr;
    const bool ok = c->eject ? g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error) != 0
                             : g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error) != 0;
    std::string message;
    if (!ok) {
      message = error != nullptr ? error->message : "unknown error";
      kLog.warn("eject {}: {}", c->mount.string(), message);
      g_clear_error(&error);
    }
    c->app->unmounted(c->mount, message);
  };
  if (canEject) {
    g_mount_eject_with_operation(gmount, G_MOUNT_UNMOUNT_NONE, nullptr, nullptr, done, ctx);
  } else {
    g_mount_unmount_with_operation(gmount, G_MOUNT_UNMOUNT_NONE, nullptr, nullptr, done, ctx);
  }
  g_object_unref(gmount);
}

void FilesWindow::unmounted(const fs::path& mount, const std::string& error) {
  if (!error.empty()) {
    showToast(std::format("“{}” can’t be ejected: {}", mount.filename().string(), error), false);
    return;
  }
  const std::string at = location().string();
  if (at == mount.string() || at.starts_with(mount.string() + "/")) {
    go(g_get_home_dir());
  }
  loadTrash();
}

void FilesWindow::go(const fs::path& location) { m_browser->go(location); }

void FilesWindow::goTrash() {
  loadTrash();
  if (!inTrash()) {
    m_browser->showListing(std::move(m_trashListing));
  }
}

void FilesWindow::goRecents() {
  // The shared GTK/Qt recent-files list; entries that no longer exist drop out.
  FileBrowser::Listing listing{.id = "recents", .title = "Recents", .rows = {}, .activatable = true};
  GBookmarkFile* bookmarks = g_bookmark_file_new();
  const std::string file = (fs::path(g_get_user_data_dir()) / "recently-used.xbel").string();
  if (g_bookmark_file_load_from_file(bookmarks, file.c_str(), nullptr)) {
    gsize count = 0;
    gchar** uris = g_bookmark_file_get_uris(bookmarks, &count);
    // The file keeps entries in the order they were added: newest use first, by visited (else modified) time.
    std::vector<std::pair<gint64, std::string>> byUse;
    for (gsize i = 0; i < count; ++i) {
      GDateTime* visited = g_bookmark_file_get_visited_date_time(bookmarks, uris[i], nullptr);
      GDateTime* modified = visited == nullptr ? g_bookmark_file_get_modified_date_time(bookmarks, uris[i], nullptr)
                                               : nullptr;
      GDateTime* when = visited != nullptr ? visited : modified;
      byUse.emplace_back(when != nullptr ? g_date_time_to_unix(when) : 0, uris[i]);
    }
    g_strfreev(uris);
    std::ranges::sort(byUse, std::greater<>{}, &std::pair<gint64, std::string>::first);
    for (const auto& [when, uri] : byUse) {
      if (listing.rows.size() >= kRecentsMax) {
        break;
      }
      gchar* path = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
      if (path == nullptr) {
        continue;
      }
      if (auto info = statFile(fs::path(path)); info.has_value() && !info->isDir) {
        info->name = path; // absolute: the row opens as a path
        listing.rows.push_back(std::make_shared<const FileInfo>(std::move(*info)));
      }
      g_free(path);
    }
  }
  g_bookmark_file_free(bookmarks);
  m_browser->showListing(std::move(listing));
}

void FilesWindow::goStarred() {
  // Starred items that are gone (or on a drive that is not mounted) do not show; they stay in the list.
  FileBrowser::Listing listing{.id = "starred", .title = "Starred", .rows = {}, .activatable = true};
  for (const fs::path& path : m_app.starred().paths()) {
    if (auto info = statFile(path); info.has_value()) {
      info->name = path.string(); // absolute: the row opens as a path
      listing.rows.push_back(std::make_shared<const FileInfo>(std::move(*info)));
    }
  }
  m_browser->showListing(std::move(listing));
}

bool FilesWindow::selectionStarred() const {
  const auto paths = selectedPaths();
  return !paths.empty() && std::ranges::all_of(paths, [this](const fs::path& p) { return m_app.starred().contains(p); });
}

void FilesWindow::toggleStar() {
  const auto paths = selectedPaths();
  if (paths.empty() || inTrash()) {
    return;
  }
  const bool starred = m_app.starred().toggle(paths);
  showToast(paths.size() == 1 ? std::format("{} “{}”", starred ? "Starred" : "Unstarred", paths.front().filename().string())
                              : std::format("{} {} items", starred ? "Starred" : "Unstarred", paths.size()),
            false);
  if (m_browser->listingId() == "starred") {
    goStarred();
  }
}

void FilesWindow::showItemMenu(std::size_t index, float sceneX, float sceneY) {
  if (m_picker.has_value() || index >= m_browser->rows().size()) {
    return;
  }
  // A right click on an item outside the selection selects that item alone (Finder).
  const FileInfo& row = *m_browser->rows()[index];
  const fs::path clicked = m_browser->pathOf(row);
  if (!std::ranges::contains(selectedPaths(), clicked)) {
    m_browser->selectNames({row.name});
  }
  const auto paths = selectedPaths();
  if (paths.empty()) {
    return;
  }
  CommandTable& commands = m_app.commands();
  std::vector<std::string> ids; // entry id - 1 -> command id
  std::vector<ContextMenuControlEntry> entries;
  const auto add = [&](const char* command, std::string label = {}) {
    const AppCommand* c = commands.find(command);
    if (c == nullptr || (c->enabled && !c->enabled())) {
      return;
    }
    ids.emplace_back(command);
    entries.push_back({.id = static_cast<std::int32_t>(ids.size()),
                       .label = label.empty() ? c->label : std::move(label),
                       .shortcut = c->shortcut ? keyChordDisplayLabel(*c->shortcut) : std::string()});
  };
  const auto separator = [&] {
    if (!entries.empty() && !entries.back().separator) {
      entries.push_back({.separator = true});
    }
  };
  const std::string name = paths.size() == 1 ? paths.front().filename().string() : std::string();
  if (inTrash()) {
    add("file.restore");
    separator();
    add("file.info");
  } else {
    add("file.open");
    add("file.open-with");
    separator();
    add("file.info");
    add("file.rename");
    add("file.compress", name.empty() ? std::format("Compress {} Items", paths.size()) : std::format("Compress “{}”", name));
    add("file.extract-here");
    add("edit.copy");
    add("edit.cut");
    add("edit.copy-location");
    separator();
    add("file.star", selectionStarred() ? "Remove from Starred" : "Add to Starred");
    add("file.open-terminal");
    // Tags: each one checks when every selected item has it; a click adds or removes it on all of them.
    const auto defs = loadTagConfig();
    if (!defs.empty() && m_app.fileOps() != nullptr) {
      ContextMenuControlEntry tags{.id = 0, .label = "Tags", .hasSubmenu = true};
      for (std::size_t t = 0; t < defs.size(); ++t) {
        const bool all = std::ranges::all_of(paths, [&](const fs::path& p) {
          return std::ranges::contains(readTags(p), defs[t].name);
        });
        tags.children.push_back({.id = static_cast<std::int32_t>(kTagEntryBase + t), .label = defs[t].name,
                                 .checkmark = true, .toggleState = all ? 1 : 0,
                                 .indicatorColor = ColorSpec{.role = std::nullopt, .fixed = rgbHex(defs[t].rgb), .alpha = 1.0F}});
      }
      entries.push_back(std::move(tags));
    }
    separator();
    add("edit.trash");
  }
  m_window.openContextMenu(std::move(entries), sceneX, sceneY,
                           [this, ids, paths, alive = std::weak_ptr<bool>(m_alive)](std::int32_t id) {
                             if (alive.expired()) {
                               return;
                             }
                             if (id >= static_cast<std::int32_t>(kTagEntryBase)) {
                               const auto defs = loadTagConfig();
                               const auto t = static_cast<std::size_t>(id) - kTagEntryBase;
                               if (t < defs.size() && m_app.fileOps() != nullptr) {
                                 const bool all = std::ranges::all_of(paths, [&](const fs::path& p) {
                                   return std::ranges::contains(readTags(p), defs[t].name);
                                 });
                                 m_app.fileOps()->setTags(paths, {defs[t].name}, all ? "remove" : "add");
                               }
                               return;
                             }
                             if (id > 0 && static_cast<std::size_t>(id) <= ids.size()) {
                               kLog.debug("menu {}", ids[static_cast<std::size_t>(id) - 1]);
                               m_app.commands().run(ids[static_cast<std::size_t>(id) - 1]);
                             }
                           });
}

void FilesWindow::openTerminalHere() {
  const auto paths = selectedPaths();
  const fs::path dir = paths.size() == 1 && isDir(paths.front()) ? paths.front() : location();
  if (openTerminalIn(dir).empty()) {
    showToast("No terminal app is installed", false);
  }
}

void FilesWindow::openArchive(const fs::path& archive) {
  FileOpsClient* ops = m_app.fileOps();
  if (ops == nullptr) {
    showToast("File operations are not available", false);
    return;
  }
  ops->listArchive(archive, [this, alive = std::weak_ptr<bool>(m_alive), archive](
                                std::vector<FileOpsClient::ArchiveEntry> entries, const std::string& error,
                                bool passwordNeeded) {
    if (alive.expired()) {
      return;
    }
    if (passwordNeeded) {
      openExtractSheet(archive, ExtractSheet::State::PasswordNeeded);
      return;
    }
    if (!error.empty()) {
      showToast(error, false);
      return;
    }
    m_archive = std::make_unique<ArchiveListing>(archive, entries);
    kLog.debug("archive {} {} items", archive.filename().string(), m_archive->itemCount());
    showArchive({});
  });
}

void FilesWindow::showArchive(const fs::path& inner) {
  if (m_archive == nullptr) {
    return;
  }
  FileBrowser::Listing listing = m_archive->listing(inner);
  listing.open = [this](const FileInfo& row) {
    if (m_archive == nullptr) {
      return true;
    }
    const auto in = m_archive->innerOf(row.name);
    if (in.has_value() && m_archive->isFolder(*in)) {
      DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), next = *in] {
        if (!alive.expired()) {
          showArchive(next);
        }
      });
    }
    return true; // files inside open after Extract (Quick Look inside archives is not built)
  };
  listing.banner = [this, archive = m_archive->archive()](float scale) {
    return m_archive->buildExtractBar(
        scale, [this, archive] { openExtractSheet(archive, ExtractSheet::State::Normal); },
        [this, archive] { extractHere(archive); });
  };
  m_browser->showListing(std::move(listing));
}

void FilesWindow::extractHere(const fs::path& archive) {
  startExtract(archive, {.destination = archive.parent_path(), .password = {}, .showWhenDone = false, .trashArchive = false});
}

void FilesWindow::openExtractSheet(const fs::path& archive, ExtractSheet::State state) {
  std::vector<std::string> top;
  if (m_archive != nullptr && m_archive->archive() == archive) {
    top = m_archive->topNames();
  }
  m_extract = std::make_unique<ExtractSheet>(
      archive, std::move(top),
      [this, archive](const ExtractSheet::Request& request) {
        m_window.dismissSheet();
        startExtract(archive, request);
      },
      ExtractSheet::ChooseFolder{}, [this] { m_window.dismissSheet(); }, [this] { m_window.requestRebuild(); });
  m_extract->setState(state);
  for (const auto& [job, pending] : m_extracts) {
    if (pending.archive == archive) {
      m_extract->setDestination(pending.request.destination);
    }
  }
  m_window.presentSheet([this](float scale) { return m_extract->build(scale); }, [this] { m_window.dismissSheet(); });
}

void FilesWindow::startExtract(const fs::path& archive, ExtractSheet::Request request) {
  startJob(
      "Extracting", 1,
      [&](FileOpsClient& ops, auto done) { ops.extract(archive, {}, request.destination, request.password, done); },
      [this, archive, request](const std::string& job) {
        m_extracts[job] = PendingExtract{.archive = archive, .request = request};
      });
}

bool FilesWindow::canExtract() const {
  const auto paths = m_browser->selectedPaths();
  return (paths.size() == 1 && ArchiveListing::isArchive(paths.front())) ||
         (m_archive != nullptr && ArchiveListing::isArchiveId(m_browser->listingId()));
}

void FilesWindow::extractSelection(bool here) {
  const auto paths = m_browser->selectedPaths();
  fs::path archive;
  if (paths.size() == 1 && ArchiveListing::isArchive(paths.front())) {
    archive = paths.front();
  } else if (m_archive != nullptr && ArchiveListing::isArchiveId(m_browser->listingId())) {
    archive = m_archive->archive();
  } else {
    return;
  }
  if (here) {
    extractHere(archive);
  } else {
    openExtractSheet(archive, ExtractSheet::State::Normal);
  }
}

void FilesWindow::getInfo() {
  const auto paths = selectedPaths();
  if (paths.empty()) {
    return;
  }
  m_info = std::make_unique<GetInfoSheet>(
      paths, m_app.fileOps(), [this] { m_window.dismissSheet(); }, [this] { m_window.requestRebuild(); },
      [this] { m_window.invalidate(); });
  m_window.presentSheet([this](float scale) { return m_info->build(scale); }, [this] { m_window.dismissSheet(); });
}

void FilesWindow::showStorage() {
  auto page = std::make_shared<StoragePage>(
      m_app.fileOps(), g_get_home_dir(),
      StoragePage::Actions{
          .rebuild = [this] { m_window.requestRebuild(); },
          .emptyTrash = [this] { emptyTrash(); },
          .trash =
              [this](std::vector<fs::path> paths) {
                startJob("Moving to Trash", paths.size(), [&](FileOpsClient& ops, auto done) { ops.trash(paths, done); });
              },
          .remove =
              [this](std::vector<fs::path> paths) {
                startJob("Deleting", paths.size(), [&](FileOpsClient& ops, auto done) { ops.remove(paths, done); });
              },
          .open = [this](const fs::path& folder) { m_browser->jump(folder); },
      });
  m_storage = page;
  page->scan();
  m_browser->showListing({.id = "storage", .title = "Storage", .rows = {}, .activatable = false,
                          .page = [page](float scale) { return page->build(scale); }});
}

void FilesWindow::askEmptyTrash() {
  loadTrash(); // other apps may have trashed or restored items since the last read
  const std::size_t count = m_trash.size();
  if (count == 0) {
    return;
  }
  m_window.presentSheet(
      [this, count](float s) {
        auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                               .width = Tokens::componentFilesNameField * s});
        box->addChild(ui::label({.text = std::string("Empty the Trash?"), .fontSize = Tokens::typeTitle3.size * s,
                                 .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
        box->addChild(ui::label({.text = std::format("{} {} deleted for good. You can’t undo this.", noun(count),
                                                     count == 1 ? "is" : "are"),
                                 .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2),
                                 .maxLines = 3}));
        auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
        buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Style::fontSizeBody * s,
                                      .variant = ButtonVariant::Default, .onClick = [this] { m_window.dismissSheet(); }}));
        buttons->addChild(ui::button({.text = std::string("Empty Trash"), .fontSize = Style::fontSizeBody * s,
                                      .variant = ButtonVariant::Destructive, .onClick = [this] {
                                        m_window.dismissSheet();
                                        emptyTrash();
                                      }}));
        box->addChild(std::move(buttons));
        return box;
      },
      [this] { m_window.dismissSheet(); });
}

void FilesWindow::emptyTrash() {
  // Each item and its .trashinfo go for good (the user confirmed); no undo. Read again: "empty" means what is there now.
  loadTrash();
  std::vector<fs::path> paths;
  for (const TrashItem& item : m_trash) {
    paths.push_back(item.trashDir / "files" / item.name);
    paths.push_back(item.trashDir / "info" / (item.name + ".trashinfo"));
  }
  if (paths.empty()) {
    return;
  }
  startJob("Emptying Trash", m_trash.size(), [&](FileOpsClient& ops, auto done) { ops.remove(paths, done); });
}

void FilesWindow::compressSelection() {
  const auto paths = m_browser->selectedPaths();
  if (paths.empty()) {
    return;
  }
  m_compress = std::make_unique<CompressSheet>(
      paths,
      [this, paths](const fs::path& destination, const std::string& format, const std::string& password) {
        m_window.dismissSheet();
        startJob("Compressing", paths.size(), [&](FileOpsClient& ops, auto done) {
          ops.compress(paths, destination, format, password, done);
        });
      },
      [this] { m_window.dismissSheet(); }, [this] { m_window.requestRebuild(); });
  m_window.presentSheet([this](float scale) { return m_compress->build(scale); }, [this] { m_window.dismissSheet(); });
}

void FilesWindow::goThisPc() {
  ThisPcOptions options;
  for (const auto& [dir, label] : {std::pair{G_USER_DIRECTORY_DESKTOP, "Desktop"}, {G_USER_DIRECTORY_DOCUMENTS, "Documents"},
                                   {G_USER_DIRECTORY_DOWNLOAD, "Downloads"}, {G_USER_DIRECTORY_PICTURES, "Pictures"},
                                   {G_USER_DIRECTORY_MUSIC, "Music"}, {G_USER_DIRECTORY_VIDEOS, "Videos"}}) {
    const char* path = g_get_user_special_dir(dir);
    if (path != nullptr && std::string_view(path) != g_get_home_dir()) {
      options.folders.push_back({.label = label, .path = path, .art = {}, .caption = {}});
    }
  }
  const std::size_t trash = m_trash.size();
  options.extra.push_back({.label = "Storage", .path = {}, .art = "drive", .caption = "See what uses the disk"});
  options.extra.push_back({.label = "Network", .path = {}, .art = "network", .caption = "Servers and shared folders"});
  options.extra.push_back({.label = "Trash", .path = {}, .art = "trash",
                           .caption = trash == 0 ? std::string("Empty") : trash == 1 ? std::string("1 item")
                                                                         : std::format("{} items", trash)});
  options.open = [this, alive = std::weak_ptr<bool>(m_alive)](const ThisPcPlace& place) {
    // Deferred: the page that holds the clicked cell goes away with the jump.
    DeferredCall::callLater([this, alive, place] {
      if (alive.expired()) {
        return;
      }
      if (place.label == "Storage") {
        showStorage();
      } else if (place.label == "Network") {
        goNetwork();
      } else if (place.path.empty()) {
        goTrash();
      } else {
        m_browser->jump(place.path);
      }
    });
  };
  m_browser->showListing({.id = "this-pc", .title = "This PC", .rows = {}, .activatable = false,
                          .page = [options = std::move(options)](float scale) { return buildThisPc(scale, options); }});
}

void FilesWindow::goNetwork() {
  if (m_network == nullptr) {
    m_network = std::make_shared<NetworkPage>(NetworkPage::Actions{
        .rebuild = [this] { rebuild(); },
        .open = [this](const fs::path& path) { m_browser->jump(path); },
        .connect = [this](const std::string& address) { openConnect(address); },
    });
    m_network->start();
  }
  m_browser->showListing({.id = "network", .title = "Network", .rows = {}, .activatable = false,
                          .page = [net = m_network](float scale) { return net->build(scale); }});
}

void FilesWindow::openConnect(const std::string& address) {
  if (m_network == nullptr) {
    goNetwork();
  }
  // A fresh sheet never accepts a host key on its first Connect.
  m_network->forgetQuestion();
  // Closing drops the window's reference after this callback (the sheet runs it), so a late mount result finds no
  // sheet and touches nothing that shows now.
  const auto close = [this, alive = std::weak_ptr<bool>(m_alive)] {
    m_window.dismissSheet();
    DeferredCall::callLater([this, alive] {
      if (!alive.expired()) {
        m_connect.reset();
      }
    });
  };
  // The sheet hears the mount result through a weak pointer: closing it first must not crash the callback.
  auto self = std::make_shared<std::weak_ptr<ConnectSheet>>();
  auto sheet = std::make_shared<ConnectSheet>(
      address,
      [this, net = m_network, self](const ServerUrl& url, const std::string& user, const std::string& password,
                                    bool remember) {
        net->mount(url, user, password, remember, [this, self, user, net](const std::string& error, bool needsPassword) {
          auto live = self->lock();
          if (live == nullptr || live != m_connect) {
            return;
          }
          if (error.empty()) {
            m_window.dismissSheet();
            m_connect.reset();
          } else if (needsPassword) {
            live->askPassword(user, error);
          } else if (net->questionPending()) {
            live->setQuestion(error);
          } else {
            live->setError(error);
          }
        });
      },
      close, [this] { m_window.requestRebuild(); });
  *self = sheet;
  m_connect = sheet;
  m_window.presentSheet([sheet](float scale) { return sheet->build(scale); }, close);
}

void FilesWindow::openPathField(const std::string& initial) {
  // Go to Folder (mockup §13: Ctrl+L, or "/" or "~" typed in the list): a path with folder completion. Tab takes the
  // shared start of the matches; Enter opens a folder, reveals a file, or in a Save picker fills folder and name.
  m_pathText = initial.empty() ? location().string() + "/" : initial;
  m_window.presentSheet([this](float s) { return buildPathSheet(s); }, [this] { m_window.dismissSheet(); });
}

std::string FilesWindow::absolutePathText() const {
  // "~" is home; anything not absolute is under the current folder (never the process directory).
  if (m_pathText.starts_with("~")) {
    return std::string(g_get_home_dir()) + m_pathText.substr(1);
  }
  if (!m_pathText.starts_with("/")) {
    return (location() / m_pathText).string();
  }
  return m_pathText;
}

std::vector<std::string> FilesWindow::pathMatches() const {
  const std::string text = absolutePathText();
  const std::size_t slash = text.rfind('/');
  if (slash == std::string::npos) {
    return {};
  }
  const fs::path dir = text.substr(0, slash + 1);
  const std::string prefix = TypeAhead::fold(text.substr(slash + 1));
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if ((name.starts_with('.') && !prefix.starts_with('.')) || !it->is_directory(ec)) {
      continue;
    }
    if (TypeAhead::fold(name).starts_with(prefix)) {
      out.push_back((dir / name).string() + "/");
    }
    if (out.size() > kPathMatchScan) {
      break;
    }
  }
  std::ranges::sort(out);
  out.resize(std::min(out.size(), kPathMatchesShown));
  return out;
}

void FilesWindow::completePath() {
  const auto matches = pathMatches();
  if (matches.empty()) {
    return;
  }
  std::string common = matches.front();
  for (const std::string& m : matches) {
    std::size_t n = 0;
    while (n < common.size() && n < m.size() && common[n] == m[n]) {
      ++n;
    }
    common.resize(n);
  }
  if (common.size() > m_pathText.size()) {
    m_pathText = common;
    m_window.requestRebuild();
  }
}

void FilesWindow::submitPath() {
  const fs::path path = fs::path(absolutePathText()).lexically_normal();
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    m_window.dismissSheet();
    m_browser->jump(path);
    return;
  }
  if (m_picker.has_value() && m_picker->request.kind == PickerPortal::Request::Kind::Save &&
      fs::is_directory(path.parent_path(), ec)) {
    m_window.dismissSheet();
    m_browser->jump(path.parent_path());
    setPickerName(path.filename().string());
    return;
  }
  if (fs::exists(path, ec)) {
    m_window.dismissSheet();
    m_browser->reveal(path);
    return;
  }
  m_pathError = "No such folder.";
  m_window.requestRebuild();
}

std::unique_ptr<Node> FilesWindow::buildPathSheet(float s) {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                         .width = Tokens::componentFilesNameField * s});
  box->addChild(ui::label({.text = std::string("Go to Folder"), .fontSize = Tokens::typeTitle3.size * s,
                           .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  box->addChild(ui::input({
      .value = m_pathText,
      .placeholder = std::string("/path/to/folder"),
      .fontSize = Style::fontSizeBody * s,
      .invalid = !m_pathError.empty(),
      .onChange =
          [this](const std::string& text) {
            m_pathText = text;
            m_pathError.clear();
          },
      .onSubmit = [this](const std::string&) { submitPath(); },
      .onKeyEvent =
          [this](std::uint32_t sym, std::uint32_t modifiers) {
            if (sym == XKB_KEY_Tab && (modifiers & (KeyMod::Ctrl | KeyMod::Alt)) == 0) {
              completePath();
              return true;
            }
            return false;
          },
      .configure =
          [this](Input& input) {
            input.selectRange(input.value().size(), input.value().size());
            m_window.input().setFocus(input.inputArea());
          },
  }));
  if (!m_pathError.empty()) {
    box->addChild(ui::label({.text = m_pathError, .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::DangerText)}));
  }
  for (const std::string& match : pathMatches()) {
    box->addChild(ui::label({.text = match, .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                             .ellipsize = TextEllipsize::Middle}));
  }
  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
  buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Default, .onClick = [this] { m_window.dismissSheet(); }}));
  buttons->addChild(ui::button({.text = std::string("Go"), .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Primary, .onClick = [this] { submitPath(); }}));
  box->addChild(std::move(buttons));
  return box;
}

void FilesWindow::focusSearch() { m_browser->focusSearch(); }
void FilesWindow::newTab() { m_browser->newTab(); }
bool FilesWindow::closeTab() { return m_browser->closeTab(); }
void FilesWindow::stepTab(int delta) { m_browser->stepTab(delta); }
std::size_t FilesWindow::tabCount() const { return m_browser->tabCount(); }
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
  // One item opens as a click would: folders, archives, rows of an archive listing, the picker.
  if (m_browser->activateSelection()) {
    return;
  }
  const auto paths = selectedPaths();
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

bool FilesWindow::canOpenWith() const {
  if (inTrash() || m_picker.has_value() || !hasSelection()) {
    return false;
  }
  return std::ranges::none_of(selectedPaths(), isDir);
}

void FilesWindow::openWith() {
  if (!canOpenWith()) {
    return;
  }
  const auto paths = selectedPaths();
  const auto info = statFile(paths.front());
  AppChooserPortal::ChooseRequest request{.appId = {}, .parentWindow = {}, .choices = {}, .lastChoice = {},
                                          .contentType = info ? info->contentType : std::string(),
                                          .filename = paths.size() == 1 ? paths.front().filename().string() : ""};
  // The answer runs inside the sheet: the sheet goes after this callback.
  m_openWith = std::make_shared<AppChooserSheet>(
      std::move(request),
      [this, paths, alive = std::weak_ptr<bool>(m_alive)](std::uint32_t response, const std::string& id) {
        m_window.dismissSheet();
        if (response == 0) {
          launchWith(id, paths);
        }
        DeferredCall::callLater([this, alive] {
          if (!alive.expired()) {
            m_openWith.reset();
          }
        });
      },
      [this] { m_window.requestRebuild(); });
  m_window.presentSheet([sheet = m_openWith](float scale) { return sheet->build(scale); },
                        [sheet = std::weak_ptr<AppChooserSheet>(m_openWith)] {
                          if (const auto live = sheet.lock()) {
                            live->cancel();
                          }
                        });
}

void FilesWindow::launchWith(const std::string& id, const std::vector<fs::path>& paths) {
  GDesktopAppInfo* app = g_desktop_app_info_new((id + ".desktop").c_str());
  if (app == nullptr) {
    showToast("Can’t find that app", false);
    return;
  }
  GList* uris = nullptr;
  for (const fs::path& path : paths) {
    uris = g_list_append(uris, g_strdup(uriFor(path).c_str()));
  }
  GError* error = nullptr;
  if (!g_app_info_launch_uris(G_APP_INFO(app), uris, nullptr, &error)) {
    showToast(std::format("Can’t open with {}", g_app_info_get_name(G_APP_INFO(app))), false);
    kLog.warn("open with {}: {}", id, error != nullptr ? error->message : "?");
    g_clear_error(&error);
  } else {
    kLog.info("opened {} items with {}", paths.size(), id);
  }
  g_list_free_full(uris, g_free);
  g_object_unref(app);
}

void FilesWindow::copySelection(bool cut) {
  if (inTrash() || !hasSelection()) {
    return;
  }
  const auto paths = selectedPaths();
  m_app.clipboard() = {.paths = paths, .cut = cut};
  // The system clipboard too, so other apps paste the files (GNOME, KDE and browsers read one of these types).
  std::string uris;
  std::string text;
  for (const fs::path& p : paths) {
    uris += uriFor(p) + "\r\n";
    text += (text.empty() ? "" : "\n") + p.string();
  }
  std::string gnome = cut ? "cut" : "copy";
  for (const fs::path& p : paths) {
    gnome += "\n" + uriFor(p);
  }
  if (DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop()) {
    dnd->setSelection(m_app.loop().wayland().lastInputSerial(),
                      {.mimeTypes = {"x-special/gnome-copied-files", "text/uri-list", "application/x-kde-cutselection",
                                     "text/plain;charset=utf-8", "text/plain"},
                       .data = [uris, text, gnome, cut](const std::string& mime) -> std::string {
                         if (mime == "x-special/gnome-copied-files") {
                           return gnome;
                         }
                         if (mime == "text/uri-list") {
                           return uris;
                         }
                         if (mime == "application/x-kde-cutselection") {
                           return cut ? "1" : "0";
                         }
                         return text;
                       }});
  }
  showToast(std::format("{} {}", cut ? "Cut" : "Copied", noun(paths.size())), false);
}

void FilesWindow::copyLocation() {
  const auto paths = selectedPaths();
  std::string text;
  for (const fs::path& p : paths.empty() ? std::vector<fs::path>{location()} : paths) {
    text += (text.empty() ? "" : "\n") + p.string();
  }
  if (DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop()) {
    dnd->setSelection(m_app.loop().wayland().lastInputSerial(),
                      {.mimeTypes = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING"},
                       .data = [text](const std::string&) { return text; }});
    kLog.debug("location copied {}", text);
    showToast("Copied location", false);
  }
}

void FilesWindow::startJob(const char* verb, std::size_t count, std::function<void(FileOpsClient&, FileOpsClient::Started)> start,
                           std::function<void(const std::string& job)> started) {
  FileOpsClient* ops = m_app.fileOps();
  if (ops == nullptr) {
    showToast("File operations are not available", false);
    return;
  }
  showToast(std::format("{} {}…", verb, noun(count)), false);
  start(*ops, [this, alive = std::weak_ptr<bool>(m_alive), verb = std::string(verb),
               started = std::move(started)](const std::string& job,
                                                                                       const std::string& error) {
    if (alive.expired()) {
      return;
    }
    if (job.empty()) {
      showToast(std::format("{} failed: {}", verb, error), false);
      return;
    }
    m_jobs.insert(job);
    if (started) {
      started(job);
    }
  });
}

void FilesWindow::paste() {
  if (inTrash()) {
    return;
  }
  // Files another app copied come from the system clipboard; our own copy stays in m_app.clipboard().
  DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop();
  if (dnd != nullptr && !dnd->ownsSelection() && !dnd->selectionTypes().empty()) {
    dnd->readSelection({"x-special/gnome-copied-files", "text/uri-list"},
                       [this, alive = std::weak_ptr<bool>(m_alive)](const std::string& mime, std::string bytes) {
                         if (alive.expired() || mime.empty()) {
                           return;
                         }
                         bool cut = false;
                         std::vector<fs::path> paths;
                         std::istringstream lines(bytes);
                         std::string line;
                         for (bool first = true; std::getline(lines, line); first = false) {
                           if (!line.empty() && line.back() == '\r') {
                             line.pop_back();
                           }
                           if (first && mime == "x-special/gnome-copied-files") {
                             cut = line == "cut";
                             continue;
                           }
                           if (line.empty() || line.front() == '#') {
                             continue;
                           }
                           if (gchar* path = g_filename_from_uri(line.c_str(), nullptr, nullptr)) {
                             paths.emplace_back(path);
                             g_free(path);
                           }
                         }
                         kLog.debug("paste {} items from the clipboard ({})", paths.size(), cut ? "cut" : "copy");
                         if (paths.empty()) {
                           return;
                         }
                         m_app.clipboard() = {.paths = std::move(paths), .cut = cut};
                         pasteInternal();
                       });
    return;
  }
  pasteInternal();
}

void FilesWindow::pasteInternal() {
  auto clip = m_app.clipboard();
  if (clip.paths.empty()) {
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
  auto& stack = m_app.undoStack();
  if (ops == nullptr || stack.empty()) {
    return;
  }
  const std::string job = stack.back();
  stack.pop_back();
  kLog.debug("undo {}", job);
  startJob("Undoing", 1, [&](FileOpsClient& c, auto done) { c.undo(job, done); });
}

bool FilesWindow::canUndo() const noexcept { return !m_app.undoStack().empty(); }

void FilesWindow::showConflict(const FileOpsClient::Job& job) {
  const auto& [source, target] = *job.conflict;
  m_conflict = std::make_shared<ConflictSheet>(
      ConflictSheet::Info{.verb = job.kind == "move" ? "Moving" : "Copying", .source = source, .target = target,
                          .totalFiles = job.totalFiles, .conflictIndex = job.conflictIndex,
                          .conflictTotal = job.conflictTotal, .folderClash = job.folderClash},
      &m_app.thumbnails(),
      [this, alive = std::weak_ptr<bool>(m_alive)](ConflictSheet::Choice choice, bool applyToAll) {
        if (alive.expired()) {
          return;
        }
        m_applyToAll = applyToAll;
        switch (choice) {
        case ConflictSheet::Choice::Replace:
          resolveConflict(FileOpsClient::Resolution::Replace);
          break;
        case ConflictSheet::Choice::Skip:
          resolveConflict(FileOpsClient::Resolution::Skip);
          break;
        case ConflictSheet::Choice::KeepBoth:
          resolveConflict(FileOpsClient::Resolution::KeepBoth);
          break;
        case ConflictSheet::Choice::Stop:
          if (m_conflictJob.has_value() && m_app.fileOps() != nullptr) {
            m_app.fileOps()->cancel(*m_conflictJob);
          }
          closeConflict();
          break;
        }
      },
      [this] { m_window.requestRebuild(); });
  kLog.debug("job conflict sheet {}", target.filename().string());
  m_window.presentSheet([sheet = m_conflict](float scale) { return sheet->build(scale); },
                        [sheet = std::weak_ptr<ConflictSheet>(m_conflict)] {
                          if (const auto live = sheet.lock()) {
                            live->choose(ConflictSheet::Choice::Skip);
                          }
                        });
}

void FilesWindow::closeConflict() {
  m_conflictJob.reset();
  m_applyToAll = false;
  if (m_conflict != nullptr) {
    m_window.dismissSheet();
    // The sheet's own button runs this: it goes after the callback.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        m_conflict.reset();
      }
    });
  }
}

void FilesWindow::resolveConflict(FileOpsClient::Resolution resolution) {
  if (!m_conflictJob.has_value() || m_app.fileOps() == nullptr) {
    return;
  }
  m_app.fileOps()->resolveConflict(*m_conflictJob, resolution, m_applyToAll);
  closeConflict();
  rebuild();
}

void FilesWindow::onJobChanged(const FileOpsClient::Job& job) {
  if (m_jobs.contains(job.path)) {
    kLog.debug("job {} {} {}", job.kind, job.state, job.path);
  }
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
    // The same job's next question (k moved on) gets a fresh sheet too.
    if (m_conflictJob != job.path || m_conflict == nullptr || job.conflictIndex != m_conflictShownIndex) {
      m_conflictJob = job.path;
      m_conflictShownIndex = job.conflictIndex;
      showConflict(job);
    }
    return;
  }
  if (!job.finished()) {
    if (job.totalFiles > 1) {
      showToast(std::format("{} of {}…", job.doneFiles, job.totalFiles), false);
    }
    return;
  }
  m_jobs.erase(job.path);
  if (auto it = m_renameDone.find(job.path); it != m_renameDone.end()) {
    auto done = std::move(it->second);
    m_renameDone.erase(it);
    done(job.state == "done" ? std::string() : job.error.empty() ? std::string("The name could not be changed.") : job.error);
  }
  kLog.debug("job {} {}{}", job.kind, job.state, job.undoable ? " undoable" : "");
  if (m_conflictJob == job.path) {
    closeConflict();
  }
  if (job.state == "done") {
    const bool undoable = job.undoable && job.kind != "undo";
    if (undoable) {
      m_app.undoStack().push_back(job.path);
    }
    const std::string what = job.kind == "trash" ? "Moved to Trash"
        : job.kind == "copy"                    ? "Copied"
        : job.kind == "move"                    ? "Moved"
        : job.kind == "restore"                 ? "Restored"
        : job.kind == "undo"                    ? "Undone"
        : job.kind == "rename"                  ? "Renamed"
        : job.kind == "new_folder"              ? "Created"
        : job.kind == "link"                    ? "Linked"
        : job.kind == "extract"                 ? "Extracted"
        : job.kind == "compress"                ? "Compressed"
                                                : "Done";
    showToast(std::format("{} {}", what, noun(job.doneFiles > 0 ? job.doneFiles : 1)), undoable);
  } else if (job.state == "failed" && job.kind == "extract" && m_extracts.contains(job.path) &&
             (job.error.ends_with("password needed") || job.error.ends_with("wrong password"))) {
    openExtractSheet(m_extracts.at(job.path).archive, job.error.ends_with("wrong password")
                                                           ? ExtractSheet::State::WrongPassword
                                                           : ExtractSheet::State::PasswordNeeded);
  } else if (job.state == "failed") {
    showToast(job.error.empty() ? std::string("The operation failed") : job.error, false);
  }
  // A finished, failed or cancelled extract leaves the map; only a done one runs its follow-up.
  if (const auto extract = m_extracts.find(job.path);
      job.kind == "extract" && extract != m_extracts.end() && job.state != "running" && job.state != "conflict") {
    const PendingExtract done = extract->second;
    m_extracts.erase(extract);
    if (job.state == "done" && done.request.trashArchive && m_app.fileOps() != nullptr) {
      m_app.fileOps()->trash({done.archive});
    }
    if (job.state == "done" && done.request.showWhenDone) {
      m_browser->jump(done.request.destination);
    }
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

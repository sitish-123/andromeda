#include "files_window.h"
#include "server_url.h"
#include "app_chooser_sheet.h"
#include "conflict_sheet.h"
#include "connect_sheet.h"
#include "vault_sheet.h"
#include "core/input/key_chord.h"
#include "details_pane.h"
#include "job_pill.h"
#include "ui/controls/countdown_ring.h"
#include "tags.h"
#include "tag_manager_sheet.h"
#include "terminal_pane.h"
#include "select_pattern_sheet.h"
#include "smart_folder_sheet.h"
#include "exif_date.h"
#include "gallery_video.h"
#include "offline_map.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"
#include "terminal.h"
#include "service_menus.h"
#include "view_store.h"
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
#include "ui/files/file_gallery.h"
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
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <utility>
#include <format>
#include <sstream>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <thread>
#include <sys/xattr.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

// A fixed-size clip whose one child sits shifted by a slide offset: a pane moves on its spring with no relayout
// (the content beside it reflows once, at the start of an open and at the end of a close).
class SlideClip final : public Flex {
public:
  void setSlide(float x, float y) {
    m_slideX = x;
    m_slideY = y;
    place();
    markPaintDirty();
  }

protected:
  void doLayout(Renderer& renderer) override {
    Flex::doLayout(renderer);
    takeBase();
  }
  void doArrange(Renderer& renderer, const LayoutRect& rect) override {
    Flex::doArrange(renderer, rect);
    takeBase();
  }

private:
  void takeBase() {
    if (!children().empty()) {
      m_baseX = children().front()->x();
      m_baseY = children().front()->y();
    }
    place();
  }
  void place() {
    if (!children().empty()) {
      children().front()->setPosition(m_baseX + m_slideX, m_baseY + m_slideY);
    }
  }
  float m_baseX = 0.0F;
  float m_baseY = 0.0F;
  float m_slideX = 0.0F;
  float m_slideY = 0.0F;
};

namespace {
  std::unique_ptr<SlideClip> slideClip(FlexDirection direction, float width, float height) {
    auto clip = std::make_unique<SlideClip>();
    clip->setDirection(direction);
    clip->setAlign(FlexAlign::Stretch);
    clip->setClipChildren(true);
    if (width > 0.0F) {
      clip->setMinWidth(width);
      clip->setMaxWidth(width);
    }
    if (height > 0.0F) {
      clip->setMinHeight(height);
      clip->setMaxHeight(height);
    }
    return clip;
  }
  // A Google motion photo keeps an MP4 after the JPEG data (MotionPhoto / MicroVideo). Its start is the last "ftyp"
  // box with a plausible size and brand; the tail goes to a private runtime file the media helper can open. Blocks:
  // runs on a worker thread. Empty when the photo has no video.
  fs::path cutMotionVideo(const fs::path& photo) {
    constexpr std::uintmax_t kMaxBytes = 64ULL << 20;
    std::error_code ec;
    const auto size = fs::file_size(photo, ec);
    if (ec || size < 64 || size > kMaxBytes) {
      return {};
    }
    std::ifstream in(photo, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
      return {};
    }
    std::size_t start = std::string::npos;
    for (std::size_t at = bytes.find("ftyp", 8); at != std::string::npos; at = bytes.find("ftyp", at + 4)) {
      const auto u8 = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])); };
      const std::uint32_t box = (u8(at - 4) << 24) | (u8(at - 3) << 16) | (u8(at - 2) << 8) | u8(at - 1);
      const std::string_view brand = std::string_view(bytes).substr(at + 4, 4);
      if (box >= 16 && box <= 64 && at + 4 + 4 <= bytes.size() &&
          (brand == "mp42" || brand == "isom" || brand == "mp41" || brand == "avc1" || brand == "iso5" || brand == "iso6")) {
        start = at - 4;
      }
    }
    if (start == std::string::npos) {
      return {};
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const fs::path dir = fs::path(runtime != nullptr && *runtime != '\0' ? runtime : "/tmp") / "andromeda-files";
    fs::create_directories(dir, ec);
    fs::permissions(dir, fs::perms::owner_all, ec);
    // One file per process, replaced by rename: a helper still reading the previous one keeps its inode.
    const fs::path out = dir / std::format("motion-{}.mp4", ::getpid());
    const fs::path tmp = dir / std::format("motion-{}.mp4.part", ::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      return {};
    }
    const std::string_view video = std::string_view(bytes).substr(start);
    bool ok = true;
    for (std::size_t done = 0; ok && done < video.size();) {
      const ssize_t n = ::write(fd, video.data() + done, video.size() - done);
      ok = n > 0;
      done += ok ? static_cast<std::size_t>(n) : 0;
    }
    ok = ::close(fd) == 0 && ok;
    if (!ok || ::rename(tmp.c_str(), out.c_str()) != 0) {
      ::unlink(tmp.c_str());
      return {};
    }
    return out;
  }

  // The Gallery Info card content (mockup section 15). Blocks on file reads: runs on a worker thread.
  FileGallery::PhotoInfo readPhotoInfo(const fs::path& path, int mapW, int mapH, float scale,
                                       const std::vector<std::string>& tags) {
    FileGallery::PhotoInfo info;
    info.name = path.filename().string();
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (!ext.empty()) {
      ext.erase(0, 1);
    }
    const bool exifType = ext == "JPG" || ext == "JPEG" || ext == "TIF" || ext == "TIFF";
    const PhotoExif exif = exifType ? readPhotoExif(path) : PhotoExif{};
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    info.kind = ext == "JPG" ? std::string("JPEG") : ext;
    if (exif.width > 0 && exif.height > 0) {
      info.kind += std::format(" · {} × {}", exif.width, exif.height);
    }
    if (!ec) {
      info.kind += " · " + formatFileSize(bytes);
    }
    if (!exif.camera.empty()) {
      info.chips.push_back(exif.camera);
    }
    if (exif.fNumber && *exif.fNumber > 0.0) {
      info.chips.push_back(std::format("ƒ/{:g}", std::round(*exif.fNumber * 10.0) / 10.0));
    }
    if (exif.exposureSeconds && *exif.exposureSeconds > 0.0) {
      const double e = *exif.exposureSeconds;
      info.chips.push_back(e < 1.0 ? std::format("1/{} s", std::lround(1.0 / e)) : std::format("{:g} s", e));
    }
    if (exif.iso) {
      info.chips.push_back(std::format("ISO {}", *exif.iso));
    }
    if (exif.focalMm && *exif.focalMm > 0.0) {
      info.chips.push_back(std::format("{} mm", std::lround(*exif.focalMm)));
    }
    if (exif.gps) {
      auto map = offline_map::render(exif.gps->first, exif.gps->second, mapW, mapH, scale);
      info.mapBgra = std::move(map.bgra);
      info.mapWidth = map.width;
      info.mapHeight = map.height;
      info.place = offline_map::placeName(exif.gps->first, exif.gps->second);
    }
    if (exif.taken) {
      // The EXIF wall time comes back as UTC (exif_date.h): show it as written, no zone shift.
      const auto minute = std::chrono::floor<std::chrono::minutes>(*exif.taken);
      const std::chrono::year_month_day day{std::chrono::floor<std::chrono::days>(minute)};
      info.rows.emplace_back("Taken", std::format("{} {:%b %Y, %H:%M}", static_cast<unsigned>(day.day()), minute));
    }
    // Browsers record where a download came from (freedesktop user.xdg.origin.url).
    std::array<char, 2048> origin{};
    const ssize_t n = ::getxattr(path.c_str(), "user.xdg.origin.url", origin.data(), origin.size() - 1);
    if (n > 0) {
      std::string url(origin.data(), static_cast<std::size_t>(n));
      const auto scheme = url.find("://");
      const auto host = scheme == std::string::npos ? std::string() : url.substr(scheme + 3, url.find('/', scheme + 3) - scheme - 3);
      if (!host.empty()) {
        info.rows.emplace_back("From", host);
      }
    }
    fs::path folder = path.parent_path();
    std::string where;
    if (const char* home = std::getenv("HOME"); home != nullptr && folder.string().starts_with(std::string(home) + "/")) {
      for (const auto& part : folder.lexically_relative(home)) {
        where += (where.empty() ? "" : " › ") + part.string();
      }
    } else {
      where = folder.string();
    }
    info.rows.emplace_back("Folder", where.empty() ? std::string("Home") : where);
    if (!tags.empty()) {
      std::string joined;
      for (const auto& t : tags) {
        joined += (joined.empty() ? "" : ", ") + t;
      }
      info.rows.emplace_back("Tags", joined);
    }
    return info;
  }
} // namespace

namespace {
  constexpr std::size_t kTagEntryBase = 1000; // menu entry ids of the Tags submenu rows

  // A template's menu symbol by its file kind.
  std::string templateGlyph(const fs::path& file) {
    gboolean uncertain = FALSE;
    gchar* type = g_content_type_guess(file.c_str(), nullptr, 0, &uncertain);
    const std::string mime = type != nullptr ? type : "";
    g_free(type);
    if (mime.find("spreadsheet") != std::string::npos) {
      return "table";
    }
    if (mime.find("presentation") != std::string::npos) {
      return "presentation";
    }
    if (mime.starts_with("image/")) {
      return "photo";
    }
    if (mime.starts_with("text/") || mime.find("document") != std::string::npos) {
      return "file-text";
    }
    return "file";
  }

  // Mockup §4 .mi .k: "Ctrl I", "⏎", "Del" (keys spaced, no "+").
  std::string menuShortcut(const KeyChord& chord) {
    std::string text = keyChordDisplayLabel(chord);
    for (std::size_t at = text.find(" + "); at != std::string::npos; at = text.find(" + ", at)) {
      text.replace(at, 3, " ");
    }
    if (text == "Enter" || text == "Return") {
      return "⏎";
    }
    if (const auto at = text.rfind("Delete"); at != std::string::npos && at + 6 == text.size()) {
      text.replace(at, 6, "Del");
    }
    return text;
  }
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

FilesWindow::FilesWindow(FilesApp& app, AppWindow& window, fs::path location, bool photos)
    : m_app(app), m_window(window), m_photos(photos) {
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
            if (m_browser->openInGallery(path)) {
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
      .backgroundMenu = [this](float x, float y) { showBackgroundMenu(x, y); },
      .startDrag =
          [this](const std::vector<fs::path>& dragged) {
            DragAndDrop* dnd = m_app.loop().wayland().dragAndDrop();
            if (dnd == nullptr || m_window.wlSurface() == nullptr) {
              return false;
            }
            const std::vector<fs::path> paths = dragOutOfArchive(dragged);
            std::string uris;
            std::string text;
            for (const fs::path& p : paths) {
              uris += uriFor(p) + "\r\n";
              text += (text.empty() ? "" : "\n") + p.string();
            }
            m_dragIcon.reset();
            if (m_window.surface() == nullptr) {
              return false;
            }
            m_dragImage = m_browser->buildDragImage(m_window.surface()->renderTarget().renderer());
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
      .modal = [this] { return m_window.hasSheet(); },
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
              m_renameDone[job] = [done](const std::string& err) { done(err); };
            });
          },
      .quickLook = [this](const fs::path& path) { quickLook(path, true); },
      // The Gallery is full-bleed (mockup §15): the sidebar folds away while it shows.
      .viewChanged = [this](FileBrowser::View view) { m_window.setSidebarVisible(view != FileBrowser::View::Gallery); },
      .headerButtons =
          [this](Flex& row, float s) {
            m_pillAnchor = nullptr;
            m_pillRing = nullptr;
            if (m_picker.has_value()) {
              return;
            }
            if (const auto jobs = pillJobs(); !jobs.empty()) {
              row.addChild(job_pill::pill(
                  jobs, s,
                  [this, alive = std::weak_ptr<bool>(m_alive)] {
                    DeferredCall::callLater([this, alive] {
                      if (!alive.expired()) {
                        openJobsPopover();
                      }
                    });
                  },
                  &m_pillAnchor, &m_pillRing));
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
            if (m_panes[1] != nullptr) {
              row.addChild(ui::button({
                  .glyph = std::string("arrows-diff"),
                  .glyphSize = Tokens::sizeSymbol * s,
                  .variant = ButtonVariant::Ghost,
                  .customPalette = Button::ButtonPalette{.normal = m_compareOn ? on : off, .hover = on,
                                                         .pressed = pressed, .disabled = off},
                  .tooltip = std::string("Compare Folders"),
                  .radius = Tokens::sizeControlM * s / 2.0F,
                  .width = Tokens::sizeControlM * s,
                  .height = Tokens::sizeControlM * s,
                  .onClick = [this] { toggleCompare(); },
              }));
              if (m_compareOn) {
                // Mockup §16: "Sync Left → Right" on the side the copy goes to; here it follows the active pane.
                row.addChild(ui::button({
                    .text = std::string(m_activePane == 0 ? "Sync Left → Right" : "Sync Right → Left"),
                    .variant = ButtonVariant::Default,
                    .onClick = [this] { syncCompare(); },
                }));
              }
            }
            row.addChild(ui::button({
                .glyph = std::string("terminal-2"),
                .glyphSize = Tokens::sizeSymbol * s,
                .variant = ButtonVariant::Ghost,
                .customPalette = Button::ButtonPalette{.normal = m_terminalOpen ? on : off, .hover = on,
                                                       .pressed = pressed, .disabled = off},
                .tooltip = std::string("Terminal (F4)"),
                .radius = Tokens::sizeControlM * s / 2.0F,
                .width = Tokens::sizeControlM * s,
                .height = Tokens::sizeControlM * s,
                .onClick = [this] { toggleTerminal(); },
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
  m_window.setHeaderAccessoryBuilder([this] {
    auto accessory = m_browser->buildHeaderAccessory();
    m_window.setHeaderSearchMode(m_browser->searchMode());
    return accessory;
  });
  m_window.setContentBuilder([this](Renderer& renderer, float w, float h) { return buildContent(renderer, w, h); });
  m_window.setLayoutCallback([this] {
    if (std::exchange(m_resizeJobs, false)) {
      // Out of the rebuild; the new pill has its place and width by now.
      DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
        const auto jobs = pillJobs();
        if (!alive.expired() && m_pillAnchor != nullptr && m_window.popoverOpen() && !jobs.empty()) {
          m_window.resizePopover(m_pillAnchor, job_pill::kPopoverWidth, job_pill::popoverHeight(jobs.size()));
        }
      });
    }
  });
  m_window.setTitleClick([this] { openPathField(""); });
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
    // Alt+Enter = Properties (Windows, Dolphin), besides Ctrl+I (Finder).
    if (event.pressed && !event.repeat && (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) &&
        (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Shift)) == KeyMod::Alt) {
      return m_app.commands().isEnabled("file.info") && m_app.commands().run("file.info");
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
      .enter =
          [this](const std::vector<std::string>& mimes) {
            m_dragHasUris = std::ranges::contains(mimes, std::string("text/uri-list"));
            return true;
          },
      .motion =
          [this](float x, float y) -> std::uint32_t {
            if (m_picker.has_value()) {
              return DragAndDrop::None;
            }
            const auto target = dragOver(x, y);
            if (!target.has_value() || (!target->place.empty() && !m_dragHasUris)) {
              return DragAndDrop::None; // a phone takes files only
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
            } else if (target.has_value() && !target->trash && target->place.empty()) {
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
  if (!target.place.empty()) {
    return DropOp::Copy; // a phone gets a copy
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

std::vector<fs::path> FilesWindow::dragOutOfArchive(const std::vector<fs::path>& dragged) {
  if (m_archive == nullptr || !ArchiveListing::isArchiveId(m_browser->listingId())) {
    return dragged;
  }
  std::vector<fs::path> inner;
  for (const fs::path& p : dragged) {
    if (auto in = m_archive->innerOf(p)) {
      inner.push_back(*in);
    }
  }
  if (inner.empty() || m_app.fileOps() == nullptr) {
    return dragged;
  }
  // Ark's drag-out: the items extract into a cache folder at drag start and the drag offers those files. A drop in
  // this window waits for the job; another app gets the paths at once (small items are there before the drop).
  std::error_code ec;
  if (m_dragExtract.has_value() && m_dragExtract->done) {
    fs::remove_all(m_dragExtract->dir, ec);
  }
  const char* cache = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");
  const fs::path base = cache != nullptr && *cache != '\0' ? fs::path(cache) : fs::path(home != nullptr ? home : "/tmp") / ".cache";
  const fs::path dir = base / "andromeda" / "files" / "drag-out" /
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  fs::create_directories(dir, ec);
  m_dragExtract = DragExtract{.dir = dir, .done = false, .waiting = {}};
  const fs::path archive = m_archive->archive();
  kLog.debug("drag out {} items from {} via {}", inner.size(), archive.filename().string(), dir.string());
  startJob(
      "Extracting", inner.size(),
      [&](FileOpsClient& ops, auto done) { ops.extract(archive, inner, dir, m_archivePassword, done); },
      [this, dir](const std::string& job) {
        m_renameDone[job] = [this, dir](const std::string&) {
          if (m_dragExtract.has_value() && m_dragExtract->dir == dir) {
            m_dragExtract->done = true;
            for (auto& then : std::exchange(m_dragExtract->waiting, {})) {
              then();
            }
          }
        };
      });
  std::vector<fs::path> out;
  for (const fs::path& in : inner) {
    out.push_back(dir / in.filename());
  }
  return out;
}

void FilesWindow::drop(const FileBrowser::DropTarget& target, const std::string& uriList, std::uint32_t action) {
  const std::vector<fs::path> sources = parseUriList(uriList);
  if (sources.empty()) {
    return;
  }
  if (m_dragExtract.has_value() && !m_dragExtract->done && std::ranges::any_of(sources, [&](const fs::path& s) {
        return s.parent_path() == m_dragExtract->dir;
      })) {
    kLog.debug("drop waits for the drag-out extract");
    m_dragExtract->waiting.push_back([this, target, uriList, action] { drop(target, uriList, action); });
    return;
  }
  if (target.place.starts_with("phone:")) {
    kLog.debug("drop {} items onto {}", sources.size(), target.place);
    sendToPhone(target.place.substr(6), sources);
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
  std::vector<fs::path> sources;
  for (const auto& row : rows) {
    sources.push_back(m_browser->pathOf(*row));
    items.push_back({.name = row->displayName,
                     .modified = std::chrono::system_clock::time_point(
                         std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(row->mtimeNs))),
                     .taken = row->contentType == "image/jpeg" || row->contentType == "image/tiff"
                                  ? readDateTaken(sources.back())
                                  : std::nullopt});
  }
  std::set<std::string> existing;
  for (const auto& row : m_browser->rows()) {
    existing.insert(row->displayName);
  }
  m_batchRename = std::make_unique<BatchRenameSheet>(
      std::move(sources), std::move(items), std::move(existing),
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
  const bool written = out != nullptr && std::fwrite(data.data(), 1, data.size(), out) == data.size();
  const bool closed = out != nullptr && std::fclose(out) == 0;
  if (!written || !closed) {
    if (out != nullptr) {
      fs::remove(folder / name, ec); // ours (wbx made it): a short write leaves no half file behind
    }
    showToast(std::format("Can’t save the dropped {}", image ? "image" : "text"), false);
    return;
  }
  kLog.debug("dropped data {}", (folder / name).string());
  if (folder == location()) {
    m_browser->renameWhenListed(name);
  }
}

void FilesWindow::newFolder() {
  if (!inFolder()) {
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

std::vector<fs::path> FilesWindow::templates() {
  // XDG_TEMPLATES_DIR (~/Templates): each regular file is a "New Document" kind (GNOME, KDE, Windows ShellNew).
  std::vector<fs::path> out;
  const char* dir = g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES);
  std::error_code ec;
  if (dir == nullptr || fs::path(dir) == fs::path(g_get_home_dir())) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && !entry.path().filename().string().starts_with('.')) {
      out.push_back(entry.path());
    }
  }
  std::ranges::sort(out);
  return out;
}

void FilesWindow::newFromTemplate(const fs::path& file) {
  if (!inFolder()) {
    return;
  }
  // "Report.odt" -> "Report.odt", "Report 2.odt", … ; a small file, copied here so the name is known for the rename.
  const std::string stem = file.stem().string();
  const std::string ext = file.extension().string();
  std::string name = file.filename().string();
  std::error_code ec;
  for (int n = 2; fs::exists(location() / name, ec); ++n) {
    name = std::format("{} {}{}", stem, n, ext);
  }
  if (!fs::copy_file(file, location() / name, ec)) {
    showToast(std::format("Could not create “{}”: {}", name, ec.message()), false);
    return;
  }
  kLog.debug("new from template {}", name);
  m_browser->renameWhenListed(name);
}

std::unique_ptr<Node> FilesWindow::buildContent(Renderer& renderer, float width, float height) {
  m_pickerPrimary = nullptr; // the old footer goes with this rebuild
  if (m_retiredPane != nullptr) { // the old scene, with the closed pane's nodes, is replaced when this returns
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        m_retiredPane.reset();
      }
    });
  }
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
                                                                          .share = [this](float x, float y) {
                                                                            showShareMenu(x, y);
                                                                          },
                                                                          .changed = [this] { m_window.invalidate(); },
                                                                      });
    }
    auto split = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    auto browser = buildPanes(s);
    browser->setFlexGrow(1.0F);
    split->addChild(std::move(browser));
    // The pane slides in and out from the right edge (toggleDetails) inside a clip of its full width.
    const float paneWidth = DetailsPane::kPaneWidth * s;
    auto clip = slideClip(FlexDirection::Horizontal, paneWidth, 0.0F);
    clip->addChild(m_details->build(s));
    clip->setSlide(paneWidth * (1.0F - m_detailsMix), 0.0F);
    m_detailsClip = clip.get();
    split->addChild(std::move(clip));
    m_details->update(location(), selectedPaths());
    content->addChild(std::move(split));
  } else {
    m_detailsClip = nullptr;
    content->addChild(buildPanes(s));
  }
  if ((m_terminalOpen || m_terminalAnim != 0) && m_terminal != nullptr && !m_picker.has_value()) {
    const float paneHeight = TerminalPane::kHeight * s;
    auto clip = slideClip(FlexDirection::Vertical, 0.0F, paneHeight);
    clip->addChild(m_terminal->build(renderer, s, width));
    clip->setSlide(0.0F, paneHeight * (1.0F - m_terminalMix));
    m_terminalClip = clip.get();
    content->addChild(std::move(clip));
  } else {
    m_terminalClip = nullptr;
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
bool FilesWindow::inFolder() const { return m_browser->listingId().empty() && !location().empty(); }
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
  rememberPlace();
  if (m_terminalOpen && m_terminal != nullptr && m_browser->listingId().empty()) {
    m_terminal->followFolder(location());
  }
  // A pane's location change clears its marks; selection changes keep them.
  if (m_compareOn && (m_panes[1] == nullptr || m_comparedDirs != std::pair{m_panes[0]->location(), m_panes[1]->location()})) {
    refreshCompare();
  }
  if (m_details != nullptr && m_detailsOpen) {
    m_details->update(location(), selectedPaths());
    m_window.invalidate();
  }
}

void FilesWindow::rememberPlace() {
  // A plain launch reopens the last place (folder, This PC, Recents or Starred); other listings fall back to home.
  const std::string& id = m_browser->listingId();
  std::string place = id.empty() ? location().string() : id;
  if (m_photos || m_picker.has_value() || (!id.empty() && id != "recents" && id != "starred" && id != "this-pc")
      || place.empty()) {
    return;
  }
  if (m_app.state().get<std::string>("/files/last_place", "") != place) {
    m_app.state().set("/files/last_place", std::move(place));
  }
  // Open tabs come back too (Finder, Dolphin): their folders and the current one.
  nlohmann::json tabs = nlohmann::json::array();
  if (m_browser->tabCount() > 1) {
    for (const fs::path& folder : m_browser->tabFolders()) {
      tabs.push_back(folder.string());
    }
  }
  const nlohmann::json saved = {{"folders", tabs}, {"current", m_browser->currentTab()}};
  if (m_app.state().get<nlohmann::json>("/files/tabs", nlohmann::json{}) != saved) {
    m_app.state().set("/files/tabs", saved);
  }
}

void FilesWindow::restoreTabs(const nlohmann::json& saved) {
  if (!saved.is_object() || !saved.contains("folders") || !saved["folders"].is_array() || saved["folders"].size() < 2) {
    return;
  }
  std::vector<fs::path> folders;
  for (const auto& f : saved["folders"]) {
    folders.emplace_back(f.is_string() ? f.get<std::string>() : std::string());
  }
  m_browser->restoreTabs(folders, saved.value("current", std::size_t{0}));
}

void FilesWindow::reopenPlace(const std::string& place) {
  if (place == "recents") {
    goRecents();
  } else if (place == "starred") {
    goStarred();
  } else if (place == "this-pc") {
    goThisPc();
  }
}

void FilesWindow::toggleDetails() {
  const bool open = m_detailsAnim != 0 ? m_detailsTarget == 0.0F : !m_detailsOpen;
  kLog.debug("details pane {}", open ? "on" : "off");
  m_detailsTarget = open ? 1.0F : 0.0F;
  if (open && !m_detailsOpen) {
    m_detailsOpen = true;
    m_detailsMix = 0.0F;
    m_window.requestRebuild();
  }
  m_detailsAnim = m_window.animations().drive(
      m_detailsAnim, m_detailsMix, m_detailsTarget, Style::spring(Tokens::springMove),
      [this](float v) {
        m_detailsMix = std::clamp(v, 0.0F, 1.0F);
        if (m_detailsClip != nullptr) {
          m_detailsClip->setSlide(DetailsPane::kPaneWidth * m_window.uiScale() * (1.0F - m_detailsMix), 0.0F);
        }
        m_window.invalidate();
      },
      [this] {
        m_detailsAnim = 0;
        if (m_detailsTarget == 0.0F) {
          m_detailsOpen = false; // out of the layout once closed
          m_detailsMix = 1.0F;
          m_window.requestRebuild();
        }
      },
      nullptr);
  m_window.invalidate();
}

void FilesWindow::searchAnswered(const std::string& text, const std::vector<std::shared_ptr<const FileInfo>>& rows) {
  for (const auto& pane : m_panes) {
    if (pane != nullptr) {
      pane->setSearchResults(text, rows);
    }
  }
}

void FilesWindow::makePane(std::size_t index) {
  FileBrowser::Host host = m_host;
  // A press in the other pane makes it active before the press acts (menu, drag, open use the active pane). The
  // pane finds its slot by address: closing the left pane moves the right one to slot 0.
  auto self = std::make_shared<FileBrowser*>(nullptr);
  if (!m_picker.has_value() && IndexSearch::available()) {
    host.search = [this](const std::string& text, std::string_view kind) {
      m_app.index().query(text, kind, [app = &m_app](const std::string& t, std::vector<std::shared_ptr<const FileInfo>> rows) {
        app->searchAnswered(t, std::move(rows));
      });
    };
  }
  host.viewFor = [](const fs::path& folder) -> std::optional<FileBrowserViewState> {
    return view_store::forFolder(folder);
  };
  host.viewSaved = [this](const fs::path& folder, const FileBrowserViewState& view) {
    if (m_photos) {
      return; // the Gallery of Photos is not the folder's view in Files
    }
    view_store::write(folder, view_store::encode(view));
    kLog.debug("view saved {} {}", folder.string(), view_store::encode(view));
  };
  host.viewOptions = [this](const Node* anchor) { openViewOptions(anchor); };
  host.tagOf = [this](const fs::path& path) -> std::optional<std::pair<int, std::string>> {
    // Groups of tags sort by name (rank 0 for all); the walk covers home only.
    const auto& tags = m_app.tags().tagsOf(path);
    return tags.empty() ? std::nullopt : std::optional(std::pair{0, tags.front()});
  };
  host.starred = [this](const fs::path& path) { return m_app.starred().contains(path); };
  host.phoneName = [this] {
    const auto device = galleryPhone();
    return device ? device->name : std::string();
  };
  host.galleryAction = [this](std::string_view action, float x, float y) { galleryAction(action, x, y); };
  host.galleryPlayVideo = [this](const fs::path& video, bool sound, std::array<float, 4> rect) {
    if (m_galleryVideo == nullptr) {
      m_galleryVideo = std::make_unique<GalleryVideo>(
          m_app.loop(), m_app.loop().wayland(),
          GalleryVideo::Callbacks{
              .parent = [this] { return m_window.wlSurface(); },
              .redraw = [this] { m_window.invalidate(); },
              .progress = [this](bool playing, float progress, const std::string& time) {
                m_browser->galleryVideoProgress(playing, progress, time);
              },
              .ended = [this] { m_browser->galleryVideoEnded(); },
          });
    }
    m_galleryVideo->play(video, sound, rect);
  };
  host.galleryPlaceVideo = [this](std::array<float, 4> rect) {
    if (m_galleryVideo != nullptr) {
      m_galleryVideo->place(rect);
    }
  };
  host.galleryToggleVideo = [this] {
    if (m_galleryVideo != nullptr) {
      m_galleryVideo->toggle();
    }
  };
  host.galleryStopVideo = [this] {
    if (m_galleryVideo != nullptr) {
      m_galleryVideo->stop();
    }
  };
  host.galleryMotionVideo = [](const fs::path& photo, std::function<void(fs::path)> done) {
    std::thread([photo, done = std::move(done)]() mutable {
      fs::path video = cutMotionVideo(photo);
      DeferredCall::callLater([done = std::move(done), video = std::move(video)]() mutable { done(std::move(video)); });
    }).detach();
  };
  host.galleryInfo = [this](const fs::path& path, int mapW, int mapH, std::function<void(FileGallery::PhotoInfo)> done) {
    std::thread([path, mapW, mapH, scale = m_window.uiScale(), tags = m_app.tags().tagsOf(path),
                 done = std::move(done)]() mutable {
      auto info = readPhotoInfo(path, mapW, mapH, scale, tags);
      DeferredCall::callLater([done = std::move(done), info = std::move(info)]() mutable { done(std::move(info)); });
    }).detach();
  };
  host.contextMenu = [this](std::vector<ContextMenuControlEntry> entries, float x, float y,
                            std::function<void(std::int32_t)> activated) {
    m_window.openContextMenu(std::move(entries), x, y, std::move(activated));
  };
  host.searchMode = [this, self](bool on) {
    if (*self == m_browser) {
      m_window.setHeaderSearchMode(on);
    }
  };
  host.pressed = [this, self] {
    if (m_panes[1] != nullptr && *self != m_browser) {
      setActivePane(*self == m_panes[0].get() ? 0 : 1);
    }
  };
  m_panes[index] = std::make_unique<FileBrowser>(m_app.watcher(), m_window.animations(), std::move(host),
                                                 FileBrowser::Options{.scale = m_window.uiScale(), .thumbnails = &m_app.thumbnails()});
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
    // The new pane fades in: a width spring would reflow its grid on every frame.
    m_splitMix = 0.0F;
    m_splitAnim = m_window.animations().drive(
        m_splitAnim, 0.0F, 1.0F, Style::spring(Tokens::springFade),
        [this](float v) {
          m_splitMix = v;
          if (m_splitNode != nullptr) {
            m_splitNode->setOpacity(v);
          }
          m_window.invalidate();
        },
        [this] { m_splitAnim = 0; }, nullptr);
    setActivePane(1);
    return;
  }
  // F3 again closes the inactive pane (spec §13): the active one stays, in slot 0.
  m_browser->releaseChrome();
  if (m_activePane == 1) {
    std::swap(m_panes[0], m_panes[1]);
  }
  // The scene holds the closed pane's nodes until the rebuild; buildContent frees the pane once that scene is gone,
  // so its listeners (folder peeks, thumbnails) never reach the freed nodes.
  m_retiredPane = std::move(m_panes[1]);
  m_activePane = 0;
  m_browser = m_panes[0].get();
  m_browser->setStatusSuffix({});
  m_browser->setCrumbAccent(false);
  kLog.debug("split view off");
  m_compareOn = false;
  m_panes[0]->setRowMarks({});
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
  FileBrowser& other = *m_panes[m_activePane == 0 ? 1 : 0];
  std::string suffix = std::format("{} pane: {}", m_activePane == 0 ? "Right" : "Left", other.title());
  if (const auto free = other.freeSpace()) {
    suffix += std::format(" · {} free", formatFileSize(*free));
  }
  m_browser->setStatusSuffix(std::move(suffix));
  m_panes[m_activePane == 0 ? 1 : 0]->setStatusSuffix({});
}

std::unique_ptr<Node> FilesWindow::buildPanes(float scale) {
  if (m_panes[1] == nullptr) {
    m_splitNode = nullptr;
    return m_browser->buildContent();
  }
  // Spec §13: two panes, flex 1 each, a 0.5 px hairline between; only the active pane has the path bar.
  // The active pane's status bar (its last two children: rule, bar) spans both panes (mockup §2 .pbar).
  auto row = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  std::vector<std::unique_ptr<Node>> bar;
  for (std::size_t i = 0; i < m_panes.size(); ++i) {
    if (i > 0) {
      row->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator), .width = 0.5F * scale}));
    }
    auto pane = m_panes[i]->buildContent(i == m_activePane, true);
    pane->setFlexGrow(1.0F);
    if (i == 1) {
      m_splitNode = pane.get();
      pane->setOpacity(m_splitMix);
    }
    if (i == m_activePane && pane->children().size() >= 2) {
      Node* rule = pane->children()[pane->children().size() - 2].get();
      Node* status = pane->children().back().get();
      // The crumbs above each pane show the path, so the bar shows only the status (mockup .pbar).
      if (!status->children().empty()) {
        status->children().front()->setVisible(false);
        status->children().front()->setParticipatesInLayout(false);
      }
      bar.push_back(pane->removeChild(rule));
      bar.push_back(pane->removeChild(status));
    }
    row->addChild(std::move(pane));
  }
  auto column = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  column->addChild(std::move(row));
  for (auto& node : bar) {
    column->addChild(std::move(node));
  }
  return column;
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
  // Folders the user added to the sidebar (Add to Sidebar), after Recents and Starred; gone ones stay hidden.
  for (const fs::path& pin : m_app.pins().paths()) {
    std::error_code ec;
    if (fs::is_directory(pin, ec)) {
      places.push_back({.label = pin.filename().string(), .glyph = "folder", .path = pin, .id = "pin:" + pin.string(),
                        .art = "folder", .group = FileBrowser::Place::Group::Favourites});
    }
  }
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
  // Vaults (mockup §17): a locked one opens the Unlock sheet; an unlocked one opens its folder and locks by eject.
  for (const Vaults::Vault& vault : m_app.vaults().list()) {
    const bool open = Vaults::unlocked(vault);
    places.push_back({.label = vault.name, .glyph = open ? "lock-open" : "lock",
                      .path = open ? vault.mount : fs::path(), .caption = {},
                      .onClick = open ? std::function<void()>() : [this, name = vault.name] { askUnlockVault(name); },
                      .id = "vault:" + vault.name, .art = "vault", .eject = open,
                      .onEject = [this, name = vault.name] { lockVault(name); }});
  }
  // Drives not mounted yet, and phones and cameras (no /dev mount): a click mounts and opens them.
  if (Drives* drives = m_app.drives()) {
    for (const Drives::Drive& d : drives->list()) {
      kLog.debug("sidebar drive:{}{}", d.id, d.encrypted ? " locked" : "");
      const char* glyph = d.kind == Drives::Kind::Phone    ? "device-mobile"
                          : d.kind == Drives::Kind::Camera ? "camera"
                          : d.encrypted                    ? "lock"
                                                           : "usb";
      places.push_back({.label = d.name, .glyph = glyph, .path = d.mounted ? d.root : fs::path(),
                        .caption = d.encrypted ? std::string("Locked") : std::string(),
                        .onClick = d.mounted ? std::function<void()>() : [this, id = d.id] { mountDrive(id); },
                        .id = "drive:" + d.id,
                        .art = d.kind == Drives::Kind::Usb ? "usb" : d.kind == Drives::Kind::Drive ? "drive" : "",
                        .eject = d.mounted, .onEject = [this, root = d.root] { eject(root); }});
    }
  }
  // Paired Link phones (FILES-UI-SPEC §20): battery as the caption, "away" while not connected.
  if (LinkShare* link = m_app.linkShare(); link != nullptr && !isPicker()) {
    for (const LinkShare::Device& d : link->devices()) {
      kLog.debug("sidebar phone:{} {}", d.id, d.connected ? std::to_string(d.battery) : std::string("away"));
      places.push_back({.label = d.name, .glyph = "device-mobile",
                        .caption = !d.connected ? std::string("away")
                                   : d.battery >= 0 ? std::format("{} %", d.battery)
                                                    : std::string(),
                        .onClick = [this, id = d.id] { showPhone(id); }, .id = "phone:" + d.id, .art = "phone",
                        .dropFiles = d.connected});
    }
  }
  // Network closes the Locations group, after the drives (mockup §1).
  places.push_back({.label = "Network", .glyph = "network", .onClick = [this] { goNetwork(); }, .id = "network",
                    .art = "network"});
  std::string smartNames;
  for (const SmartFolder& smart : smart_folders::load()) {
    smartNames += (smartNames.empty() ? std::string() : std::string(",")) + smart.name;
    places.push_back({.label = smart.name, .glyph = "folder-search", .onClick = [this, name = smart.name] { goSmart(name); },
                      .id = "smart:" + smart.name, .group = FileBrowser::Place::Group::Favourites});
  }
  kLog.debug("sidebar smart [{}]", smartNames);
  std::string tagNames;
  for (const SidebarTag& tag : m_app.tags().sidebar()) {
    tagNames += (tagNames.empty() ? std::string() : std::string(",")) + tag.name;
    places.push_back({.label = tag.name, .glyph = "tag", .onClick = [this, name = tag.name] { goTag(name); },
                      .id = "tag:" + tag.name, .group = FileBrowser::Place::Group::Tags, .dot = rgbHex(tag.rgb)});
  }
  kLog.debug("sidebar tags [{}]", tagNames);
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
  for (const auto& pane : m_panes) {
    if (pane != nullptr && pane->groupBy() == FileGroupBy::Tags) {
      pane->regroup();
    }
  }
  if (!m_tagShown.empty() && m_browser->listingId() == "tag:" + m_tagShown) {
    m_browser->showListing(m_app.tags().listing(m_tagShown));
  }
  if (m_tagManager != nullptr && m_window.hasSheet()) {
    m_tagManager->indexChanged();
  }
}

TagManagerSheet* FilesWindow::tagManager() const { return m_window.hasSheet() ? m_tagManager.get() : nullptr; }

void FilesWindow::openTagManager() {
  m_app.tags().refresh();
  m_tagManager = std::make_shared<TagManagerSheet>(TagManagerSheet::Host{
      .count = [this](const std::string& tag) { return m_app.tags().count(tag); },
      .unlisted = [this](const std::vector<TagDef>& config) { return m_app.tags().unlisted(config); },
      .renameOnFiles =
          [this](const std::string& old, const std::string& now) {
            m_tagOps.push_back({.tag = old, .tags = {old, now}, .mode = "rename"});
            runTagOps();
          },
      .removeFromFiles =
          [this](const std::string& tag) {
            m_tagOps.push_back({.tag = tag, .tags = {tag}, .mode = "remove"});
            runTagOps();
          },
      .configChanged = [this] { m_app.tags().configChanged(); },
      .close = [this] { m_window.dismissSheet(); },
      .rebuild = [this] { m_window.requestRebuild(); },
      .focus = [this](Input& input) { m_window.input().setFocus(input.inputArea()); },
  });
  m_window.presentSheet([sheet = m_tagManager](float scale) { return sheet->build(scale); },
                        [this] { m_tagManager->done(); });
}

void FilesWindow::runTagOps() {
  if (m_tagOpRunning || m_tagOps.empty()) {
    return;
  }
  m_tagOpRunning = true;
  TagFileOp op = std::move(m_tagOps.front());
  m_tagOps.pop_front();
  const auto next = [this] {
    m_tagOpRunning = false;
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        runTagOps();
      }
    });
  };
  m_app.tags().walkThen([this, alive = std::weak_ptr<bool>(m_alive), op = std::move(op), next] {
    if (alive.expired()) {
      return;
    }
    const auto paths = m_app.tags().paths(op.tag);
    kLog.debug("tag {} {} on {} files", op.mode, op.tag, paths.size());
    if (paths.empty() || m_app.fileOps() == nullptr) {
      next();
      return;
    }
    startJob(
        op.mode == "rename" ? "Renaming the tag on" : "Removing the tag from", paths.size(),
        [&](FileOpsClient& ops, auto done) {
          ops.setTags(paths, op.tags, op.mode, [done, next](const std::string& job, const std::string& error) {
            if (job.empty()) {
              next();
            }
            done(job, error);
          });
        },
        [this, paths, next](const std::string& job) {
          m_app.tags().addJob(job);
          m_renameDone[job] = [this, paths, next](const std::string&) {
            m_app.tags().reread(paths);
            next();
          };
        });
  });
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
  // Every pane on the drive leaves it, not only the active one.
  for (const auto& pane : m_panes) {
    const std::string at = pane != nullptr ? pane->location().string() : std::string();
    if (!at.empty() && (at == mount.string() || at.starts_with(mount.string() + "/"))) {
      pane->go(g_get_home_dir());
    }
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
  if (m_picker.has_value() || !IndexSearch::available()) {
    m_browser->showListing(std::move(listing));
    return;
  }
  // Files changed in the last 30 days that no app reported (FB3): the index's newest documents, PDFs, images, sheets
  // and slides join the list. One show once every kind answered, so the page does not change under the pointer.
  struct Pending {
    FileBrowser::Listing listing;
    int left = 0;
    fs::path from; // where the browser was: a move before the answers cancels the show
    std::string fromListing;
  };
  static constexpr std::array<std::string_view, 5> kKinds{"doc", "pdf", "image", "sheet", "slides"};
  static constexpr int kRecentDays = 30;
  auto pending = std::make_shared<Pending>(Pending{.listing = std::move(listing), .left = static_cast<int>(kKinds.size()),
                                                          .from = m_browser->location(), .fromListing = m_browser->listingId()});
  const auto cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          (std::chrono::system_clock::now() - std::chrono::days(kRecentDays)).time_since_epoch()).count();
  for (const std::string_view kind : kKinds) {
    m_app.index().query({}, kind, [this, pending, cutoff, alive = std::weak_ptr<bool>(m_alive)](
                                      const std::string&, std::vector<std::shared_ptr<const FileInfo>> rows) {
      for (auto& row : rows) {
        auto& have = pending->listing.rows;
        if (row->mtimeNs >= cutoff && have.size() < kRecentsMax &&
            std::ranges::none_of(have, [&](const auto& r) { return r->name == row->name; })) {
          have.push_back(std::move(row));
        }
      }
      if (--pending->left == 0 && !alive.expired() && m_browser->location() == pending->from &&
          m_browser->listingId() == pending->fromListing) {
        kLog.debug("recents {} rows", pending->listing.rows.size());
        m_browser->showListing(std::move(pending->listing));
      }
    });
  }
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

bool FilesWindow::selectionPinned() const {
  const auto paths = selectedPaths();
  return !paths.empty() && std::ranges::all_of(paths, [this](const fs::path& p) { return m_app.pins().contains(p); });
}

bool FilesWindow::selectionIsFolders() const {
  const auto paths = selectedPaths();
  return !paths.empty() && std::ranges::all_of(paths, [](const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
  });
}

void FilesWindow::togglePin() {
  const auto paths = selectedPaths();
  if (!selectionIsFolders() || inTrash()) {
    return;
  }
  const bool pinned = m_app.pins().toggle(paths);
  kLog.debug("{} {} folders", pinned ? "pinned" : "unpinned", paths.size());
  m_app.refreshPlaces();
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
  const auto add = [&](const char* command, std::string label = {}, std::string glyph = {}) {
    const AppCommand* c = commands.find(command);
    if (c == nullptr || (c->enabled && !c->enabled())) {
      return;
    }
    ids.emplace_back(command);
    entries.push_back({.id = static_cast<std::int32_t>(ids.size()),
                       .label = label.empty() ? c->label : std::move(label),
                       .shortcut = c->shortcut ? menuShortcut(*c->shortcut) : std::string(),
                       .glyph = std::move(glyph)});
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
    // Mockup §4 .menu: Finder order, 16 symbols on the actions that have one, the tag dots, Trash in red.
    add("file.open");
    if (const AppCommand* c = commands.find("file.open-with"); c != nullptr && (!c->enabled || c->enabled())) {
      entries.push_back(openWithEntry(paths, ids));
    }
    separator();
    add("file.info", {}, "info-circle");
    add("file.rename");
    add("file.compress", name.empty() ? std::format("Compress {} Items", paths.size()) : std::format("Compress “{}”", name),
        "file-zip");
    add("file.extract-here");
    add("edit.copy", {}, "copy");
    add("edit.cut", {}, "scissors");
    add("edit.copy-location");
    separator();
    entries.push_back(shareEntry(ids));
    add("file.star", selectionStarred() ? "Remove from Starred" : "Add to Starred", "star");
    add("file.pin", selectionPinned() ? "Remove from Sidebar" : "Add to Sidebar");
    add("file.open-terminal", {}, "terminal-2");
    // Actions ›: KDE service menus that cover the selection, X-KDE-Submenu groups as their own submenus.
    m_serviceActions = service_menus::forItems(paths);
    if (!m_serviceActions.empty()) {
      ContextMenuControlEntry actions{.label = "Actions", .hasSubmenu = true};
      for (std::size_t i = 0; i < m_serviceActions.size(); ++i) {
        const auto& action = m_serviceActions[i];
        ids.push_back(std::format("service:{}", i));
        ContextMenuControlEntry row{.id = static_cast<std::int32_t>(ids.size()), .label = action.name};
        if (action.submenu.empty()) {
          actions.children.push_back(std::move(row));
          continue;
        }
        auto group = std::ranges::find_if(actions.children, [&](const auto& e) { return e.hasSubmenu && e.label == action.submenu; });
        if (group == actions.children.end()) {
          actions.children.push_back({.label = action.submenu, .hasSubmenu = true});
          group = actions.children.end() - 1;
        }
        group->children.push_back(std::move(row));
      }
      entries.push_back(std::move(actions));
    }
    // Tags: a click on a dot adds the tag to every selected item, or removes it when all have it.
    const auto defs = loadTagConfig();
    if (!defs.empty() && m_app.fileOps() != nullptr) {
      ContextMenuControlEntry tags{.label = "Tags"};
      for (std::size_t t = 0; t < defs.size(); ++t) {
        tags.dots.push_back({.id = static_cast<std::int32_t>(kTagEntryBase + t),
                             .color = ColorSpec{.role = std::nullopt, .fixed = rgbHex(defs[t].rgb), .alpha = 1.0F}});
      }
      entries.push_back(std::move(tags));
    }
    separator();
    add("edit.trash", {}, "trash");
    entries.back().danger = !ids.empty() && ids.back() == "edit.trash";
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
                                 m_app.fileOps()->setTags(
                                     paths, {defs[t].name}, all ? "remove" : "add",
                                     [this, paths, alive](const std::string& job, const std::string&) {
                                       if (job.empty() || alive.expired()) {
                                         return;
                                       }
                                       // The sidebar counts and a shown tag listing follow once the job lands.
                                       m_jobs.insert(job);
                                       m_app.tags().addJob(job);
                                       m_renameDone[job] = [this, paths](const std::string&) {
                                         m_app.tags().reread(paths);
                                       };
                                     });
                               }
                               return;
                             }
                             if (id > 0 && static_cast<std::size_t>(id) <= ids.size()) {
                               runMenuCommand(ids[static_cast<std::size_t>(id) - 1], paths);
                             }
                           });
}

void FilesWindow::runMenuCommand(const std::string& command, const std::vector<fs::path>& paths) {
  kLog.debug("menu {}", command);
  if (command.starts_with("share.device:")) {
    shareTo(command.substr(std::string_view("share.device:").size()));
  } else if (command == "share.email") {
    shareByEmail();
  } else if (command.starts_with("open.app:")) {
    launchWith(command.substr(std::string_view("open.app:").size()), paths);
  } else if (command.starts_with("service:")) {
    const std::size_t i = std::stoul(command.substr(std::string_view("service:").size()));
    if (i < m_serviceActions.size()) {
      const bool ok = service_menus::run(m_serviceActions[i], paths, location());
      kLog.info("service action {} on {} items{}", m_serviceActions[i].name, paths.size(), ok ? "" : " failed");
    }
  } else {
    m_app.commands().run(command);
  }
}

ContextMenuControlEntry FilesWindow::shareEntry(std::vector<std::string>& ids) {
  // Share ›: paired phones (Link), then Email and Copy Location; ids are command-like strings for the menu log.
  // The row names the phones (mockup §4 "Share › Pixel 9, Link…").
  ContextMenuControlEntry share{.label = "Share", .hasSubmenu = true, .glyph = "share"};
  const auto child = [&](std::string id, std::string label, std::string glyph, bool enabled, std::string caption) {
    ids.push_back(std::move(id));
    share.children.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = std::move(label),
                              .shortcut = std::move(caption), .enabled = enabled, .glyph = std::move(glyph)});
  };
  std::string names;
  for (const LinkShare::Device& device : shareDevices()) {
    child("share.device:" + device.id, device.name, "device-mobile", device.connected,
          device.connected ? std::string() : std::string("Not connected"));
    names += (names.empty() ? "" : ", ") + device.name;
  }
  if (!share.children.empty()) {
    share.children.push_back({.separator = true});
    share.shortcut = names + ", Link…";
  }
  child("share.email", "Email…", "mail", true, {});
  if (const AppCommand* c = m_app.commands().find("edit.copy-location"); c != nullptr && (!c->enabled || c->enabled())) {
    child("edit.copy-location", c->label, {}, true, {});
  }
  return share;
}

ContextMenuControlEntry FilesWindow::openWithEntry(const std::vector<fs::path>& paths, std::vector<std::string>& ids) {
  // Open With ›: the apps for the first item's type, the default first (Finder), then the full chooser.
  constexpr std::size_t kMaxApps = 8;
  ContextMenuControlEntry entry{.label = "Open With", .hasSubmenu = true};
  const auto info = statFile(paths.front());
  if (info && !info->contentType.empty()) {
    std::string def;
    if (GAppInfo* d = g_app_info_get_default_for_type(info->contentType.c_str(), FALSE)) {
      def = g_app_info_get_id(d) != nullptr ? g_app_info_get_id(d) : "";
      g_object_unref(d);
    }
    std::vector<std::pair<std::string, std::string>> apps; // desktop id, name
    GList* list = g_app_info_get_all_for_type(info->contentType.c_str());
    for (GList* l = list; l != nullptr; l = l->next) {
      auto* app = G_APP_INFO(l->data);
      const char* id = g_app_info_get_id(app);
      const char* name = g_app_info_get_display_name(app);
      if (id != nullptr && name != nullptr && g_app_info_should_show(app) &&
          std::ranges::none_of(apps, [&](const auto& a) { return a.second == name; })) {
        apps.emplace_back(id, name);
      }
    }
    g_list_free_full(list, g_object_unref);
    std::ranges::stable_partition(apps, [&](const auto& a) { return a.first == def; });
    apps.resize(std::min(apps.size(), kMaxApps));
    for (const auto& [id, name] : apps) {
      std::string bare = id.ends_with(".desktop") ? id.substr(0, id.size() - 8) : id;
      ids.push_back("open.app:" + bare);
      entry.children.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = name,
                                .shortcut = id == def ? std::string("Default") : std::string()});
    }
    if (!entry.children.empty()) {
      entry.children.push_back({.separator = true});
    }
  }
  ids.emplace_back("file.open-with");
  entry.children.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = "Other Application…"});
  return entry;
}

void FilesWindow::showShareMenu(float sceneX, float sceneY) {
  const auto paths = selectedPaths();
  if (paths.empty()) {
    return;
  }
  std::vector<std::string> ids;
  m_window.openContextMenu(shareEntry(ids).children, sceneX, sceneY,
                           [this, ids, paths, alive = std::weak_ptr<bool>(m_alive)](std::int32_t id) {
                             if (!alive.expired() && id > 0 && static_cast<std::size_t>(id) <= ids.size()) {
                               runMenuCommand(ids[static_cast<std::size_t>(id) - 1], paths);
                             }
                           });
}

void FilesWindow::showBackgroundMenu(float sceneX, float sceneY) {
  if (m_picker.has_value()) {
    return;
  }
  kLog.debug("menu background");
  // Entry ids: 1..n commands, then the Sort By and View rows, then the folder's Get Info.
  constexpr std::int32_t kSortBase = 2000;
  constexpr std::int32_t kViewBase = 2100;
  constexpr std::int32_t kFolderInfo = 2200;
  constexpr std::int32_t kTemplateBase = 2300;
  const auto templateFiles = templates();
  CommandTable& commands = m_app.commands();
  std::vector<std::string> ids;
  std::vector<ContextMenuControlEntry> entries;
  const auto add = [&](const char* command, std::string glyph = {}) {
    const AppCommand* c = commands.find(command);
    if (c == nullptr || (c->enabled && !c->enabled())) {
      return;
    }
    ids.emplace_back(command);
    entries.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = c->label,
                       .shortcut = c->shortcut ? menuShortcut(*c->shortcut) : std::string(), .glyph = std::move(glyph)});
  };
  const bool folder = !location().empty() && m_browser->listingId().empty();
  if (folder) {
    add("file.new-folder", "folder-plus");
    // New Document ›: the ~/Templates files; none = the submenu says why in one disabled row (the long reason
    // on the row itself would set the menu width).
    ContextMenuControlEntry doc{.label = "New Document", .hasSubmenu = true, .glyph = "file-plus"};
    if (templateFiles.empty()) {
      doc.children.push_back({.label = "No templates in ~/Templates", .enabled = false});
    }
    for (std::size_t i = 0; i < templateFiles.size(); ++i) {
      doc.children.push_back({.id = kTemplateBase + static_cast<std::int32_t>(i),
                              .label = templateFiles[i].stem().string(),
                              .glyph = templateGlyph(templateFiles[i])});
    }
    entries.push_back(std::move(doc));
    add("edit.paste", "clipboard");
    entries.push_back({.separator = true});
  }
  static constexpr std::array kSorts{std::pair{FileSortKey::Name, "Name"}, std::pair{FileSortKey::Modified, "Date Modified"},
                                     std::pair{FileSortKey::Size, "Size"}, std::pair{FileSortKey::Kind, "Kind"}};
  ContextMenuControlEntry sort{.label = "Sort By", .hasSubmenu = true};
  for (std::size_t i = 0; i < kSorts.size(); ++i) {
    sort.children.push_back({.id = kSortBase + static_cast<std::int32_t>(i), .label = kSorts[i].second, .radio = true,
                             .toggleState = m_browser->sortKey() == kSorts[i].first ? 1 : 0});
  }
  entries.push_back(std::move(sort));
  static constexpr std::array kViews{"Icons", "List", "Columns", "Gallery"};
  ContextMenuControlEntry view{.label = "View", .hasSubmenu = true};
  for (std::size_t i = 0; i < kViews.size(); ++i) {
    view.children.push_back({.id = kViewBase + static_cast<std::int32_t>(i), .label = kViews[i], .radio = true,
                             .toggleState = static_cast<std::size_t>(m_browser->view()) == i ? 1 : 0});
  }
  entries.push_back(std::move(view));
  if (const AppCommand* hidden = commands.find("view.hidden")) {
    ids.emplace_back("view.hidden");
    entries.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = hidden->label,
                       .shortcut = hidden->shortcut ? menuShortcut(*hidden->shortcut) : std::string(),
                       .checkmark = true, .toggleState = m_browser->showHidden() ? 1 : 0});
  }
  add("view.options");
  if (folder) {
    entries.push_back({.separator = true});
    add("file.open-terminal", "terminal-2");
    entries.push_back({.id = kFolderInfo, .label = "Get Info", .glyph = "info-circle"});
  }
  m_window.openContextMenu(std::move(entries), sceneX, sceneY,
                           [this, ids, templateFiles, alive = std::weak_ptr<bool>(m_alive)](std::int32_t id) {
                             if (alive.expired()) {
                               return;
                             }
                             if (id >= kSortBase && id < kSortBase + static_cast<std::int32_t>(kSorts.size())) {
                               kLog.debug("menu sort {}", kSorts[static_cast<std::size_t>(id - kSortBase)].second);
                               // setSort flips the direction on the current key; the menu only picks the key.
                               if (const FileSortKey key = kSorts[static_cast<std::size_t>(id - kSortBase)].first;
                                   key != m_browser->sortKey()) {
                                 m_browser->setSort(key);
                               }
                             } else if (id >= kViewBase && id < kViewBase + static_cast<std::int32_t>(kViews.size())) {
                               kLog.debug("menu view {}", kViews[static_cast<std::size_t>(id - kViewBase)]);
                               m_browser->setView(static_cast<FileBrowser::View>(id - kViewBase));
                             } else if (id == kFolderInfo) {
                               kLog.debug("menu folder info");
                               getInfo();
                             } else if (id >= kTemplateBase &&
                                        id < kTemplateBase + static_cast<std::int32_t>(templateFiles.size())) {
                               newFromTemplate(templateFiles[static_cast<std::size_t>(id - kTemplateBase)]);
                             } else if (id > 0 && static_cast<std::size_t>(id) <= ids.size()) {
                               kLog.debug("menu {}", ids[static_cast<std::size_t>(id) - 1]);
                               m_app.commands().run(ids[static_cast<std::size_t>(id) - 1]);
                             }
                           });
}

void FilesWindow::slideTerminal(float target) {
  m_terminalAnim = m_window.animations().drive(
      m_terminalAnim, m_terminalMix, target, Style::spring(Tokens::springMove),
      [this](float v) {
        m_terminalMix = std::clamp(v, 0.0F, 1.0F);
        if (m_terminalClip != nullptr) {
          m_terminalClip->setSlide(0.0F, TerminalPane::kHeight * m_window.uiScale() * (1.0F - m_terminalMix));
        }
        m_window.invalidate();
      },
      [this, target] {
        m_terminalAnim = 0;
        if (target == 0.0F) {
          if (m_terminal != nullptr) {
            m_terminal->detach(); // a rebuild during the slide built it again
          }
          m_window.requestRebuild(); // out of the layout once closed
        }
      },
      nullptr);
  m_window.invalidate();
}

void FilesWindow::toggleTerminal() {
  if (m_terminalOpen) {
    m_terminalOpen = false;
    kLog.debug("terminal pane off");
    if (m_terminal != nullptr) {
      m_terminal->detach();
    }
    slideTerminal(0.0F);
    return;
  }
  if (m_terminal == nullptr || !m_terminal->running()) {
    const fs::path dir = m_browser->listingId().empty() ? location() : fs::path(g_get_home_dir());
    m_terminal = std::make_unique<TerminalPane>(
        dir, TerminalPane::Host{
                 .invalidate = [this] { m_window.invalidate(); },
                 .cwdChanged =
                     [this](const fs::path& cwd) {
                       if (cwd != location()) {
                         go(cwd);
                       }
                     },
                 .exited =
                     [this] {
                       m_terminalOpen = false;
                       m_terminal.reset();
                       m_window.requestRebuild();
                     },
                 .focus = [this](InputArea* area) { m_window.input().setFocus(area); },
             });
    if (!m_terminal->running()) {
      m_terminal.reset();
      showToast("The terminal could not start", false);
      return;
    }
  }
  m_terminalOpen = true;
  kLog.debug("terminal pane on");
  if (m_terminalAnim == 0) {
    m_terminalMix = 0.0F;
  }
  m_window.requestRebuild();
  slideTerminal(1.0F);
  // The pane takes the keys once it is built (Dolphin focuses its terminal on F4).
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (!alive.expired() && m_terminal != nullptr && m_terminalOpen) {
      m_terminal->focus();
    }
  });
}

void FilesWindow::toggleCompare() {
  m_compareOn = m_panes[1] != nullptr && !m_compareOn;
  kLog.debug("compare {}", m_compareOn ? "on" : "off");
  refreshCompare();
  m_window.requestRebuild();
}

void FilesWindow::refreshCompare() {
  if (m_panes[1] == nullptr) {
    m_compareOn = false;
  }
  const auto marksOf = [](const std::map<std::string, folder_compare::Mark>& marks) {
    std::map<std::string, ColorSpec> out;
    for (const auto& [name, mark] : marks) {
      switch (mark) {
      case folder_compare::Mark::Same: break;
      case folder_compare::Mark::Newer: out[name] = colorSpecFromRole(ColorRole::Accent); break;
      case folder_compare::Mark::Older: out[name] = colorSpecFromRole(ColorRole::Label3); break;
      case folder_compare::Mark::OnlyLeft: out[name] = colorSpecFromRole(ColorRole::Success); break;
      case folder_compare::Mark::OnlyRight: out[name] = colorSpecFromRole(ColorRole::Warning); break;
      }
    }
    return out;
  };
  ++m_compareRun;
  if (!m_compareOn || !m_panes[0]->listingId().empty() || !m_panes[1]->listingId().empty()) {
    m_compare = {};
    for (const auto& pane : m_panes) {
      if (pane != nullptr) {
        pane->setRowMarks({});
      }
    }
    return;
  }
  m_comparedDirs = {m_panes[0]->location(), m_panes[1]->location()};
  // Nested folders are walked to the bottom: on a worker, the marks land when it is done.
  std::thread([left = m_comparedDirs.first, right = m_comparedDirs.second, hidden = m_browser->showHidden(),
               run = m_compareRun, alive = std::weak_ptr<bool>(m_alive), this, marksOf] {
    auto r = folder_compare::compare(left, right, hidden);
    DeferredCall::callLater([this, alive, run, marksOf, r = std::move(r)]() mutable {
      if (alive.expired() || run != m_compareRun || m_panes[1] == nullptr) {
        return;
      }
      kLog.debug("compare same {} differ {} only-left {} only-right {}{}", r.same, r.differ, r.onlyLeft, r.onlyRight,
                 r.truncated ? " (truncated)" : "");
      m_panes[0]->setRowMarks(marksOf(r.left));
      m_panes[1]->setRowMarks(marksOf(r.right));
      m_compare = std::move(r);
      m_window.invalidate();
    });
  }).detach();
}

void FilesWindow::syncCompare() {
  if (!m_compareOn || m_panes[1] == nullptr) {
    return;
  }
  // The lists hold paths relative to the folders that were compared; a pane that moved on since must not count.
  if (m_comparedDirs != std::pair{m_panes[0]->location(), m_panes[1]->location()}) {
    refreshCompare();
    return;
  }
  const std::size_t from = m_activePane;
  const fs::path source = from == 0 ? m_comparedDirs.first : m_comparedDirs.second;
  const fs::path destination = from == 0 ? m_comparedDirs.second : m_comparedDirs.first;
  // One copy job per destination folder: the daemon copies many sources into one folder.
  std::map<fs::path, std::vector<fs::path>> byFolder;
  for (const fs::path& rel : from == 0 ? m_compare.sendLeft : m_compare.sendRight) {
    byFolder[rel.parent_path()].push_back(source / rel);
  }
  kLog.debug("sync {} items in {} folders {} -> {}", (from == 0 ? m_compare.sendLeft : m_compare.sendRight).size(),
             byFolder.size(), source.string(), destination.string());
  if (byFolder.empty()) {
    showToast("The folders are already in sync", false);
    return;
  }
  // One job at a time: the window shows one conflict sheet, so a second job's question would wait unseen.
  for (auto& [folder, sources] : byFolder) {
    m_syncQueue.emplace_back(destination / folder, std::move(sources));
  }
  runSyncQueue();
}

void FilesWindow::runSyncQueue() {
  if (m_syncQueue.empty()) {
    refreshCompare();
    return;
  }
  auto [into, sources] = std::move(m_syncQueue.front());
  m_syncQueue.pop_front();
  const auto next = [this, alive = std::weak_ptr<bool>(m_alive)] {
    DeferredCall::callLater([this, alive] {
      if (!alive.expired()) {
        runSyncQueue();
      }
    });
  };
  startJob(
      "Syncing", sources.size(),
      [&](FileOpsClient& ops, auto done) {
        ops.copy(sources, into, [this, done, next](const std::string& job, const std::string& error) {
          if (job.empty()) {
            m_syncQueue.clear(); // the daemon refused: the rest would fail the same way
            next();
          }
          done(job, error);
        });
      },
      [this, next](const std::string& job) { m_renameDone[job] = [next](const std::string&) { next(); }; });
}

CompressSheet* FilesWindow::compressSheet() const { return m_window.hasSheet() ? m_compress.get() : nullptr; }

ExtractSheet* FilesWindow::extractSheet() const { return m_window.hasSheet() ? m_extract.get() : nullptr; }

BatchRenameSheet* FilesWindow::batchRenameSheet() const { return m_window.hasSheet() ? m_batchRename.get() : nullptr; }

SmartFolderSheet* FilesWindow::smartSheet() const { return m_window.hasSheet() ? m_smartSheet.get() : nullptr; }

bool FilesWindow::inSmartFolder() const { return m_browser->listingId().starts_with("smart:"); }

bool FilesWindow::canSaveSearch() const {
  const auto state = m_browser->searchState();
  return state.has_value() && (!state->text.empty() || state->kind != FileBrowser::SearchKind::All);
}

void FilesWindow::presentSmartSheet(SmartFolder folder, std::string original) {
  const fs::path here = m_browser->listingId().empty() ? location() : fs::path();
  m_smartSheet = std::make_shared<SmartFolderSheet>(
      std::move(folder), here, std::move(original),
      [this](const SmartFolder& saved, const std::string& was) {
        auto all = smart_folders::load();
        const auto it = std::ranges::find(all, was.empty() ? saved.name : was, &SmartFolder::name);
        if (it != all.end()) {
          *it = saved;
        } else {
          all.push_back(saved);
        }
        smart_folders::save(all);
        kLog.info("smart folder saved {}: {}", saved.name, saved.sentence());
        m_app.tags().configChanged(); // every window's sidebar
        goSmart(saved.name);
      },
      [this] { m_window.dismissSheet(); }, [this] { m_window.requestRebuild(); });
  m_window.presentSheet([sheet = m_smartSheet](float scale) { return sheet->build(scale); },
                        [this] { m_window.dismissSheet(); });
}

void FilesWindow::newSmartFolder(bool fromSearch) {
  SmartFolder folder{.name = "", .text = "", .kind = "", .days = 0, .folder = {}};
  if (const auto state = m_browser->searchState(); fromSearch && state.has_value()) {
    using Kind = FileBrowser::SearchKind;
    folder.text = state->text;
    folder.kind = state->kind == Kind::Pdf     ? "pdf"
                  : state->kind == Kind::Image ? "image"
                  : state->kind == Kind::Doc   ? "doc"
                  : state->kind == Kind::Sheet ? "sheet"
                                               : "";
    folder.days = state->kind == Kind::Recent ? 30 : 0;
    if (state->scope == FileBrowser::SearchScope::Folder && m_browser->listingId().empty()) {
      folder.folder = location();
    }
    folder.name = !folder.text.empty() ? folder.text : "Saved Search";
  }
  presentSmartSheet(std::move(folder), {});
}

void FilesWindow::editSmartFolder() {
  const auto all = smart_folders::load();
  const auto it = std::ranges::find(all, m_smartShown, &SmartFolder::name);
  if (inSmartFolder() && it != all.end()) {
    presentSmartSheet(*it, it->name);
  }
}

void FilesWindow::removeSmartFolder() {
  if (!inSmartFolder()) {
    return;
  }
  auto all = smart_folders::load();
  std::erase_if(all, [&](const SmartFolder& f) { return f.name == m_smartShown; });
  smart_folders::save(all);
  kLog.info("smart folder removed {}", m_smartShown);
  m_smartShown.clear();
  m_app.tags().configChanged();
  go(fs::path(g_get_home_dir()));
}

void FilesWindow::showPhotos() {
  const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
  const SmartFolder photos{.name = "Photos", .text = {}, .kind = "image", .days = 0,
                           .folder = pictures != nullptr ? fs::path(pictures) : fs::path(g_get_home_dir()) / "Pictures"};
  const std::string id = "photos:";
  m_browser->showListing({.id = id, .title = photos.name, .rows = {}, .activatable = true});
  m_browser->setView(FileBrowser::View::Gallery);
  smart_folders::run(photos, &m_app.index(), m_alive, [this, id](smart_folders::Rows rows) {
    kLog.info("photos: {} images", rows.size());
    if (m_browser->listingId() == id) {
      m_browser->showListing({.id = id, .title = "Photos", .rows = std::move(rows), .activatable = true});
    }
  });
}

void FilesWindow::goSmart(const std::string& name) {
  const auto all = smart_folders::load();
  const auto it = std::ranges::find(all, name, &SmartFolder::name);
  if (it == all.end()) {
    return;
  }
  m_smartShown = name;
  const std::string id = "smart:" + name;
  // The title shows at once; the rows land when the search answers (a walk can take a moment).
  if (m_browser->listingId() != id) {
    m_browser->showListing({.id = id, .title = name, .rows = {}, .activatable = true});
  }
  smart_folders::run(*it, &m_app.index(), m_alive, [this, id, name](smart_folders::Rows rows) {
    kLog.info("smart folder {}: {} results", name, rows.size());
    if (m_browser->listingId() == id) {
      m_browser->showListing({.id = id, .title = name, .rows = std::move(rows), .activatable = true});
    }
  });
}

SelectPatternSheet* FilesWindow::patternSheet() const { return m_window.hasSheet() ? m_patternSheet.get() : nullptr; }

void FilesWindow::selectByPattern() {
  std::vector<std::string> names;
  for (const auto& row : m_browser->rows()) {
    names.push_back(row->name);
  }
  m_patternSheet = std::make_shared<SelectPatternSheet>(
      std::move(names), m_lastPattern,
      m_lastPatternRegex ? SelectPatternSheet::Mode::Regex : SelectPatternSheet::Mode::Wildcard,
      [this](const std::vector<std::string>& matches, bool select) {
        m_lastPattern = m_patternSheet->pattern();
        m_lastPatternRegex = m_patternSheet->mode() == SelectPatternSheet::Mode::Regex;
        std::vector<std::string> now;
        for (const auto& row : m_browser->selectedRows()) {
          now.push_back(row->name);
        }
        if (select) {
          for (const std::string& m : matches) {
            if (!std::ranges::contains(now, m)) {
              now.push_back(m);
            }
          }
        } else {
          std::erase_if(now, [&](const std::string& n) { return std::ranges::contains(matches, n); });
        }
        kLog.debug("pattern {} {} matches", select ? "select" : "deselect", matches.size());
        m_browser->selectNames(now);
      },
      [this] { m_window.dismissSheet(); }, [this] { m_window.requestRebuild(); },
      [this](Input& input) { m_window.input().setFocus(input.inputArea()); });
  kLog.debug("pattern sheet open");
  m_window.presentSheet([sheet = m_patternSheet](float scale) { return sheet->build(scale); },
                        [this] { m_window.dismissSheet(); });
}

bool FilesWindow::terminalFocused() const { return m_terminalOpen && m_terminal != nullptr && m_terminal->focused(); }

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
  kLog.debug("extract sheet {}", state == ExtractSheet::State::Normal ? "normal" : "password");
  for (const auto& [job, pending] : m_extracts) {
    if (pending.archive == archive) {
      m_extract->setDestination(pending.request.destination);
    }
  }
  m_window.presentSheet([this](float scale) { return m_extract->build(scale); }, [this] { m_window.dismissSheet(); });
}

void FilesWindow::startExtract(const fs::path& archive, ExtractSheet::Request request) {
  if (!request.password.empty()) {
    m_archivePassword = request.password;
  }
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

void FilesWindow::getInfo(std::vector<fs::path> paths) {
  if (paths.empty()) {
    paths = selectedPaths();
  }
  // No selection: the folder itself (Finder; the background menu).
  if (paths.empty() && !location().empty() && m_browser->listingId().empty()) {
    paths.push_back(location());
  }
  if (paths.empty()) {
    return;
  }
  kLog.debug("get info {} items", paths.size());
  m_info = std::make_unique<GetInfoSheet>(
      paths, m_app.fileOps(), [this] { m_window.dismissSheet(); }, [this] { m_window.requestRebuild(); },
      [this] { m_window.invalidate(); });
  m_info->setRestore([this](const fs::path& version, const fs::path& folder) {
    // After the click returns: dismissing the sheet destroys the button that called this.
    DeferredCall::callLater([this, version, folder, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      m_window.dismissSheet();
      // Same name in the folder: the conflict sheet asks Replace or Keep Both.
      startJob("Restoring", 1, [&](FileOpsClient& ops, auto done) { ops.copy({version}, folder, done); });
    });
  });
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

std::optional<LinkShare::Device> FilesWindow::galleryPhone() const {
  const auto devices = shareDevices();
  const auto connected = std::ranges::find_if(devices, &LinkShare::Device::connected);
  if (connected != devices.end()) {
    return *connected;
  }
  return devices.empty() ? std::nullopt : std::optional(devices.front());
}

void FilesWindow::galleryAction(std::string_view action, float sceneX, float sceneY) {
  const auto paths = selectedPaths();
  if (paths.size() != 1) {
    return;
  }
  const fs::path& photo = paths.front();
  if (action == "favourite") {
    toggleStar();
  } else if (action == "rotate") {
    if (!rotateJpegClockwise(photo)) {
      showToast(std::format("“{}” cannot be rotated without loss.", photo.filename().string()), false);
      return;
    }
    m_app.thumbnails().reload(photo.string());
  } else if (action == "markup") {
    openWith();
  } else if (action == "phone") {
    if (const auto device = galleryPhone()) {
      sendToPhone(device->id, paths);
    }
  } else if (action == "share") {
    showShareMenu(sceneX, sceneY);
  } else if (action == "more") {
    const auto& rows = m_browser->rows();
    const auto hit = std::ranges::find_if(rows, [&](const auto& r) { return m_browser->pathOf(*r) == photo; });
    if (hit != rows.end()) {
      showItemMenu(static_cast<std::size_t>(hit - rows.begin()), sceneX, sceneY);
    }
  }
  m_browser->refreshGallery();
}

std::vector<LinkShare::Device> FilesWindow::shareDevices() const {
  LinkShare* link = m_app.linkShare();
  return link != nullptr ? link->devices() : std::vector<LinkShare::Device>{};
}

void FilesWindow::shareTo(const std::string& deviceId) { sendToPhone(deviceId, m_browser->selectedPaths()); }

void FilesWindow::sendToPhone(const std::string& deviceId, std::vector<fs::path> paths) {
  LinkShare* link = m_app.linkShare();
  std::erase_if(paths, [](const fs::path& p) {
    std::error_code ec;
    return !fs::is_regular_file(p, ec); // folders do not go to a phone
  });
  if (link == nullptr) {
    return;
  }
  if (paths.empty()) {
    showToast("Only files can go to a phone, not folders.", false);
    return;
  }
  std::string name = deviceId;
  for (const auto& d : link->devices()) {
    if (d.id == deviceId) {
      name = d.name;
    }
  }
  std::uint64_t bytes = 0;
  for (const fs::path& p : paths) {
    std::error_code ec;
    bytes += fs::file_size(p, ec);
  }
  link->send(deviceId, paths, [this, link, name, count = paths.size(), bytes, alive = std::weak_ptr<bool>(m_alive)](
                                  const std::string& transferId, const std::string& error) {
    if (alive.expired()) {
      return;
    }
    kLog.debug("share result {}", error.empty() ? "sent" : error);
    showToast(error.empty() ? std::format("Sending {} to {}", noun(count), name)
                            : std::format("Could not send to {}: {}", name, error),
              false);
    if (error.empty() && !transferId.empty()) {
      const std::string key = "link:" + transferId;
      m_phoneJobs[key] = FileOpsClient::Job{.path = key, .kind = "send", .state = "running", .totalFiles = count,
                                            .totalBytes = bytes, .destination = fs::path(name)};
      updateJobPill();
      // The phone may decline or have no space: the end of the transfer gets its own toast.
      link->watch(transferId, {.progress = [this, alive, key](std::uint64_t done, std::uint64_t total) {
                                 if (alive.expired()) {
                                   return;
                                 }
                                 if (auto it = m_phoneJobs.find(key); it != m_phoneJobs.end()) {
                                   it->second.doneBytes = done;
                                   it->second.totalBytes = total;
                                   updateJobPill();
                                 }
                               },
                               .finished = [this, alive, name, count, key](const std::string& status,
                                                                           const std::string&) {
                                 if (alive.expired()) {
                                   return;
                                 }
                                 m_phoneJobs.erase(key);
                                 updateJobPill();
                                 kLog.debug("share finished {}", status);
                                 const std::string why = LinkShare::outcome(status, name);
                                 showToast(why.empty() ? std::format("Sent {} to {}", noun(count), name) : why, false);
                               }});
    }
  });
}

void FilesWindow::showPhone(const std::string& deviceId) {
  LinkShare* link = m_app.linkShare();
  if (link == nullptr) {
    return;
  }
  if (m_phone == nullptr || m_phone->deviceId() != deviceId) {
    const auto alive = std::weak_ptr<bool>(m_alive);
    m_phone = std::make_shared<PhonePage>(
        *link, deviceId,
        PhonePage::Actions{
            .rebuild = [this] { m_window.requestRebuild(); },
            .open =
                [this](const fs::path& path) {
                  std::error_code ec;
                  if (fs::is_directory(path, ec)) {
                    m_browser->jump(path);
                  } else {
                    m_browser->jump(path.parent_path());
                  }
                },
            .quickLook = [this](const fs::path& path) { quickLook(path, false); },
            .sendFiles =
                [this, alive, deviceId] {
                  m_app.pickFiles("Send Files", "Send", [this, alive, deviceId](std::vector<fs::path> files) {
                    if (!alive.expired() && !files.empty()) {
                      sendToPhone(deviceId, std::move(files));
                    }
                  });
                },
            .showImport = [this](std::shared_ptr<PhonePage> page) { showPhoneImport(std::move(page)); },
            .toast = [this](const std::string& text) { showToast(text, false); },
            .chooseFolder =
                [this, alive](std::function<void(const fs::path&)> chosen) {
                  m_app.pickFiles("Import To", "Choose", [alive, chosen = std::move(chosen)](std::vector<fs::path> picked) {
                    if (!alive.expired() && !picked.empty()) {
                      chosen(picked.front());
                    }
                  }, true);
                },
        });
  }
  m_phone->load();
  const LinkShare::Device* d = link->device(deviceId);
  kLog.info("phone page {}", deviceId);
  m_browser->showListing({.id = "phone:" + deviceId, .title = d != nullptr ? d->name : std::string("Phone"), .rows = {},
                          .activatable = false, .page = [page = m_phone](float scale) { return page->build(scale); }});
}

void FilesWindow::showPhoneImport(std::shared_ptr<PhonePage> page) {
  if (page != nullptr) {
    m_phone = std::move(page); // the page that asked: Back may show an older phone's page
  }
  if (m_phone == nullptr) {
    return;
  }
  kLog.info("phone import view {}", m_phone->deviceId());
  m_browser->showListing({.id = "phone-import:" + m_phone->deviceId(), .title = "Import", .rows = {}, .activatable = false,
                          .page = [page = m_phone](float scale) { return page->buildImport(scale); }});
}

void FilesWindow::phonePick(std::size_t index) {
  if (m_phone != nullptr) {
    m_phone->pick(index);
  }
}

void FilesWindow::phoneImport(bool all) {
  if (m_phone != nullptr) {
    m_phone->import(all);
  }
}

void FilesWindow::shareByEmail() {
  // xdg-email opens the user's mail app with the files attached.
  std::vector<std::string> args{"xdg-email"};
  for (const auto& path : m_browser->selectedPaths()) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      args.push_back("--attach");
      args.push_back(path.string());
    }
  }
  if (args.size() == 1) {
    return;
  }
  std::vector<char*> argv;
  for (auto& a : args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);
  GError* error = nullptr;
  if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
    showToast(std::format("Could not open the mail app: {}", error->message), false);
    g_error_free(error);
    return;
  }
  kLog.debug("share by email {} files", (args.size() - 1) / 2);
}

void FilesWindow::askNewVault(const std::string& name) { presentVaultSheet(VaultSheet::Mode::Create, name); }

void FilesWindow::askUnlockVault(const std::string& name) {
  const Vaults::Vault* vault = m_app.vaults().find(name);
  if (vault == nullptr) {
    return;
  }
  if (Vaults::unlocked(*vault)) {
    go(vault->mount);
    return;
  }
  presentVaultSheet(VaultSheet::Mode::Unlock, name);
}

void FilesWindow::presentVaultSheet(VaultSheet::Mode mode, const std::string& name) {
  const auto close = [this, alive = std::weak_ptr<bool>(m_alive)] {
    m_window.dismissSheet();
    DeferredCall::callLater([this, alive] {
      if (!alive.expired()) {
        m_vaultSheet.reset();
      }
    });
  };
  // gocryptfs answers after the sheet may be gone: the result finds it through a weak pointer.
  auto self = std::make_shared<std::weak_ptr<VaultSheet>>();
  auto sheet = std::make_shared<VaultSheet>(
      mode, name,
      [this, mode, self, alive = std::weak_ptr<bool>(m_alive)](const std::string& vaultName,
                                                                const std::string& password) {
        auto done = [this, self, alive, vaultName](const std::string& error) {
          if (alive.expired()) {
            return;
          }
          auto live = self->lock();
          if (!error.empty()) {
            if (live != nullptr) {
              live->setError(error);
            }
            return;
          }
          if (live != nullptr && live == m_vaultSheet) {
            m_window.dismissSheet();
            m_vaultSheet.reset();
          }
          loadTrash(); // the sidebar shows the vault open
          if (const Vaults::Vault* vault = m_app.vaults().find(vaultName)) {
            go(vault->mount);
          }
        };
        if (mode == VaultSheet::Mode::Create) {
          m_app.vaults().create(vaultName, password, std::move(done));
        } else if (const Vaults::Vault* vault = m_app.vaults().find(vaultName)) {
          m_app.vaults().unlock(*vault, password, std::move(done));
        }
      },
      close, [this] { m_window.requestRebuild(); }, &m_window.input());
  *self = sheet;
  m_vaultSheet = sheet;
  m_window.presentSheet([sheet](float scale) { return sheet->build(scale); }, close);
}

void FilesWindow::mountDrive(const std::string& id) {
  Drives* drives = m_app.drives();
  if (drives == nullptr) {
    return;
  }
  auto alive = std::weak_ptr<bool>(m_alive);
  const std::vector<Drives::Drive> list = drives->list();
  const auto drive = std::ranges::find(list, id, &Drives::Drive::id);
  if (drive != list.end() && drive->encrypted && !drive->device.empty()) {
    // The passphrase first, then udisks unlocks and mounts; a wrong one stays in the sheet with its error.
    kLog.debug("drive asks password for {}", drive->name);
    auto self = std::make_shared<std::weak_ptr<VaultSheet>>();
    const auto close = [this] {
      m_window.dismissSheet();
      DeferredCall::callLater([this] { m_vaultSheet.reset(); });
    };
    auto sheet = std::make_shared<VaultSheet>(
        VaultSheet::Mode::Unlock, drive->name,
        [this, alive, self, device = drive->device](const std::string&, const std::string& password) {
          Drives::unlock(device, password, [this, alive, self](const fs::path& root, const std::string& error) {
            if (alive.expired()) {
              return;
            }
            auto live = self->lock();
            if (!error.empty()) {
              if (live != nullptr) {
                live->setError(error);
              } else {
                showToast(std::format("Can’t open the drive: {}", error), false);
              }
              return;
            }
            if (live != nullptr && live == m_vaultSheet) {
              m_window.dismissSheet();
              m_vaultSheet.reset();
            }
            refreshPlaces();
            go(root);
          });
        },
        close, [this] { m_window.requestRebuild(); }, &m_window.input());
    *self = sheet;
    m_vaultSheet = sheet;
    m_window.presentSheet([sheet](float scale) { return sheet->build(scale); }, close);
    return;
  }
  drives->mount(
      id,
      // An encrypted drive asks for its passphrase in the Unlock sheet; Cancel answers no password.
      [this, alive](const std::string& name, const std::string&, std::function<void(const std::string*)> answer) {
        if (alive.expired()) {
          answer(nullptr);
          return;
        }
        auto reply = std::make_shared<std::function<void(const std::string*)>>(std::move(answer));
        const auto close = [this, reply] {
          if (*reply) {
            (*reply)(nullptr);
            *reply = nullptr;
          }
          m_window.dismissSheet();
          DeferredCall::callLater([this] { m_vaultSheet.reset(); });
        };
        m_vaultSheet = std::make_shared<VaultSheet>(
            VaultSheet::Mode::Unlock, name,
            [this, reply](const std::string&, const std::string& password) {
              if (*reply) {
                (*reply)(&password);
                *reply = nullptr;
              }
              m_window.dismissSheet();
              DeferredCall::callLater([this] { m_vaultSheet.reset(); });
            },
            close, [this] { m_window.requestRebuild(); }, &m_window.input());
        m_window.presentSheet([sheet = m_vaultSheet](float scale) { return sheet->build(scale); }, close);
      },
      [this, alive](const fs::path& root, const std::string& error) {
        if (alive.expired() || error == "cancelled") {
          return;
        }
        if (!error.empty()) {
          showToast(std::format("Can’t open the drive: {}", error), false);
          return;
        }
        refreshPlaces();
        go(root);
      });
}

fs::path FilesWindow::importSource() const {
  // The DCIM folder of the drive shown now: at the location, under it, or above it.
  const fs::path here = location();
  for (fs::path p = here; !p.empty(); p = p.parent_path()) {
    if (p.filename() == "DCIM") {
      return p;
    }
    if (p == p.root_path()) {
      break;
    }
  }
  return PhotoImport::dcim(here);
}

void FilesWindow::importPhotos() {
  const fs::path dcim = importSource();
  if (dcim.empty()) {
    return;
  }
  // A card of thousands of photos takes a while to walk, and a slow card longer: the scan runs on a worker.
  std::thread([this, dcim, imports = m_app.imports(), alive = std::weak_ptr<bool>(m_alive)] {
    std::vector<fs::path> files = imports.pending(dcim);
    DeferredCall::callLater([this, dcim, files = std::move(files), alive]() mutable {
      if (!alive.expired()) {
        importFiles(dcim, std::move(files));
      }
    });
  }).detach();
}

void FilesWindow::importFiles(const fs::path& dcim, std::vector<fs::path> files) {
  kLog.debug("import {} new files from {}", files.size(), dcim.string());
  if (files.empty()) {
    showToast("All photos on this drive are imported", false);
    return;
  }
  const fs::path dest = PhotoImport::destination();
  std::error_code ec;
  fs::create_directories(dest, ec);
  startJob(
      "Importing", files.size(), [&](FileOpsClient& ops, auto done) { ops.copy(files, dest, done); },
      [this, files, dest](const std::string& job) {
        m_renameDone[job] = [this, files, dest](const std::string& error) {
          if (!error.empty()) {
            return;
          }
          // A conflict the user skipped copied nothing: only a photo that is now in the destination counts as
          // imported (same name and size), so a skipped one is offered again next time.
          std::thread([this, files, dest, alive = std::weak_ptr<bool>(m_alive)] {
            std::vector<fs::path> done;
            for (const fs::path& f : files) {
              std::error_code a;
              std::error_code b;
              const auto size = fs::file_size(f, a);
              if (!a && fs::file_size(dest / f.filename(), b) == size && !b) {
                done.push_back(f);
              }
            }
            DeferredCall::callLater([this, done = std::move(done), dest, alive] {
              if (!alive.expired()) {
                m_app.imports().remember(done);
                kLog.debug("imported {} files to {}", done.size(), dest.string());
              }
            });
          }).detach();
        };
      });
}

std::string FilesWindow::formatTarget() const {
  // Removable drives only: Format never offers the system or home drive.
  // The drive whose mount point holds the location: whole path components only (/run/media/u/USB never matches
  // /run/media/u/USB1), and the deepest mount wins (a drive mounted inside another).
  const std::string here = location().lexically_normal().string();
  const std::vector<Volume> volumes = listVolumes(); // `best` points into it
  const Volume* best = nullptr;
  for (const Volume& v : volumes) {
    const std::string mount = v.mountPoint.lexically_normal().string();
    const bool inside = here == mount || here.starts_with(mount.ends_with('/') ? mount : mount + "/");
    // At the same mount (an encrypted drive lists its device and its unlocked mapper), the removable one counts.
    const std::size_t bestSize = best != nullptr ? best->mountPoint.lexically_normal().string().size() : 0;
    if (inside && (best == nullptr || mount.size() > bestSize || (mount.size() == bestSize && v.removable))) {
      best = &v;
    }
  }
  return best != nullptr && best->removable ? Drives::deviceOf(best->mountPoint) : std::string();
}

void FilesWindow::askFormat() {
  const std::string device = formatTarget();
  if (device.empty()) {
    return;
  }
  const std::vector<std::string> types = Drives::formats();
  if (types.empty()) {
    showToast("No formatting tools are installed", false);
    return;
  }
  m_formatName = location().filename().string();
  m_formatDevice = device;
  m_formatTypes = types;
  m_formatType = 0;
  m_formatError.clear();
  m_formatBusy = false;
  static const std::map<std::string, std::string> kLabels{{"exfat", "ExFAT (all computers)"},
                                                          {"vfat", "FAT32 (old devices, 4 GB file limit)"},
                                                          {"ntfs", "NTFS (Windows)"},
                                                          {"ext4", "ext4 (Linux)"}};
  m_window.presentSheet(
      [this, device, types](float s) {
        auto sheet = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                                 .padding = Tokens::componentDialogPadding * s,
                                 .width = Tokens::componentDialogWidth * s});
        sheet->addChild(ui::label({.text = std::format("Erase “{}”?", m_formatName),
                                   .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                                   .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                                   .ellipsize = TextEllipsize::Middle}));
        sheet->addChild(ui::label({.text = std::format("Everything on {} is deleted for good.", device),
                                   .fontSize = Style::fontSizeCaption * s, .color = colorSpecFromRole(ColorRole::Label2),
                                   .maxLines = 2}));
        sheet->addChild(ui::input({.value = m_formatName, .placeholder = std::string("Name"),
                                   .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                                   .onChange = [this](const std::string& v) { m_formatName = v; }}));
        std::vector<std::string> options;
        for (const std::string& t : types) {
          options.push_back(kLabels.contains(t) ? kLabels.at(t) : t);
        }
        sheet->addChild(ui::select({.options = options, .selectedIndex = m_formatType,
                                    .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                                    .onSelectionChanged = [this](std::size_t i, std::string_view) { m_formatType = i; }}));
        if (!m_formatError.empty()) {
          sheet->addChild(ui::label({.text = m_formatError, .fontSize = Style::fontSizeCaption * s,
                                     .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 3}));
        }
        auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
        footer->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default,
                                     .onClick = [this] { DeferredCall::callLater([this] { m_window.dismissSheet(); }); }}));
        footer->addChild(ui::button({.text = std::string(m_formatBusy ? "Erasing…" : "Erase"), .enabled = !m_formatBusy,
                                     .variant = ButtonVariant::Destructive, .onClick = [this] { eraseDrive(); }}));
        sheet->addChild(std::move(footer));
        return sheet;
      },
      [this] { m_window.dismissSheet(); });
}

void FilesWindow::eraseDrive() {
  if (m_formatBusy || m_formatDevice.empty() || m_formatTypes.empty()) {
    return;
  }
  m_formatBusy = true;
  m_window.requestRebuild();
  go(g_get_home_dir()); // the window's own folder watch would keep the drive busy
  Drives::format(m_formatDevice, m_formatTypes[std::min(m_formatType, m_formatTypes.size() - 1)], m_formatName,
                 [this, alive = std::weak_ptr<bool>(m_alive)](const std::string& error) {
                   if (alive.expired()) {
                     return;
                   }
                   m_formatBusy = false;
                   if (error.empty()) {
                     m_formatDevice.clear();
                     m_window.dismissSheet();
                     showToast(std::format("“{}” is erased", m_formatName), false);
                     refreshPlaces();
                   } else {
                     m_formatError = error;
                     m_window.requestRebuild();
                   }
                 });
}

void FilesWindow::submitSheetPassword(const std::string& password) {
  if (m_vaultSheet != nullptr) {
    m_vaultSheet->submitPassword(password);
  }
}

void FilesWindow::lockVault(const std::string& name) {
  const Vaults::Vault* vault = m_app.vaults().find(name);
  if (vault == nullptr) {
    return;
  }
  // Out of the vault first: the window's own folder watch would keep the mount busy.
  if (location().string().starts_with(vault->mount.string())) {
    go(g_get_home_dir());
  }
  m_app.vaults().lock(*vault, [this, alive = std::weak_ptr<bool>(m_alive), name](const std::string& error) {
    if (alive.expired()) {
      return;
    }
    if (!error.empty()) {
      showToast(std::format("Can’t lock “{}”: {}", name, error), false);
    }
    loadTrash();
  });
}

void FilesWindow::askDeleteSelection() {
  auto paths = m_browser->selectedPaths();
  if (paths.empty()) {
    return;
  }
  const std::string what = paths.size() == 1 ? std::format("“{}”", paths.front().filename().string()) : noun(paths.size());
  kLog.debug("ask delete {} items", paths.size());
  m_window.presentSheet(
      [this, paths, what](float s) {
        auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                               .width = Tokens::componentFilesNameField * s});
        box->addChild(ui::label({.text = std::format("Delete {} immediately?", what),
                                 .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                                 .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2}));
        box->addChild(ui::label({.text = std::string("It does not go to the Trash. You can’t undo this."),
                                 .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2),
                                 .maxLines = 3}));
        auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
        buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Style::fontSizeBody * s,
                                      .variant = ButtonVariant::Default, .onClick = [this] { m_window.dismissSheet(); }}));
        buttons->addChild(ui::button({.text = std::string("Delete"), .fontSize = Style::fontSizeBody * s,
                                      .variant = ButtonVariant::Destructive, .onClick = [this, paths] {
                                        m_window.dismissSheet();
                                        startJob("Deleting", paths.size(),
                                                 [&](FileOpsClient& ops, auto done) { ops.remove(paths, done); });
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
      [this, paths](const fs::path& destination, const std::string& format, const std::string& password,
                    std::uint64_t volumeSize) {
        m_window.dismissSheet();
        startJob("Compressing", paths.size(), [&](FileOpsClient& ops, auto done) {
          ops.compress(paths, destination, format, password, volumeSize, done);
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
                                    bool remember, bool reconnect) {
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
        }, reconnect);
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
  // One listing per folder typed: a folder on a hung mount blocks only its own worker, never the next folder.
  if (dir != m_pathListDir && dir != m_pathListAsked) {
    m_pathListAsked = dir;
    std::thread([this, dir, alive = std::weak_ptr<bool>(m_alive)] {
      std::vector<std::string> names;
      std::error_code ec;
      std::size_t read = 0;
      for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end && names.size() < kPathMatchScan && read++ < 16 * kPathMatchScan; it.increment(ec)) {
        std::error_code entry; // its own code: a broken link must not end the walk
        if (it->is_directory(entry)) {
          names.push_back(it->path().filename().string());
        }
      }
      DeferredCall::callLater([this, dir, names = std::move(names), alive]() mutable {
        if (alive.expired() || dir != m_pathListAsked) {
          return;
        }
        m_pathListDir = dir;
        m_pathListNames = std::move(names);
        m_window.requestRebuild();
      });
    }).detach();
  }
  std::vector<std::string> out;
  if (dir != m_pathListDir) {
    return out;
  }
  for (const std::string& name : m_pathListNames) {
    if (!(name.starts_with('.') && !prefix.starts_with('.')) && TypeAhead::fold(name).starts_with(prefix)) {
      out.push_back((dir / name).string() + "/");
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
void FilesWindow::showInGallery(const std::string& name) { m_browser->showInGallery(name); }
void FilesWindow::setView(FileBrowser::View view) { m_browser->setView(view); }
void FilesWindow::toggleHidden() { m_browser->setShowHidden(!m_browser->showHidden()); }

void FilesWindow::groupBy(FileGroupBy group) {
  if (group == FileGroupBy::Tags) {
    m_app.tags().refresh(); // the walk answers through tagsChanged
  }
  m_browser->setGroupBy(group);
}

void FilesWindow::reload() {
  if (inTrash()) {
    loadTrash();
  } else if (inSmartFolder()) {
    goSmart(m_smartShown);
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
  if (!inFolder()) {
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
  startJob("Undoing", 1, [&](FileOpsClient& c, auto done) { c.undo(job, done); },
           [this, tagJob = m_app.tags().ownsJob(job)](const std::string& undoJob) {
             if (tagJob) {
               m_renameDone[undoJob] = [this](const std::string&) { m_app.tags().refresh(true); };
             }
           });
}

bool FilesWindow::canUndo() const noexcept { return !m_app.undoStack().empty(); }

void FilesWindow::showConflict(const FileOpsClient::Job& job) {
  const auto& [source, target] = *job.conflict;
  kLog.debug("conflict sheet {}", job.folderClash.has_value() ? "merge" : "file");
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
        case ConflictSheet::Choice::Merge:
          resolveConflict(FileOpsClient::Resolution::Merge);
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
  kLog.debug("conflict resolve {} {}", static_cast<int>(resolution), *m_conflictJob);
  m_app.fileOps()->resolveConflict(*m_conflictJob, resolution, m_applyToAll);
  closeConflict();
  rebuild();
}

std::vector<FileOpsClient::Job> FilesWindow::pillJobs() const {
  std::vector<FileOpsClient::Job> out;
  const FileOpsClient* ops = m_app.fileOps();
  const auto now = std::chrono::steady_clock::now();
  for (const std::string& path : m_jobs) {
    const FileOpsClient::Job* job = ops != nullptr ? ops->job(path) : nullptr;
    const auto started = m_jobStarted.find(path);
    if (job != nullptr && !job->finished() && started != m_jobStarted.end() && now - started->second >= kPillDelay) {
      out.push_back(*job);
    }
  }
  for (const auto& [path, job] : m_phoneJobs) {
    out.push_back(job);
  }
  return out;
}

void FilesWindow::armPillTimer() {
  // The earliest job still waiting for its kPillDelay decides when the pill looks again.
  const auto now = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::duration> next;
  for (const auto& [path, started] : m_jobStarted) {
    if (const auto left = started + kPillDelay - now; left > std::chrono::steady_clock::duration::zero()) {
      next = next.has_value() ? std::min(*next, left) : left;
    }
  }
  if (next.has_value()) {
    m_pillTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(*next) + std::chrono::milliseconds(1), [this] {
      updateJobPill();
      armPillTimer();
    });
  }
}

double FilesWindow::jobRate(const std::string& path) const {
  const auto it = m_rateSamples.find(path);
  if (it == m_rateSamples.end() || it->second.size() < 2) {
    return 0.0;
  }
  const auto& [t0, b0] = it->second.front();
  const auto& [t1, b1] = it->second.back();
  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  return seconds > 0.0 && b1 >= b0 ? static_cast<double>(b1 - b0) / seconds : 0.0;
}

void FilesWindow::sampleRate(const FileOpsClient::Job& job) {
  auto& samples = m_rateSamples[job.path];
  if (job.state != "running") {
    samples.clear(); // paused or asking: the time does not count
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  samples.emplace_back(now, job.doneBytes);
  while (samples.size() > 2 && now - samples.front().first > kRateWindow) {
    samples.pop_front();
  }
}

void FilesWindow::updateJobPill() {
  const auto jobs = pillJobs();
  if (jobs.size() != m_pillShown) {
    m_pillShown = jobs.size();
    kLog.debug("job pill {}", jobs.size());
    // A new or finished job: the open popover takes its new height on the rebuilt pill, in place (layout callback);
    // with no job left it closes.
    if (jobs.empty()) {
      m_window.closePopover();
    }
    m_resizeJobs = m_window.popoverOpen();
    m_window.requestRebuild();
    return;
  }
  if (m_pillRing != nullptr) {
    m_pillRing->setProgress(job_pill::combined(jobs));
    m_window.invalidate();
  }
  if (!m_window.popoverOpen()) {
    return;
  }
  // A rebuild between press and release drops the click: rebuild only when the rows' circles change.
  if (job_pill::layoutKey(jobs) != m_popoverKey) {
    m_window.refreshPopover();
  } else {
    job_pill::update(m_popoverRows, jobs, [this](const std::string& path) { return jobRate(path); });
    m_window.invalidatePopover();
  }
}

void FilesWindow::openViewOptions(const Node* anchor) {
  if (m_picker.has_value()) {
    return;
  }
  // FILES-UI-SPEC §16: 340 wide (five sort segments + the direction circle), padding 16, gap 12; captions 500 12
  // label_2 over their controls. The height is the sum of the rows; the icon size row shows in the icons view only.
  constexpr float kWidth = 340.0F;
  constexpr float kBaseHeight = 400.0F;
  constexpr float kIconRow = 60.0F;
  const float kHeight = kBaseHeight + (m_browser->view() == FileBrowser::View::Icons ? kIconRow : 0.0F);
  const Node* at = anchor != nullptr ? anchor : m_browser->viewOptionsAnchor();
  if (at == nullptr) {
    return;
  }
  kLog.debug("view options");
  m_window.openPopover(at, kWidth, kHeight, [this](float s) {
    FileBrowser& b = *m_browser;
    const FileBrowserViewState v = b.viewState();
    auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP16 * s});
    auto head = ui::column({.align = FlexAlign::Start});
    head->addChild(ui::label({.text = b.title(), .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                              .ellipsize = TextEllipsize::Middle}));
    const bool own = b.listingId().empty() && !location().empty();
    head->addChild(ui::label({.text = own ? "Saved for this folder" : "Applies here until you leave",
                              .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                              .color = colorSpecFromRole(ColorRole::Label2)}));
    box->addChild(std::move(head));
    const auto caption = [&](const char* text) {
      return ui::label({.text = std::string(text), .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                        .color = colorSpecFromRole(ColorRole::Label2)});
    };
    const auto group = [&](const char* text, std::unique_ptr<Node> control) {
      auto g = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
      g->addChild(caption(text));
      g->addChild(std::move(control));
      box->addChild(std::move(g));
    };
    group("View", ui::segmented({.options = std::vector<ui::SegmentedOption>{{.glyph = "layout-grid", .tooltip = "Icons"},
                                                                             {.glyph = "list", .tooltip = "List"},
                                                                             {.glyph = "columns-3", .tooltip = "Columns"},
                                                                             {.glyph = "photo", .tooltip = "Gallery"}},
                                 .selectedIndex = v.view, .scale = s, .equalSegmentWidths = true,
                                 .height = Tokens::sizeControlM * s,
                                 .onChange = [this](std::size_t i) {
                                   m_browser->setView(static_cast<FileBrowser::View>(i));
                                   openViewOptions(); // the icon size row comes or goes: the popover resizes
                                 }}));
    if (static_cast<FileBrowser::View>(v.view) == FileBrowser::View::Icons) {
      group("Icon Size", ui::slider({.minValue = Tokens::componentFilesIconMin, .maxValue = Tokens::componentFilesIconMax,
                                     .step = Tokens::spaceP16, .value = b.iconSize(),
                                     .controlHeight = Tokens::sizeControlS * s,
                                     .onValueChanged = [this](double value) { m_browser->setIconSize(static_cast<float>(value)); }}));
    }
    static constexpr std::array<std::pair<const char*, FileSortKey>, 5> kKeys{{{"Name", FileSortKey::Name},
                                                                               {"Modified", FileSortKey::Modified},
                                                                               {"Added", FileSortKey::Added},
                                                                               {"Size", FileSortKey::Size},
                                                                               {"Kind", FileSortKey::Kind}}};
    std::vector<ui::SegmentedOption> sorts;
    std::size_t current = 0;
    for (std::size_t i = 0; i < kKeys.size(); ++i) {
      sorts.push_back({.label = kKeys[i].first});
      current = kKeys[i].second == v.sort.key ? i : current;
    }
    auto sortRow = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    sortRow->addChild(ui::segmented({.options = std::move(sorts), .selectedIndex = current, .fontSize = Style::fontSizeCaption * s,
                                     .scale = s, .compact = true, .height = Tokens::sizeControlM * s, .flexGrow = 1.0F,
                                     .onChange = [this](std::size_t i) {
                                       FileSort sort = m_browser->sortOrder();
                                       if (sort.key != kKeys[i].second) {
                                         m_browser->setSort(kKeys[i].second);
                                       }
                                       m_window.refreshPopover();
                                     }}));
    Button::ButtonPalette plain = Button::defaultPalette(ButtonVariant::Default);
    plain.borderWidth = 0.0F;
    const float circle = Tokens::sizeControlM * s;
    sortRow->addChild(ui::button({.glyph = std::string(v.sort.descending ? "sort-descending" : "sort-ascending"),
                                  .glyphSize = Tokens::sizeSymbol * s, .variant = ButtonVariant::Default,
                                  .customPalette = plain, .tooltip = std::string(v.sort.descending ? "Descending" : "Ascending"),
                                  .radius = circle / 2.0F, .width = circle, .height = circle, .onClick = [this] {
                                    FileSort sort = m_browser->sortOrder();
                                    sort.descending = !sort.descending;
                                    m_browser->setSortOrder(sort);
                                    m_window.refreshPopover();
                                  }}));
    group("Sort By", std::move(sortRow));
    static constexpr std::array<std::pair<const char*, FileGroupBy>, 5> kGroups{{{"None", FileGroupBy::None},
                                                                                 {"Kind", FileGroupBy::Kind},
                                                                                 {"Date", FileGroupBy::Date},
                                                                                 {"Size", FileGroupBy::Size},
                                                                                 {"Tags", FileGroupBy::Tags}}};
    std::vector<ui::SegmentedOption> groups;
    std::size_t groupNow = 0;
    for (std::size_t i = 0; i < kGroups.size(); ++i) {
      groups.push_back({.label = kGroups[i].first});
      groupNow = kGroups[i].second == v.group ? i : groupNow;
    }
    group("Group By", ui::segmented({.options = std::move(groups), .selectedIndex = groupNow,
                                     .fontSize = Style::fontSizeCaption * s, .scale = s, .compact = true,
                                     .equalSegmentWidths = true, .height = Tokens::sizeControlM * s,
                                     .onChange = [this](std::size_t i) { groupBy(kGroups[i].second); }}));
    const auto toggleRow = [&](const char* text, bool on, std::function<void(bool)> change) {
      auto row = ui::row({.align = FlexAlign::Center, .height = Tokens::sizeRow * s});
      row->addChild(ui::label({.text = std::string(text), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label1), .flexGrow = 1.0F}));
      row->addChild(ui::toggle({.checked = on, .scale = s, .onChange = std::move(change)}));
      return row;
    };
    auto shows = ui::column({.align = FlexAlign::Stretch});
    shows->addChild(toggleRow("Show Hidden Files", v.showHidden, [this](bool on) { m_browser->setShowHidden(on); }));
    shows->addChild(toggleRow("Show File Extensions", v.extensions, [this](bool on) { m_browser->setShowExtensions(on); }));
    box->addChild(std::move(shows));
    box->addChild(ui::button({.text = std::string("Use as Defaults"), .fontSize = Style::fontSizeBody * s,
                              .variant = ButtonVariant::Default, .customPalette = plain, .paddingH = Tokens::spaceP12 * s,
                              .radius = Tokens::radiusR8 * s, .height = Tokens::sizeControlM * s, .onClick = [this] {
                                view_store::writeDefault(view_store::encode(m_browser->viewState()));
                                kLog.debug("view default {}", view_store::encode(m_browser->viewState()));
                                m_window.closePopover();
                              }}));
    return box;
  });
}

void FilesWindow::openJobsPopover() {
  const auto jobs = pillJobs();
  if (m_pillAnchor == nullptr || jobs.empty()) {
    return;
  }
  kLog.debug("job popover open");
  m_window.openPopover(m_pillAnchor, job_pill::kPopoverWidth, job_pill::popoverHeight(jobs.size()), [this](float s) {
    const auto now = pillJobs();
    m_popoverKey = job_pill::layoutKey(now);
    return job_pill::popover(
        now, s, [this](const std::string& path) { return jobRate(path); },
        [this](const std::string& path) {
          kLog.debug("job cancel {}", path);
          if (path.starts_with("link:")) {
            if (LinkShare* link = m_app.linkShare()) {
              link->cancel(path.substr(5));
            }
          } else if (FileOpsClient* ops = m_app.fileOps()) {
            ops->cancel(path);
          }
        },
        [this](const std::string& path, bool pause) {
          kLog.debug("job {} {}", pause ? "pause" : "resume", path);
          if (FileOpsClient* ops = m_app.fileOps()) {
            pause ? ops->pause(path) : ops->resume(path);
          }
        },
        &m_popoverRows);
  });
}

void FilesWindow::onJobChanged(const FileOpsClient::Job& job) {
  if (m_jobs.contains(job.path)) {
    kLog.debug("job {} {} {}", job.kind, job.state, job.path);
    if (m_jobStarted.emplace(job.path, std::chrono::steady_clock::now()).second) {
      // Short jobs (rename, new folder) finish before the pill would show.
      armPillTimer();
    }
    sampleRate(job);
    if (job.finished()) {
      m_jobStarted.erase(job.path);
      m_rateSamples.erase(job.path);
    }
    updateJobPill();
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
    // FolderClash can arrive in a later PropertiesChanged than the conflict: the Merge sheet then replaces the file one.
    if (m_conflictJob != job.path || m_conflict == nullptr || job.conflictIndex != m_conflictShownIndex ||
        (job.folderClash.has_value() && !m_conflict->isMerge())) {
      m_conflictJob = job.path;
      m_conflictShownIndex = job.conflictIndex;
      showConflict(job);
    }
    return;
  }
  if (!job.finished()) {
    return; // the job pill shows the progress
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
        : job.kind == "tags"                    ? "Tagged"
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

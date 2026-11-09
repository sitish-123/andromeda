#include "shell/desktop/desktop_icons.h"

#include "config/config_service.h"
#include "dbus/session_bus.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_file_ops.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/drag_and_drop.h"
#include "wayland/drag_icon_surface.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr Logger kLog("desktop-icons");

  // DESIGN.md "Desktop icons": 96 x 104 cells 16 from the usable-area edge; icon on a 72 plate (r 8) with the name 4
  // below, 2 lines (all lines while selected) in a 92 wide pill (r 4).
  constexpr int kMargin = 16;
  constexpr int kPlateRound = 8;
  constexpr int kNameGap = 4;
  constexpr int kNamePad = 4;
  constexpr int kNameRound = 4;
  constexpr int kBandRound = 4;
  constexpr int kMenuW = 220;
  constexpr float kDragSlop = 8.0F; // rulebook 7.7
  constexpr float kDraggedOpacity = 0.5F;
  constexpr auto kDoubleClick = std::chrono::milliseconds(400);

  enum MenuId : std::int32_t {
    NewFolder = 1,
    SortName,
    SortKind,
    SortDate,
    ChangeWallpaper,
    CleanUp,
    Open,
    Trash,
    Rename,
    IconSmall,
    IconMedium,
    IconLarge,
    UseStacks,
    SnapToGrid,
    NewDocument,
    Paste,
    Cut,
    Copy,
    Properties,
    OpenWithFirst = 100, // + index into the app list of the menu
  };
  constexpr std::size_t kOpenWithMax = 4;

  std::string fileUri(const std::filesystem::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      return {};
    }
    std::string out = uri;
    g_free(uri);
    return out;
  }

  // Freedesktop icon names for a MIME type: "image/png" -> "image-png", then the "image-x-generic" family icon.
  std::vector<std::string> mimeIconNames(const FileInfo& info) {
    if (info.isDir) {
      return {"folder"};
    }
    std::string specific = info.contentType;
    std::ranges::replace(specific, '/', '-');
    const std::string major = info.contentType.substr(0, info.contentType.find('/'));
    return {specific, major + "-x-generic", "text-x-generic"};
  }
} // namespace

DesktopIcons::DesktopIcons() = default;

DesktopIcons::~DesktopIcons() {
  close();
  if (m_peekListener >= 0) {
    FileIcon::removePeekListener(m_peekListener);
  }
}

void DesktopIcons::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
                              ThumbnailService& thumbnails, SessionBus* bus) {
  m_wayland = &wayland;
  m_fileOps = std::make_unique<DesktopFileOps>(bus);
  m_bus = bus;
  m_config = &config;
  m_renderContext = &renderContext;
  m_thumbnails = &thumbnails;
  m_thumbnailUploads = thumbnails.subscribePendingUpload([this] {
    if (m_drawsFileIcons) {
      requestBuild();
    }
  });
  m_peekListener = FileIcon::addPeekListener([this] {
    if (m_drawsFileIcons) {
      requestBuild();
    }
  });
  const char* desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
  m_dir = desktop != nullptr ? std::filesystem::path(desktop) : std::filesystem::path(g_get_home_dir()) / "Desktop";
  m_input.setCursorShapeCallback([](std::uint32_t, std::uint32_t) {});
  sync();
}

void DesktopIcons::sync() {
  if (m_wayland == nullptr || m_config == nullptr) {
    return;
  }
  const auto& outputs = m_wayland->outputs();
  wl_output* first = outputs.empty() ? nullptr : outputs.front().output;
  const bool want = m_config->config().desktopIcons.enabled && first != nullptr;
  // Snap to Grid turned on (menu, Settings or config file) snaps every free icon to the cell saved with it.
  const bool snap = m_config->config().desktopIcons.snapToGrid;
  if (snap && m_snapToGrid == false && !m_freeAt.empty()) {
    m_freeAt.clear();
    saveSlots();
  }
  m_snapToGrid = snap;
  if (!want || first != m_output) {
    close();
  }
  if (want && m_surface == nullptr) {
    m_output = first;
    open();
  } else if (want) {
    rebuildCells();
  }
}

bool DesktopIcons::ownsSurface(wl_surface* surface) const {
  return surface != nullptr && m_surface != nullptr && m_surface->wlSurface() == surface;
}

void DesktopIcons::open() {
  if (m_view == nullptr) {
    m_view = std::make_unique<FileModelView>(FileModel::open(m_dir, m_watcher));
    m_view->setSort({.key = FileSortKey::Name, .foldersFirst = true});
    m_view->setChangedCallback([this] {
      m_groupsStale = true;
      rebuildCells();
    });
  }
  // Covers the usable area (exclusive zone 0), under windows; the keyboard only after a click on it.
  m_surface = std::make_unique<LayerSurface>(
      *m_wayland,
      LayerSurfaceConfig{
          .nameSpace = "andromeda-desktop-icons",
          .layer = LayerShellLayer::Bottom,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .exclusiveZone = 0,
          .keyboard = LayerShellKeyboard::OnDemand,
      }
  );
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { rebuildCells(); });
  m_surface->setPrepareFrameCallback([this](bool, bool needsLayout) {
    if (m_surface == nullptr || m_surface->width() == 0 || m_surface->height() == 0) {
      return;
    }
    m_renderContext->makeCurrent(m_surface->renderTarget());
    if (m_sceneRoot == nullptr || needsLayout || m_dirty) {
      build();
    }
  });
  if (!m_surface->initialize(m_output)) {
    kLog.warn("desktop icons: no surface on the first output");
    m_surface.reset();
  }
  if (DragAndDrop* dnd = m_wayland->dragAndDrop(); dnd != nullptr && m_surface != nullptr) {
    dnd->setTarget(m_surface->wlSurface(), {
        .enter = {},
        .motion = [this](float x, float y) -> std::uint32_t {
          if (m_systemDrag) {
            onCellMotion(x, y);
          }
          setDropHighlight(folderAt(x, y));
          return m_systemDrag ? DragAndDrop::Move : DragAndDrop::Copy;
        },
        .leave =
            [this] {
              setDropHighlight(std::nullopt);
              if (m_systemDrag) {
                resetDraggedCells();
              }
            },
        .drop = [this](float x, float y, std::uint32_t action, const std::string&, std::string data) {
          onDropTarget(x, y, action, data);
        },
    });
  }
  loadSlots();
}

void DesktopIcons::onDropTarget(float x, float y, std::uint32_t action, const std::string& data) {
  setDropHighlight(std::nullopt);
  const std::optional<std::size_t> folder = folderAt(x, y);
  const bool move = action == DragAndDrop::Move;
  if (m_systemDrag) {
    // Our own drag: onto a folder the files go in (the negotiated action: move, or copy), elsewhere the icons take
    // new places.
    if (folder.has_value()) {
      const auto paths = selectedPaths();
      kLog.debug("desktop drop {} {} items into {}", move ? "moves" : "copies", paths.size(), m_cells[*folder].info->name);
      transferIn(paths, pathOf(*m_cells[*folder].info), move);
      resetDraggedCells();
    } else {
      moveSelected(x - m_pressX, y - m_pressY);
      drop();
    }
    endSystemDrag(true);
    return;
  }
  if (folder.has_value()) {
    kLog.debug("desktop drop copies into {}", m_cells[*folder].info->name);
    transferIn(urisToPaths(data), pathOf(*m_cells[*folder].info), move);
  } else {
    const Slot slot = slotAt(x, y);
    kLog.debug("desktop drop copies in at {},{}", slot.column, slot.row);
    transferIn(urisToPaths(data), m_dir, false, std::pair{x, y});
  }
}

std::optional<std::size_t> DesktopIcons::folderAt(float x, float y) const {
  for (std::size_t i = 0; i < m_cells.size(); ++i) {
    const Cell& cell = m_cells[i];
    if (!cell.stack && cell.info->isDir && !isSelected(cell) && x >= cell.x && x < cell.x + m_cellW && y >= cell.y
        && y < cell.y + m_cellH) {
      return i;
    }
  }
  return std::nullopt;
}

DesktopIcons::Slot DesktopIcons::slotAt(float x, float y) const {
  const int rows = std::max(1, (static_cast<int>(m_surface->height()) - 2 * kMargin) / m_cellH);
  const int columns = std::max(1, (static_cast<int>(m_surface->width()) - 2 * kMargin) / m_cellW);
  const bool fromRight = m_config->config().desktopIcons.startCorner == DesktopIconsCorner::TopRight;
  const float fromEdge = fromRight ? static_cast<float>(m_surface->width()) - kMargin - x : x - kMargin;
  return {.column = std::clamp(static_cast<int>(fromEdge / m_cellW), 0, columns - 1),
          .row = std::clamp(static_cast<int>((y - kMargin) / m_cellH), 0, rows - 1)};
}

void DesktopIcons::setDropHighlight(std::optional<std::size_t> index) {
  if (index == m_dropHighlight) {
    return;
  }
  for (const auto i : {m_dropHighlight, index}) {
    if (i.has_value() && *i < m_cells.size() && m_cells[*i].plate != nullptr) {
      m_cells[*i].plate->setVisible(i == index || isSelected(m_cells[*i]));
    }
  }
  m_dropHighlight = index;
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

void DesktopIcons::resetDraggedCells() {
  for (const Cell& cell : m_cells) {
    if (cell.area != nullptr) {
      cell.area->setPosition(cell.x, cell.y);
    }
  }
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

void DesktopIcons::startSystemDrag() {
  DragAndDrop* dnd = m_wayland->dragAndDrop();
  const auto paths = selectedPaths();
  if (dnd == nullptr || paths.empty() || !m_pressIndex.has_value() || *m_pressIndex >= m_cells.size()) {
    return;
  }
  std::string uris;
  std::string text;
  for (const auto& path : paths) {
    uris += fileUri(path) + "\r\n";
    text += (text.empty() ? "" : "\n") + path.string();
  }
  // Drag image: the pressed icon at icon size.
  m_dragIcon.reset();
  m_dragImage.reset();
  const Cell& pressed = m_cells[*m_pressIndex];
  const int iconSize = m_config->config().desktopIcons.iconSize;
  const std::string& type = pressed.info->contentType;
  const bool shortcut = !pressed.info->isDir && pressed.info->name.ends_with(".desktop");
  // A picture drags as itself; an app shortcut as its app icon; anything else as its Files icon, as on the desktop.
  const std::string iconFile = !pressed.info->isDir && type.starts_with("image/") ? pathOf(*pressed.info).string()
                               : shortcut                                         ? iconPathFor(*pressed.info)
                                                                                  : std::string{};
  const FileInfo info = *pressed.info;
  const std::filesystem::path path = pathOf(info);
  const int scale = std::max(1, static_cast<int>(std::ceil(m_surface->effectiveBufferScale())));
  auto icon = std::make_unique<DragIconSurface>(*m_wayland, iconSize, iconSize, scale);
  icon->setRenderContext(m_renderContext);
  m_dragImage = ui::node({});
  m_dragImage->setSize(static_cast<float>(iconSize), static_cast<float>(iconSize));
  icon->setPrepareFrameCallback([this, raw = icon.get(), iconFile, info, path, iconSize, loaded = false](bool, bool) mutable {
    m_renderContext->makeCurrent(raw->renderTarget());
    if (loaded || m_dragImage == nullptr) {
      return;
    }
    loaded = true;
    Renderer& renderer = raw->renderTarget().renderer();
    if (iconFile.empty()) {
      auto drawn = std::make_unique<FileIcon>(static_cast<float>(iconSize));
      drawn->setParticipatesInLayout(false);
      drawn->bind(renderer, info, path, nullptr);
      drawn->layout(renderer);
      m_dragImage->addChild(std::move(drawn));
      return;
    }
    auto image = ui::image({.fit = ImageFit::Contain, .width = static_cast<float>(iconSize),
                            .height = static_cast<float>(iconSize), .participatesInLayout = false});
    if (image->setSourceFile(renderer, iconFile, iconSize, true)) {
      m_dragImage->addChild(std::move(image));
    }
  });
  icon->setSceneRoot(m_dragImage.get());
  if (icon->initialize()) {
    m_dragIcon = std::move(icon);
  }
  m_input.cancelPointerCapture();
  m_systemDrag = dnd->startDrag(
      m_surface->wlSurface(), m_pressSerial,
      {.mimeTypes = {"text/uri-list", "text/plain;charset=utf-8", "text/plain"},
       .data = [uris, text](const std::string& mime) { return mime == "text/uri-list" ? uris : text; },
       .actions = DragAndDrop::Copy | DragAndDrop::Move | DragAndDrop::Ask,
       .icon = m_dragIcon != nullptr ? m_dragIcon->wlSurface() : nullptr,
       .finished =
           [this, alive = std::weak_ptr<bool>(m_alive)](std::uint32_t action, bool dropped) {
             if (alive.expired()) {
               return;
             }
             kLog.debug("desktop drag ended action {} dropped {}", action, dropped);
             if (m_systemDrag) {
               endSystemDrag(false);
             }
           }});
  kLog.debug("desktop drag started with {} items", paths.size());
  if (!m_systemDrag) {
    m_dragIcon.reset();
    m_dragImage.reset();
  }
}

void DesktopIcons::endSystemDrag(bool droppedHere) {
  m_systemDrag = false;
  for (const Cell& cell : m_cells) {
    if (cell.area != nullptr) {
      cell.area->setOpacity(1.0F);
    }
  }
  m_pressIndex.reset();
  m_dragging = false;
  if (!droppedHere) {
    rebuildCells();
  }
  // The compositor may still draw the icon this frame: drop it after.
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (!alive.expired()) {
      m_dragIcon.reset();
      m_dragImage.reset();
    }
  });
}

std::vector<std::filesystem::path> DesktopIcons::urisToPaths(const std::string& uriList) {
  std::vector<std::filesystem::path> out;
  std::istringstream lines(uriList);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (gchar* file = g_filename_from_uri(line.c_str(), nullptr, nullptr); file != nullptr) {
      out.emplace_back(file);
      g_free(file);
    }
  }
  return out;
}

void DesktopIcons::transferIn(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& dir,
                              bool move, std::optional<std::pair<float, float>> at, bool duplicate) {
  std::vector<std::filesystem::path> sources;
  for (const auto& source : paths) {
    // Nothing into the folder it is already in (a pasted copy there is a duplicate), and no folder into itself.
    if ((source.parent_path() != dir || (duplicate && !move)) && dir != source
        && !dir.string().starts_with(source.string() + "/")) {
      sources.push_back(source);
    }
  }
  if (sources.empty()) {
    return;
  }
  std::error_code error;
  if (at.has_value() && dir == m_dir && !std::filesystem::exists(dir / sources.front().filename(), error)) {
    placeAt(sources.front().filename().string(), at->first, at->second);
  }
  std::filesystem::create_directories(dir, error);
  if (m_fileOps == nullptr || !m_fileOps->transfer(move, sources, dir)) {
    kLog.warn("{} {} items into {} failed: no FileOps1", move ? "moving" : "copying", sources.size(), dir.string());
  }
}

bool DesktopIcons::canPaste() const {
  const DragAndDrop* dnd = m_wayland->dragAndDrop();
  if (dnd == nullptr) {
    return false;
  }
  if (dnd->ownsSelection()) {
    return !m_clipboard.empty();
  }
  return std::ranges::any_of(dnd->selectionTypes(), [](const std::string& type) {
    return type == "x-special/gnome-copied-files" || type == "text/uri-list";
  });
}

void DesktopIcons::copyToClipboard(const std::vector<std::filesystem::path>& paths, bool cut) {
  DragAndDrop* dnd = m_wayland->dragAndDrop();
  if (dnd == nullptr || paths.empty()) {
    return;
  }
  std::string uris;
  std::string text;
  for (const auto& path : paths) {
    uris += fileUri(path) + "\r\n";
    text += (text.empty() ? "" : "\n") + path.string();
  }
  // GNOME's copied-files type carries cut or copy; Files and Nautilus read it, others take the uri-list.
  std::string gnome = cut ? "cut" : "copy";
  for (const auto& path : paths) {
    gnome += "\n" + fileUri(path);
  }
  const bool set = dnd->setSelection(
      m_wayland->lastInputSerial(),
      {.mimeTypes = {"x-special/gnome-copied-files", "text/uri-list", "text/plain;charset=utf-8", "text/plain"},
       .data = [gnome, uris, text](const std::string& mime) {
         return mime == "x-special/gnome-copied-files" ? gnome : mime == "text/uri-list" ? uris : text;
       }}
  );
  if (set) {
    m_clipboard = paths;
    m_clipboardCut = cut;
  }
  kLog.debug("desktop {} {} items to the clipboard: {}", cut ? "cut" : "copied", paths.size(), set ? "ok" : "failed");
}

void DesktopIcons::paste(const std::filesystem::path& dir, std::optional<std::pair<float, float>> at) {
  DragAndDrop* dnd = m_wayland->dragAndDrop();
  if (dnd == nullptr) {
    return;
  }
  if (dnd->ownsSelection()) {
    if (!m_clipboard.empty()) {
      kLog.debug("desktop paste {} {} items into {}", m_clipboardCut ? "moves" : "copies", m_clipboard.size(), dir.string());
      transferIn(m_clipboard, dir, m_clipboardCut, at, true);
      if (m_clipboardCut) {
        m_clipboard.clear(); // a cut pastes once, as in Files
      }
    }
    return;
  }
  dnd->readSelection(
      {"x-special/gnome-copied-files", "text/uri-list"},
      [this, alive = std::weak_ptr<bool>(m_alive), dir, at](const std::string& mime, std::string bytes) {
        if (alive.expired() || bytes.empty()) {
          return;
        }
        bool cut = false;
        if (mime == "x-special/gnome-copied-files") {
          const std::size_t line = bytes.find('\n');
          cut = bytes.substr(0, line) == "cut";
          bytes = line == std::string::npos ? std::string{} : bytes.substr(line + 1);
        }
        const auto paths = urisToPaths(bytes);
        kLog.debug("desktop paste {} {} items into {}", cut ? "moves" : "copies", paths.size(), dir.string());
        transferIn(paths, dir, cut, at, true);
      },
      static_cast<std::size_t>(1) << 20
  );
}

void DesktopIcons::openWithApp(GAppInfo* app, const std::vector<std::filesystem::path>& paths) {
  GList* files = nullptr;
  for (const auto& path : paths) {
    files = g_list_append(files, g_file_new_for_path(path.c_str()));
  }
  GError* error = nullptr;
  if (!g_app_info_launch(app, files, nullptr, &error)) {
    kLog.warn("open with {} failed: {}", g_app_info_get_name(app), error != nullptr ? error->message : "");
    g_clear_error(&error);
  }
  g_list_free_full(files, g_object_unref);
}

void DesktopIcons::showProperties(const std::vector<std::filesystem::path>& paths) {
  if (m_bus == nullptr || paths.empty()) {
    return;
  }
  std::vector<std::string> uris;
  for (const auto& path : paths) {
    uris.push_back(fileUri(path));
  }
  // Files answers org.freedesktop.FileManager1 (D-Bus activated) with its Get Info window.
  try {
    if (m_fileManager == nullptr) {
      m_fileManager = sdbus::createProxy(m_bus->connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
                                         sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
    }
    m_fileManager->callMethodAsync("ShowItemProperties")
        .onInterface("org.freedesktop.FileManager1")
        .withArguments(uris, std::string{})
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error.has_value()) {
            kLog.warn("ShowItemProperties failed: {}", error->getMessage());
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("FileManager1 is not reachable: {}", e.getMessage());
  }
}

void DesktopIcons::createNew(bool folder, std::optional<std::pair<float, float>> at) {
  const std::string base = i18n::tr(folder ? "desktop-icons.untitled-folder" : "desktop-icons.untitled-document");
  const std::string ext = folder ? "" : ".txt";
  std::filesystem::path path = m_dir / (base + ext);
  for (int n = 2; std::filesystem::exists(path); ++n) {
    path = m_dir / std::format("{} {}{}", base, n, ext);
  }
  std::error_code error;
  std::filesystem::create_directories(m_dir, error);
  const bool made = folder ? std::filesystem::create_directory(path, error) : static_cast<bool>(std::ofstream(path));
  if (!made) {
    kLog.warn("new {} failed: {}", path.string(), error.message());
    return;
  }
  // As on Windows: the item appears where it was asked for, selected, with its name ready to type.
  const std::string name = path.filename().string();
  kLog.debug("desktop new {}", name);
  if (at.has_value()) {
    placeAt(name, at->first, at->second);
  }
  m_selected = {name};
  m_renaming = name;
  m_renameText.reset();
  requestBuild();
}

void DesktopIcons::addShortcut(const std::filesystem::path& desktopEntry) {
  kLog.debug("desktop shortcut for {}", desktopEntry.filename().string());
  transferIn({desktopEntry}, m_dir, false);
}

bool DesktopIcons::freePlacement() const {
  return !m_config->config().desktopIcons.snapToGrid && !m_config->config().desktopIcons.stacks;
}

void DesktopIcons::placeAt(const std::string& name, float x, float y) {
  if (m_surface == nullptr || m_config->config().desktopIcons.stacks) {
    return;
  }
  const Slot slot = slotAt(x, y);
  const bool free = freePlacement();
  if (!free && std::ranges::any_of(m_cells, [&](const Cell& cell) { return cell.slot == slot && cell.info->name != name; })) {
    return; // the cell is taken: the file fills the next free one
  }
  for (const Cell& cell : m_cells) {
    m_slots.try_emplace(cell.info->name, cell.slot);
  }
  m_slots[name] = slot;
  if (free) {
    // The point is the middle of the icon plate.
    const float w = static_cast<float>(m_surface->width());
    const float left = std::clamp(x - static_cast<float>(m_cellW) / 2.0F, 0.0F, w - static_cast<float>(m_cellW));
    const float top = std::clamp(y - static_cast<float>(m_plate) / 2.0F, 0.0F,
                                 static_cast<float>(m_surface->height()) - static_cast<float>(m_cellH));
    const bool fromRight = m_config->config().desktopIcons.startCorner == DesktopIconsCorner::TopRight;
    m_freeAt[name] = {fromRight ? w - static_cast<float>(m_cellW) - left : left, top};
  } else {
    m_freeAt.erase(name);
  }
  saveSlots();
  rebuildCells();
}

std::string DesktopIcons::keyOf(const Cell& cell) const {
  return cell.stack ? std::format("\x1fstack:{}", static_cast<int>(*cell.stack)) : cell.info->name;
}

// A stack is selected under its own key, so one file picked inside an open stack never stands for the whole stack.
bool DesktopIcons::isSelected(const Cell& cell) const { return m_selected.contains(keyOf(cell)); }

void DesktopIcons::selectCell(const Cell& cell, bool on) {
  on ? (void)m_selected.insert(keyOf(cell)) : (void)m_selected.erase(keyOf(cell));
}

std::vector<std::filesystem::path> DesktopIcons::selectedPaths() const {
  std::vector<std::filesystem::path> out;
  std::set<std::string> seen;
  const auto add = [&](const FileInfo& info) {
    if (seen.insert(info.name).second) {
      out.push_back(pathOf(info));
    }
  };
  for (const Cell& cell : m_cells) {
    if (!isSelected(cell)) {
      continue;
    }
    if (cell.stack) {
      for (const auto& member : cell.members) {
        add(*member);
      }
    } else {
      add(*cell.info);
    }
  }
  return out;
}

DesktopIcons::StackKind DesktopIcons::stackKindOf(const FileInfo& info) {
  const std::string& type = info.contentType;
  const auto has = [&](std::string_view part) { return type.find(part) != std::string::npos; };
  if (type.starts_with("image/")) {
    return StackKind::Images;
  }
  if (type.starts_with("video/")) {
    return StackKind::Movies;
  }
  if (type.starts_with("audio/")) {
    return StackKind::Music;
  }
  if (type == "application/pdf") {
    return StackKind::Pdfs;
  }
  if (has("spreadsheet") || has("excel") || type == "text/csv") {
    return StackKind::Spreadsheets;
  }
  if (has("presentation") || has("powerpoint")) {
    return StackKind::Presentations;
  }
  if (type == "application/zip" || has("7z") || has("x-tar") || has("compressed-tar") || type == "application/gzip"
      || has("x-xz") || has("zstd") || has("rar") || has("bzip")) {
    return StackKind::Archives;
  }
  if (type.starts_with("text/") || has("document") || has("msword") || has("rtf")) {
    return StackKind::Documents;
  }
  return StackKind::Other;
}

std::string DesktopIcons::stackName(StackKind kind) {
  switch (kind) {
  case StackKind::Images: return i18n::tr("desktop-icons.stack-images");
  case StackKind::Movies: return i18n::tr("desktop-icons.stack-movies");
  case StackKind::Music: return i18n::tr("desktop-icons.stack-music");
  case StackKind::Pdfs: return i18n::tr("desktop-icons.stack-pdfs");
  case StackKind::Documents: return i18n::tr("desktop-icons.stack-documents");
  case StackKind::Spreadsheets: return i18n::tr("desktop-icons.stack-spreadsheets");
  case StackKind::Presentations: return i18n::tr("desktop-icons.stack-presentations");
  case StackKind::Archives: return i18n::tr("desktop-icons.stack-archives");
  case StackKind::Other: return i18n::tr("desktop-icons.stack-other");
  }
  return {};
}

void DesktopIcons::layoutStacks(int rows) {
  // Folders and app shortcuts first (in sort order), then one cell per kind; an expanded stack is followed by its
  // files, and a kind with one file shows that file.
  if (m_groupsStale) {
    m_groupsStale = false;
    m_singles.clear();
    m_groups.clear();
    for (const auto& info : m_view->rows()) {
      if (info->isDir || info->name.ends_with(".desktop")) {
        m_singles.push_back(info);
      } else {
        m_groups[stackKindOf(*info)].push_back(info);
      }
    }
  }
  for (const auto& info : m_singles) {
    m_cells.push_back({.info = info});
  }
  for (const auto& [kind, files] : m_groups) {
    if (files.size() == 1) {
      m_cells.push_back({.info = files.front()});
      continue;
    }
    const auto newest = std::ranges::max_element(files, {}, [](const auto& f) { return f->mtimeNs; });
    m_cells.push_back({.info = *newest, .stack = kind, .members = files});
    if (m_expanded.contains(kind)) {
      for (const auto& file : files) {
        m_cells.push_back({.info = file});
      }
    }
  }
  for (std::size_t i = 0; i < m_cells.size(); ++i) {
    m_cells[i].slot = {.column = static_cast<int>(i) / rows, .row = static_cast<int>(i) % rows};
  }
  kLog.debug("desktop stacks on: {} cells", m_cells.size());
}

void DesktopIcons::close() {
  if (m_menu != nullptr) {
    m_menu->close();
  }
  if (DragAndDrop* dnd = m_surface != nullptr ? m_wayland->dragAndDrop() : nullptr) {
    dnd->clearTarget(m_surface->wlSurface());
  }
  m_input.setSceneRoot(nullptr);
  // The scene first: its images unload their textures through the surface's renderer.
  if (m_surface != nullptr) {
    m_surface->setSceneRoot(nullptr);
  }
  m_sceneRoot.reset();
  m_surface.reset();
  m_band = nullptr;
  m_cells.clear();
  m_output = nullptr;
  m_banding = false;
  // A drag that ends after the surface closed must not rebuild it; its icon goes with the surface.
  m_systemDrag = false;
  m_dragging = false;
  m_pressIndex.reset();
  m_dragIcon.reset();
  m_dragImage.reset();
  m_dropHighlight.reset();
}

float DesktopIcons::slotX(int column) const {
  const bool fromRight = m_config->config().desktopIcons.startCorner == DesktopIconsCorner::TopRight;
  return fromRight ? static_cast<float>(static_cast<int>(m_surface->width()) - kMargin - (column + 1) * m_cellW)
                   : static_cast<float>(kMargin + column * m_cellW);
}

float DesktopIcons::slotY(int row) const { return static_cast<float>(kMargin + row * m_cellH); }

void DesktopIcons::rebuildCells() {
  // The 64 grid is 96 x 104 cells with a 72 plate; other icon sizes keep its gaps.
  const int icon = m_config->config().desktopIcons.iconSize;
  m_plate = icon + 8;
  m_cellH = icon + 40;
  m_cellW = std::max(96, icon + 24);
  m_nameW = m_cellW - 4;
  m_cells.clear();
  if (m_view == nullptr || m_surface == nullptr || m_surface->height() == 0) {
    return;
  }
  const int rows = std::max(1, (static_cast<int>(m_surface->height()) - 2 * kMargin) / m_cellH);
  const int columns = std::max(1, (static_cast<int>(m_surface->width()) - 2 * kMargin) / m_cellW);
  if (m_config->config().desktopIcons.stacks) {
    layoutStacks(rows);
  } else {
    placeSaved(rows, columns);
  }
  const bool free = freePlacement();
  if (free) {
    kLog.debug("desktop free placement: {} cells", m_cells.size());
  }
  const bool fromRight = m_config->config().desktopIcons.startCorner == DesktopIconsCorner::TopRight;
  const auto maxX = static_cast<float>(static_cast<int>(m_surface->width()) - m_cellW);
  const auto maxY = static_cast<float>(static_cast<int>(m_surface->height()) - m_cellH);
  for (Cell& cell : m_cells) {
    cell.x = slotX(cell.slot.column);
    cell.y = slotY(cell.slot.row);
    if (const auto it = m_freeAt.find(cell.info->name); free && !cell.stack && it != m_freeAt.end()) {
      // Kept inside the surface when the output shrinks.
      cell.x = std::clamp(fromRight ? maxX - it->second.first : it->second.first, 0.0F, std::max(0.0F, maxX));
      cell.y = std::clamp(it->second.second, 0.0F, std::max(0.0F, maxY));
    }
  }
  std::set<std::string> present;
  for (const auto& info : m_view->rows()) {
    present.insert(info->name);
  }
  for (const Cell& cell : m_cells) {
    present.insert(keyOf(cell));
  }
  // A new folder is selected and renamed before the listing shows it.
  std::erase_if(m_selected, [&](const std::string& key) { return !present.contains(key) && key != m_renaming; });
  // Cell indices change with the rebuild.
  m_dropHighlight.reset();
  requestBuild();
}

void DesktopIcons::placeSaved(int rows, int columns) {
  std::set<std::pair<int, int>> taken;
  std::vector<std::shared_ptr<const FileInfo>> unplaced;
  // Saved places first; the rest fill free places column by column from the corner (more than fit go on inward).
  // A freely placed icon draws at its own point, so it keeps its nearest cell even when a grid icon holds it.
  const bool free = freePlacement();
  for (const auto& info : m_view->rows()) {
    const auto saved = m_slots.find(info->name);
    const bool fits = saved != m_slots.end() && saved->second.column < columns && saved->second.row < rows;
    if (fits && free && m_freeAt.contains(info->name)) {
      taken.insert({saved->second.column, saved->second.row});
      m_cells.push_back({.info = info, .slot = saved->second});
    } else if (fits && taken.insert({saved->second.column, saved->second.row}).second) {
      m_cells.push_back({.info = info, .slot = saved->second});
    } else {
      unplaced.push_back(info);
    }
  }
  int next = 0;
  for (const auto& info : unplaced) {
    while (taken.contains({next / rows, next % rows})) {
      ++next;
    }
    const Slot slot{.column = next / rows, .row = next % rows};
    taken.insert({slot.column, slot.row});
    m_cells.push_back({.info = info, .slot = slot});
  }
}

std::string DesktopIcons::outputKey() const {
  const auto* output = m_wayland->findOutputByWl(m_output);
  return output != nullptr ? output->connectorName : std::string{};
}

std::filesystem::path DesktopIcons::slotsFile() const {
  const char* state = std::getenv("XDG_STATE_HOME");
  const std::filesystem::path base = state != nullptr && *state != '\0'
                                         ? std::filesystem::path(state)
                                         : std::filesystem::path(g_get_home_dir()) / ".local" / "state";
  return base / "andromeda" / "desktop-icons.json";
}

void DesktopIcons::loadSlots() {
  m_slots.clear();
  m_freeAt.clear();
  std::ifstream in(slotsFile());
  const auto json = nlohmann::json::parse(in, nullptr, false);
  const auto output = json.is_object() ? json.find(outputKey()) : json.end();
  if (!json.is_object() || output == json.end() || !output->is_object()) {
    return;
  }
  for (const auto& [name, place] : output->items()) {
    // [column, row] or, for a freely placed icon, [column, row, x, y].
    if (place.is_array() && place.size() >= 2 && place[0].is_number_integer() && place[1].is_number_integer()) {
      m_slots[name] = {.column = place[0].get<int>(), .row = place[1].get<int>()};
      if (place.size() == 4 && place[2].is_number() && place[3].is_number()) {
        m_freeAt[name] = {place[2].get<float>(), place[3].get<float>()};
      }
    }
  }
}

void DesktopIcons::saveSlots() const {
  const std::filesystem::path file = slotsFile();
  std::ifstream in(file);
  auto json = nlohmann::json::parse(in, nullptr, false);
  if (!json.is_object()) {
    json = nlohmann::json::object();
  }
  auto& output = json[outputKey()];
  output = nlohmann::json::object();
  for (const auto& [name, slot] : m_slots) {
    const auto free = m_freeAt.find(name);
    output[name] = free != m_freeAt.end() ? nlohmann::json{slot.column, slot.row, free->second.first, free->second.second}
                                          : nlohmann::json{slot.column, slot.row};
  }
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  std::ofstream(file) << json.dump(2) << '\n';
}

void DesktopIcons::onCellMotion(float sceneX, float sceneY) {
  if (!m_pressIndex.has_value()) {
    return;
  }
  const float dx = sceneX - m_pressX;
  const float dy = sceneY - m_pressY;
  if (!m_dragging && std::hypot(dx, dy) < kDragSlop) {
    return;
  }
  if (!m_dragging) {
    m_dragging = true;
    startSystemDrag();
  }
  if (m_systemDrag) {
    // The compositor draws the drag image at the pointer; the originals stay dimmed in place until the drop.
    for (const std::size_t i : selection()) {
      if (m_cells[i].area != nullptr) {
        m_cells[i].area->setOpacity(kDraggedOpacity);
      }
    }
    m_surface->requestRedraw();
    return;
  }
  moveSelected(dx, dy);
}

void DesktopIcons::moveSelected(float dx, float dy) {
  // Moves the selected cells in place: a rebuild now would drop the pressed area and end the drag.
  for (const std::size_t i : selection()) {
    if (m_cells[i].area != nullptr) {
      m_cells[i].area->setPosition(m_cells[i].x + dx, m_cells[i].y + dy);
    }
  }
  m_surface->requestRedraw();
}

void DesktopIcons::drop() {
  // Stacks keep the sorted arrangement, as Finder does.
  if (m_config->config().desktopIcons.stacks) {
    rebuildCells();
    return;
  }
  const auto selected = selection();
  if (selected.empty() || selected.front() >= m_cells.size() || m_cells[selected.front()].area == nullptr) {
    return;
  }
  const InputArea& moved = *m_cells[selected.front()].area;
  const float dx = moved.x() - m_cells[selected.front()].x;
  const float dy = moved.y() - m_cells[selected.front()].y;
  const int rows = std::max(1, (static_cast<int>(m_surface->height()) - 2 * kMargin) / m_cellH);
  const int columns = std::max(1, (static_cast<int>(m_surface->width()) - 2 * kMargin) / m_cellW);
  const bool fromRight = m_config->config().desktopIcons.startCorner == DesktopIconsCorner::TopRight;
  std::set<std::pair<int, int>> taken;
  for (const Cell& cell : m_cells) {
    if (!m_selected.contains(cell.info->name)) {
      taken.insert({cell.slot.column, cell.slot.row});
    }
  }
  const bool free = freePlacement();
  const auto maxX = static_cast<float>(static_cast<int>(m_surface->width()) - m_cellW);
  const auto maxY = static_cast<float>(static_cast<int>(m_surface->height()) - m_cellH);
  // Each moved icon snaps to the nearest place; a place another icon holds keeps the moved one where it was. With
  // Snap to Grid off it stays where it was dropped.
  for (const std::size_t i : selected) {
    const Cell& cell = m_cells[i];
    if (cell.area != nullptr) {
      m_shownAt[cell.info->name] = {cell.area->x(), cell.area->y()};
    }
    if (free) {
      const float x = std::clamp(cell.x + dx, 0.0F, std::max(0.0F, maxX));
      const float y = std::clamp(cell.y + dy, 0.0F, std::max(0.0F, maxY));
      m_freeAt[cell.info->name] = {fromRight ? maxX - x : x, y};
      m_slots[cell.info->name] =
          slotAt(x + static_cast<float>(m_cellW) / 2.0F, y + static_cast<float>(m_plate) / 2.0F);
      continue;
    }
    m_freeAt.erase(cell.info->name);
    const float step = static_cast<float>(cell.slot.column) + (fromRight ? -dx : dx) / m_cellW;
    const int column = std::clamp(static_cast<int>(std::lround(step)), 0, columns - 1);
    const int row = std::clamp(static_cast<int>(std::lround(static_cast<float>(cell.slot.row) + dy / m_cellH)), 0, rows - 1);
    const Slot slot = taken.insert({column, row}).second ? Slot{.column = column, .row = row} : cell.slot;
    taken.insert({slot.column, slot.row});
    m_slots[cell.info->name] = slot;
  }
  // Every icon keeps its place from now on, so later files fill around them.
  for (const Cell& cell : m_cells) {
    m_slots.try_emplace(cell.info->name, cell.slot);
  }
  saveSlots();
  rebuildCells();
}

void DesktopIcons::glide(InputArea& area, const std::string& name, float x, float y) {
  const auto [it, fresh] = m_shownAt.try_emplace(name, x, y);
  const auto [fromX, fromY] = it->second;
  if (fresh || (std::abs(fromX - x) < 0.5F && std::abs(fromY - y) < 0.5F)) {
    it->second = {x, y};
    return;
  }
  area.setPosition(fromX, fromY);
  const desk::motion::SpringConfig move{.damping = Tokens::springMove.damping, .stiffness = Tokens::springMove.stiffness};
  InputArea* node = &area;
  m_animations.animateSpatial(fromX, x, move, [this, node, name](float v) {
    node->setPosition(v, node->y());
    m_shownAt[name].first = v;
    m_surface->requestRedraw();
  }, {}, node, 0.0F, Unit::Px);
  m_animations.animateSpatial(fromY, y, move, [this, node, name](float v) {
    node->setPosition(node->x(), v);
    m_shownAt[name].second = v;
    m_surface->requestRedraw();
  }, {}, node, 0.0F, Unit::Px);
  m_surface->requestFrameTick();
}

void DesktopIcons::requestBuild() {
  m_dirty = true;
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

std::filesystem::path DesktopIcons::pathOf(const FileInfo& info) const { return m_dir / info.name; }

std::string DesktopIcons::iconPathFor(const FileInfo& info) {
  constexpr int kIcon = 64;
  if (!info.isDir && info.name.ends_with(".desktop")) {
    if (const auto entry = loadDesktopFile(pathOf(info)); entry.has_value() && !entry->icon.empty()) {
      if (const std::string& path = m_iconResolver.resolve(entry->icon, kIcon); !path.empty()) {
        return path;
      }
    }
  }
  for (const std::string& name : mimeIconNames(info)) {
    if (const std::string& path = m_iconResolver.resolve(name, kIcon); !path.empty()) {
      return path;
    }
  }
  return {};
}

std::string DesktopIcons::labelFor(const FileInfo& info) const {
  // An app shortcut shows the app's name, as on Windows and GNOME.
  if (!info.isDir && info.name.ends_with(".desktop")) {
    // Each build asks for every label: parse a shortcut again only after it changed.
    auto& cached = m_labels[info.name];
    if (cached.first != info.mtimeNs || info.mtimeNs == 0) {
      const auto entry = loadDesktopFile(pathOf(info));
      cached = {info.mtimeNs, entry.has_value() ? entry->name : std::string{}};
    }
    if (!cached.second.empty()) {
      return cached.second;
    }
  }
  return info.displayName;
}

std::vector<std::size_t> DesktopIcons::selection() const {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < m_cells.size(); ++i) {
    if (isSelected(m_cells[i])) {
      out.push_back(i);
    }
  }
  return out;
}

void DesktopIcons::build() {
  m_dirty = false;
  Renderer& renderer = m_surface->renderTarget().renderer();
  const auto w = static_cast<float>(m_surface->width());
  const auto h = static_cast<float>(m_surface->height());
  m_renameInput = nullptr;
  m_animations.cancelAll();
  // The old icons go only after the new ones took their thumbnails, so a rebuild never unloads and decodes them again.
  std::unique_ptr<Node> previous = std::move(m_sceneRoot);
  m_sceneRoot = ui::node({});
  m_sceneRoot->setAnimationManager(&m_animations);
  m_sceneRoot->setSize(w, h);

  // Empty space: clears the selection, starts a rubber band, opens the desktop menu.
  InputArea* desk = nullptr;
  m_sceneRoot->addChild(ui::inputArea({
      .out = &desk,
      .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}),
      .width = w,
      .height = h,
      .participatesInLayout = false,
      .onMotion =
          [this](const InputArea::PointerData& data) {
            if (m_banding) {
              updateBand(data.localX, data.localY);
            }
          },
      .onPress =
          [this](const InputArea::PointerData& data) {
            if (data.button == BTN_RIGHT && data.pressed) {
              openDesktopMenu(data.localX, data.localY);
              return;
            }
            if (data.button != BTN_LEFT) {
              return;
            }
            if (data.pressed) {
              if ((m_wayland->keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Shift)) == 0) {
                m_selected.clear();
              }
              m_banding = true;
              m_bandX0 = m_bandX1 = data.localX;
              m_bandY0 = m_bandY1 = data.localY;
              syncBand();
            } else if (m_banding) {
              m_banding = false;
              requestBuild();
            }
          },
  }));

  const auto selected = [&](const Cell& cell) { return isSelected(cell); };
  (void)m_thumbnails->uploadPending(renderer.textureManager());
  m_drawsFileIcons = false;
  for (std::size_t i = 0; i < m_cells.size(); ++i) {
    const Cell& cell = m_cells[i];
    const bool on = selected(cell);
    InputArea* area = nullptr;
    m_sceneRoot->addChild(ui::inputArea({
        .out = &area,
        .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}),
        .x = cell.x,
        .y = cell.y,
        .width = static_cast<float>(m_cellW),
        .height = static_cast<float>(m_cellH),
        .participatesInLayout = false,
        .onMotion = [this](const InputArea::PointerData& data) { onCellMotion(data.sceneX, data.sceneY); },
        .onPress =
            [this, i](const InputArea::PointerData& data) {
              if (data.button == BTN_LEFT && !data.pressed) {
                const bool dragged = m_dragging;
                m_pressIndex.reset();
                m_dragging = false;
                if (dragged) {
                  drop();
                } else if (i < m_cells.size() && m_cells[i].stack
                           && (m_wayland->keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Shift)) == 0) {
                  // A click opens the stack in place; a second click folds it again.
                  const StackKind kind = *m_cells[i].stack;
                  m_expanded.erase(kind) == 0 ? (void)m_expanded.insert(kind) : (void)0;
                  kLog.debug("stack {} {}", stackName(kind), m_expanded.contains(kind) ? "expanded" : "collapsed");
                  rebuildCells();
                } else {
                  requestBuild();
                }
                return;
              }
              if (data.pressed && (data.button == BTN_LEFT || data.button == BTN_RIGHT)) {
                onCellPress(i, data.button == BTN_RIGHT);
                if (data.button == BTN_LEFT) {
                  m_pressIndex = i;
                  m_pressX = data.sceneX;
                  m_pressY = data.sceneY;
                  m_pressSerial = data.serial;
                }
                if (data.button == BTN_RIGHT) {
                  openItemMenu(i, m_cells[i].x + data.localX, m_cells[i].y + data.localY);
                }
              }
            },
    }));
    m_cells[i].area = area;
    glide(*area, keyOf(cell), cell.x, cell.y);
    const float plateX = (m_cellW - m_plate) * 0.5F;
    area->addChild(ui::box({
        .out = &m_cells[i].plate,
        .fill = colorSpecFromRole(ColorRole::Fill2),
        .radius = static_cast<float>(kPlateRound),
        .width = static_cast<float>(m_plate),
        .height = static_cast<float>(m_plate),
        .visible = on,
        .participatesInLayout = false,
        .configure = [plateX](Box& box) { box.setPosition(plateX, 0.0F); },
    }));
    const int iconSize = m_config->config().desktopIcons.iconSize;
    if (cell.stack) {
      // Two cards behind the newest file read as a pile (FILES-UI-SPEC §20).
      const float card = static_cast<float>(iconSize - 8);
      for (const auto& [offset, alpha] : {std::pair{6.0F, 0.30F}, std::pair{3.0F, 0.55F}}) {
        const float cardX = static_cast<float>(m_cellW) * 0.5F - card * 0.5F + offset;
        const float cardY = static_cast<float>(m_plate) * 0.5F - card * 0.5F - offset;
        area->addChild(ui::box({
            .fill = fixedColorSpec(Color{1.0F, 1.0F, 1.0F, alpha}),
            .radius = static_cast<float>(kPlateRound),
            .width = card,
            .height = card,
            .participatesInLayout = false,
            .configure = [cardX, cardY](Box& box) { box.setPosition(cardX, cardY); },
        }));
      }
    }
    // App shortcuts show the app's icon; every other item the Files v2 icon (folder contents, previews, page badges).
    if (!cell.info->isDir && cell.info->name.ends_with(".desktop")) {
      if (const std::string path = iconPathFor(*cell.info); !path.empty()) {
        auto icon = ui::image({
            .fit = ImageFit::Contain,
            .width = static_cast<float>(iconSize),
            .height = static_cast<float>(iconSize),
            .participatesInLayout = false,
        });
        if (icon->setSourceFile(renderer, path, iconSize, true)) {
          icon->setPosition(static_cast<float>(m_cellW - iconSize) * 0.5F, static_cast<float>(m_plate - iconSize) * 0.5F);
          area->addChild(std::move(icon));
        }
      }
    } else {
      auto icon = std::make_unique<FileIcon>(static_cast<float>(iconSize));
      icon->setParticipatesInLayout(false);
      icon->bind(renderer, *cell.info, pathOf(*cell.info), m_thumbnails);
      icon->setPosition(static_cast<float>(m_cellW - iconSize) * 0.5F, static_cast<float>(m_plate - iconSize) * 0.5F);
      area->addChild(std::move(icon));
      m_drawsFileIcons = true;
    }
    // Names: white with a soft shadow on any wallpaper; the accent pill and every line while selected.
    auto name = ui::label({
        .text = cell.stack ? stackName(*cell.stack) : labelFor(*cell.info),
        .fontSize = Style::fontSizeCaption,
        .fontWeight = FontWeight::Medium,
        .color = fixedColorSpec(Color{1.0F, 1.0F, 1.0F, 0.92F}),
        .maxWidth = static_cast<float>(m_nameW - 2 * kNamePad),
        .maxLines = on ? 4 : 2,
        .textAlign = TextAlign::Center,
        .ellipsize = TextEllipsize::End,
        .participatesInLayout = false,
    });
    name->setShadow(Color{0.0F, 0.0F, 0.0F, 0.6F}, 0.0F, 1.0F);
    name->measure(renderer);
    const float nameW = name->width() + 2 * kNamePad;
    const float nameX = std::round((m_cellW - nameW) * 0.5F);
    const float nameY = m_plate + kNameGap;
    if (on) {
      area->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Accent, 0.24F),
          .radius = static_cast<float>(kNameRound),
          .width = nameW,
          .height = name->height(),
          .participatesInLayout = false,
          .configure = [nameX, nameY](Box& box) { box.setPosition(nameX, nameY); },
      }));
    }
    if (cell.stack) {
      auto count = ui::label({
          .text = i18n::tr("desktop-icons.stack-items", "count", cell.members.size()),
          .fontSize = Style::fontSizeCaption,
          .color = fixedColorSpec(Color{1.0F, 1.0F, 1.0F, 0.70F}),
          .textAlign = TextAlign::Center,
          .participatesInLayout = false,
      });
      count->setShadow(Color{0.0F, 0.0F, 0.0F, 0.6F}, 0.0F, 1.0F);
      count->measure(renderer);
      count->setPosition(std::round((static_cast<float>(m_cellW) - count->width()) * 0.5F), nameY + name->height());
      area->addChild(std::move(count));
    }
    if (m_renaming == cell.info->name && !cell.stack) {
      // Inline rename: the field covers the name; the name without its extension starts selected (Finder).
      const std::string current = cell.info->displayName;
      const std::size_t dot = current.rfind('.');
      const std::size_t stem = dot == std::string::npos || dot == 0 || cell.info->isDir ? current.size() : dot;
      const bool fresh = !m_renameText.has_value();
      if (fresh) {
        m_renameText = current;
      }
      area->addChild(ui::input({
          .out = &m_renameInput,
          .value = *m_renameText,
          .fontSize = Style::fontSizeCaption,
          .textAlign = TextAlign::Center,
          .width = static_cast<float>(m_nameW),
          .participatesInLayout = false,
          .onChange = [this](const std::string& text) { m_renameText = text; },
          .onSubmit = [this](const std::string& text) { commitRename(text); },
          .submitOnFocusLoss = true,
          .configure = [this, nameY, stem, fresh](Input& field) {
            field.setPosition(static_cast<float>(m_cellW - m_nameW) * 0.5F, nameY);
            if (fresh) {
              field.selectRange(0, stem);
            }
          },
      }));
    } else {
      name->setPosition(nameX + kNamePad, nameY);
      area->addChild(std::move(name));
    }
  }

  // Rubber band: accent 12 % fill, 1 px accent 40 % edge (mockup section 2); moved in place while it drags.
  m_sceneRoot->addChild(ui::box({
      .out = &m_band,
      .fill = colorSpecFromRole(ColorRole::Accent, 0.12F),
      .border = colorSpecFromRole(ColorRole::Accent, 0.40F),
      .borderWidth = 1.0F,
      .radius = static_cast<float>(kBandRound),
      .visible = false,
      .participatesInLayout = false,
  }));
  // Decoration only: a band still shown before the rebuild must not take the next press on an icon.
  m_band->setHitTestVisible(false);
  syncBand();

  for (const auto& child : m_sceneRoot->children()) {
    child->layout(renderer);
  }
  // Before the new tree takes input: an old rename field going after the new one has focus would drop its focus.
  previous.reset();
  m_input.setSceneRoot(m_sceneRoot.get());
  m_surface->setSceneRoot(m_sceneRoot.get());
  if (m_renaming.has_value() && m_renameInput != nullptr) {
    m_input.setFocus(m_renameInput->inputArea());
  }
}

void DesktopIcons::commitRename(const std::string& text) {
  if (!m_renaming.has_value()) {
    return;
  }
  const std::string from = *m_renaming;
  m_renaming.reset();
  m_renameInput = nullptr;
  if (!text.empty() && text != from && text.find('/') == std::string::npos && text != "." && text != "..") {
    std::error_code error;
    if (std::filesystem::exists(m_dir / text, error)) {
      kLog.info("rename: {} already exists", text);
    } else {
      std::filesystem::rename(m_dir / from, m_dir / text, error);
      if (error) {
        kLog.warn("renaming {} failed: {}", from, error.message());
      } else {
        if (const auto slot = m_slots.find(from); slot != m_slots.end()) {
          m_slots[text] = slot->second;
          m_slots.erase(slot);
          saveSlots();
        }
        m_selected = {text};
      }
    }
  }
  requestBuild();
}

void DesktopIcons::updateBand(float x, float y) {
  m_bandX1 = x;
  m_bandY1 = y;
  const float left = std::min(m_bandX0, m_bandX1);
  const float right = std::max(m_bandX0, m_bandX1);
  const float top = std::min(m_bandY0, m_bandY1);
  const float bottom = std::max(m_bandY0, m_bandY1);
  const auto hit = [&](const Cell& cell) {
    return cell.x < right && cell.x + m_cellW > left && cell.y < bottom && cell.y + m_cellH > top;
  };
  // Clear first, then select: an open stack and its files are separate cells over the same names.
  if ((m_wayland->keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Shift)) == 0) {
    for (const Cell& cell : m_cells) {
      if (!hit(cell)) {
        selectCell(cell, false);
      }
    }
  }
  for (const Cell& cell : m_cells) {
    if (hit(cell)) {
      selectCell(cell, true);
    }
  }
  for (const Cell& cell : m_cells) {
    if (cell.plate != nullptr) {
      cell.plate->setVisible(isSelected(cell));
    }
  }
  syncBand();
}

void DesktopIcons::syncBand() {
  if (m_band == nullptr || m_surface == nullptr) {
    return;
  }
  m_band->setVisible(m_banding);
  m_band->setPosition(std::min(m_bandX0, m_bandX1), std::min(m_bandY0, m_bandY1));
  m_band->setSize(std::abs(m_bandX1 - m_bandX0), std::abs(m_bandY1 - m_bandY0));
  m_surface->requestRedraw();
}

void DesktopIcons::selectOnly(std::size_t index) {
  m_selected.clear();
  selectCell(m_cells[index], true);
  m_anchor = index;
}

void DesktopIcons::onCellPress(std::size_t index, bool rightButton) {
  if (index >= m_cells.size()) {
    return;
  }
  const Cell& pressed = m_cells[index];
  const std::uint32_t mods = m_wayland->keyboardModifiers();
  kLog.debug("desktop press {}", keyOf(pressed));
  if (rightButton) {
    if (!isSelected(pressed)) {
      selectOnly(index);
    }
  } else if ((mods & KeyMod::Ctrl) != 0) {
    selectCell(pressed, !isSelected(pressed));
    m_anchor = index;
  } else if ((mods & KeyMod::Shift) != 0 && m_anchor.has_value() && *m_anchor < m_cells.size()) {
    const auto [lo, hi] = std::minmax(*m_anchor, index);
    for (std::size_t i = lo; i <= hi; ++i) {
      selectCell(m_cells[i], true);
    }
  } else {
    const auto now = std::chrono::steady_clock::now();
    const bool second = m_lastClick == keyOf(pressed) && now - m_lastClickAt < kDoubleClick;
    m_lastClick = second ? std::nullopt : std::optional(keyOf(pressed));
    m_lastClickAt = now;
    selectOnly(index);
    // A stack opens on click (release), so a double click on it does not open every file.
    if (second && !pressed.stack) {
      activate({pathOf(*pressed.info)});
    }
  }
  if (rightButton) {
    requestBuild();
    return;
  }
  // The plates change in place; the name pills follow on release (a rebuild now would end a drag).
  for (const Cell& cell : m_cells) {
    if (cell.plate != nullptr) {
      cell.plate->setVisible(isSelected(cell));
    }
  }
  m_surface->requestRedraw();
}

void DesktopIcons::activate(const std::vector<std::filesystem::path>& paths) {
  for (const auto& path : paths) {
    if (path.extension() == ".desktop" && !std::filesystem::is_directory(path)) {
      if (const auto entry = loadDesktopFile(path); entry.has_value()) {
        (void)desktop_entry_launch::launchEntry(*entry);
        continue;
      }
    }
    if (!net::openInBrowser(fileUri(path))) {
      kLog.warn("opening {} failed", path.string());
    }
  }
}

void DesktopIcons::trash(const std::vector<std::filesystem::path>& paths) {
  for (const auto& path : paths) {
    GFile* file = g_file_new_for_path(path.c_str());
    GError* error = nullptr;
    if (!g_file_trash(file, nullptr, &error)) {
      kLog.warn("moving {} to Trash failed: {}", path.string(), error != nullptr ? error->message : "");
      g_clear_error(&error);
    }
    g_object_unref(file);
  }
}

void DesktopIcons::openDesktopMenu(float x, float y) {
  m_selected.clear();
  requestBuild();
  const FileSortKey key = m_view != nullptr ? m_view->sort().key : FileSortKey::Name;
  const int iconSize = m_config->config().desktopIcons.iconSize;
  std::vector<ContextMenuControlEntry> entries{
      {.id = NewFolder, .label = i18n::tr("desktop-icons.new-folder")},
      {.id = NewDocument, .label = i18n::tr("desktop-icons.new-document")},
      {.separator = true},
      {.id = Paste, .label = i18n::tr("desktop-icons.paste"), .enabled = canPaste()},
      {.separator = true},
      {.label = i18n::tr("desktop-icons.sort-by"), .header = true},
      {.id = SortName, .label = i18n::tr("desktop-icons.sort-name"), .radio = true,
       .toggleState = key == FileSortKey::Name ? 1 : 0, .indentLevel = 1},
      {.id = SortKind, .label = i18n::tr("desktop-icons.sort-kind"), .radio = true,
       .toggleState = key == FileSortKey::Kind ? 1 : 0, .indentLevel = 1},
      {.id = SortDate, .label = i18n::tr("desktop-icons.sort-date"), .radio = true,
       .toggleState = key == FileSortKey::Modified ? 1 : 0, .indentLevel = 1},
      {.id = UseStacks, .label = i18n::tr("desktop-icons.use-stacks"), .checkmark = true,
       .toggleState = m_config->config().desktopIcons.stacks ? 1 : 0},
      {.id = CleanUp, .label = i18n::tr("desktop-icons.clean-up"), .enabled = !m_config->config().desktopIcons.stacks},
      {.id = SnapToGrid, .label = i18n::tr("desktop-icons.snap-to-grid"), .enabled = !m_config->config().desktopIcons.stacks,
       .checkmark = true, .toggleState = m_config->config().desktopIcons.snapToGrid ? 1 : 0},
      {.separator = true},
      {.label = i18n::tr("desktop-icons.icon-size"), .header = true},
      {.id = IconSmall, .label = i18n::tr("desktop-icons.icon-small"), .radio = true,
       .toggleState = iconSize == 48 ? 1 : 0, .indentLevel = 1},
      {.id = IconMedium, .label = i18n::tr("desktop-icons.icon-medium"), .radio = true,
       .toggleState = iconSize == 64 ? 1 : 0, .indentLevel = 1},
      {.id = IconLarge, .label = i18n::tr("desktop-icons.icon-large"), .radio = true,
       .toggleState = iconSize == 80 ? 1 : 0, .indentLevel = 1},
      {.separator = true},
      {.id = ChangeWallpaper, .label = i18n::tr("desktop-icons.change-wallpaper")},
  };
  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
  }
  m_menu->setShadowConfig(m_config->config().shell.shadow);
  m_menu->setOnActivate([this, x, y](const ContextMenuControlEntry& entry) {
    const auto sortBy = [this](FileSortKey key) {
      // Sorting re-arranges every icon, as Finder's Sort By does.
      m_slots.clear();
      m_freeAt.clear();
      saveSlots();
      m_groupsStale = true;
      m_view->setSort({.key = key, .descending = key == FileSortKey::Modified, .foldersFirst = true});
    };
    switch (entry.id) {
    case NewFolder:
      createNew(true, std::pair{x, y});
      break;
    case NewDocument:
      createNew(false, std::pair{x, y});
      break;
    case Paste:
      paste(m_dir, std::pair{x, y});
      break;
    case SortName:
      sortBy(FileSortKey::Name);
      break;
    case SortKind:
      sortBy(FileSortKey::Kind);
      break;
    case SortDate:
      sortBy(FileSortKey::Modified);
      break;
    case ChangeWallpaper:
      PanelManager::instance().openPanel("wallpaper");
      break;
    case IconSmall:
    case IconMedium:
    case IconLarge: {
      const std::int64_t size = entry.id == IconSmall ? 48 : entry.id == IconMedium ? 64 : 80;
      (void)m_config->setOverride({"desktop_icons", "icon_size"}, size);
      break;
    }
    case SnapToGrid:
      (void)m_config->setOverride({"desktop_icons", "snap_to_grid"}, !m_config->config().desktopIcons.snapToGrid);
      break;
    case UseStacks:
      m_expanded.clear();
      (void)m_config->setOverride({"desktop_icons", "stacks"}, !m_config->config().desktopIcons.stacks);
      break;
    case CleanUp:
      // Forgets the arranged places: every icon goes back into sort order from the corner.
      m_slots.clear();
      m_freeAt.clear();
      saveSlots();
      rebuildCells();
      break;
    default:
      break;
    }
  });
  m_menu->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .minMenuWidth = static_cast<float>(kMenuW),
      .maxVisible = 24, // every row shows: a scrolling desktop menu hides Change Wallpaper
      .anchor = PopupAnchorRect{.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)},
      .parent = PopupSurfaceParent{.layerSurface = m_surface->layerSurface(), .output = m_output},
      .pointerParentSurface = m_surface->wlSurface(),
  });
}

void DesktopIcons::openItemMenu(std::size_t index, float x, float y) {
  const bool single = selection().size() == 1 && !m_cells[selection().front()].stack;
  std::vector<ContextMenuControlEntry> entries{{.id = Open, .label = i18n::tr("desktop-icons.open")}};
  // Open With: the apps registered for the file's type, the default first (menus flatten submenus into captions).
  std::vector<GAppInfo*> apps;
  const FileInfo& info = *m_cells[index].info;
  if (single && !info.name.ends_with(".desktop") && !info.contentType.empty()) {
    GList* list = g_app_info_get_all_for_type(info.contentType.c_str());
    for (GList* it = list; it != nullptr && apps.size() < kOpenWithMax; it = it->next) {
      auto* app = G_APP_INFO(it->data);
      if (g_app_info_should_show(app)) {
        apps.push_back(G_APP_INFO(g_object_ref(app)));
      }
    }
    g_list_free_full(list, g_object_unref);
  }
  if (!apps.empty()) {
    entries.push_back({.label = i18n::tr("desktop-icons.open-with"), .header = true});
    for (std::size_t i = 0; i < apps.size(); ++i) {
      entries.push_back({.id = static_cast<std::int32_t>(OpenWithFirst + i), .label = g_app_info_get_name(apps[i]),
                         .indentLevel = 1});
    }
  }
  entries.push_back({.separator = true});
  entries.push_back({.id = Cut, .label = i18n::tr("desktop-icons.cut")});
  entries.push_back({.id = Copy, .label = i18n::tr("desktop-icons.copy")});
  if (info.isDir && single) {
    entries.push_back({.id = Paste, .label = i18n::tr("desktop-icons.paste-into-folder"), .enabled = canPaste()});
  }
  entries.push_back({.separator = true});
  entries.push_back({.id = Rename, .label = i18n::tr("desktop-icons.rename"), .enabled = single});
  entries.push_back({.id = Trash, .label = i18n::tr("desktop-icons.move-to-trash")});
  entries.push_back({.separator = true});
  entries.push_back({.id = Properties, .label = i18n::tr("desktop-icons.properties")});
  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
  }
  m_menu->setShadowConfig(m_config->config().shell.shadow);
  auto openWith = std::shared_ptr<std::vector<GAppInfo*>>(new std::vector<GAppInfo*>(std::move(apps)), [](auto* list) {
    for (GAppInfo* app : *list) {
      g_object_unref(app);
    }
    delete list;
  });
  m_menu->setOnActivate([this, openWith, folder = pathOf(info)](const ContextMenuControlEntry& entry) {
    if (entry.id >= OpenWithFirst && static_cast<std::size_t>(entry.id - OpenWithFirst) < openWith->size()) {
      openWithApp((*openWith)[static_cast<std::size_t>(entry.id - OpenWithFirst)], selectedPaths());
    } else if (entry.id == Cut || entry.id == Copy) {
      copyToClipboard(selectedPaths(), entry.id == Cut);
    } else if (entry.id == Paste) {
      paste(folder, std::nullopt);
    } else if (entry.id == Properties) {
      showProperties(selectedPaths());
    } else if (entry.id == Open) {
      activate(selectedPaths());
    } else if (entry.id == Trash) {
      trash(selectedPaths());
    } else if (entry.id == Rename && selection().size() == 1 && !m_cells[selection().front()].stack) {
      m_renaming = m_cells[selection().front()].info->name;
      m_renameText.reset();
      requestBuild();
    }
  });
  m_menu->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .minMenuWidth = static_cast<float>(kMenuW),
      .maxVisible = 20,
      .anchor = PopupAnchorRect{.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)},
      .parent = PopupSurfaceParent{.layerSurface = m_surface->layerSurface(), .output = m_output},
      .pointerParentSurface = m_surface->wlSurface(),
  });
}

bool DesktopIcons::onPointerEvent(const PointerEvent& event) {
  if (m_menu != nullptr && m_menu->isOpen()) {
    if (m_menu->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_menu->close();
    }
  }
  if (!ownsSurface(event.surface)) {
    if (event.type == PointerEvent::Type::Leave && m_banding) {
      m_banding = false;
      requestBuild();
    }
    return false;
  }
  const auto x = static_cast<float>(event.sx);
  const auto y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_input.pointerEnter(x, y, event.serial);
    return true;
  case PointerEvent::Type::Leave:
    m_input.pointerLeave();
    return true;
  case PointerEvent::Type::Motion:
    m_input.pointerMotion(x, y, event.serial);
    return true;
  case PointerEvent::Type::Button:
    m_input.pointerMotion(x, y, event.serial);
    m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    return true;
  case PointerEvent::Type::Axis:
    return true;
  }
  return false;
}

void DesktopIcons::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_renaming.has_value()) {
    if (event.pressed && event.sym == XKB_KEY_Escape) {
      m_renaming.reset();
      requestBuild();
      return;
    }
    m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    return;
  }
  // Ctrl+Z undoes the last desktop copy or move, also when it emptied the desktop.
  if (event.pressed && (event.sym == XKB_KEY_z || event.sym == XKB_KEY_Z) && (event.modifiers & KeyMod::Ctrl) != 0) {
    if (m_fileOps != nullptr && !m_fileOps->undo()) {
      kLog.debug("desktop undo: nothing to undo");
    }
    return;
  }
  if (event.pressed) {
    kLog.debug("desktop key {:#x} with {} selected", event.sym, m_selected.size());
  }
  if (event.pressed && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N) && (event.modifiers & KeyMod::Ctrl) != 0
      && (event.modifiers & KeyMod::Shift) != 0) {
    createNew(true, std::nullopt);
    return;
  }
  if (event.pressed && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_V) && (event.modifiers & KeyMod::Ctrl) != 0) {
    paste(m_dir, std::nullopt);
    return;
  }
  if (!event.pressed || m_cells.empty()) {
    return;
  }
  switch (event.sym) {
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    activate(selectedPaths());
    break;
  case XKB_KEY_Delete:
    trash(selectedPaths());
    break;
  case XKB_KEY_F2:
    if (const auto selected = selection(); selected.size() == 1 && !m_cells[selected.front()].stack) {
      m_renaming = m_cells[selected.front()].info->name;
      m_renameText.reset();
    }
    break;
  case XKB_KEY_Escape:
    m_selected.clear();
    break;
  case XKB_KEY_a:
  case XKB_KEY_A:
    if ((event.modifiers & KeyMod::Ctrl) != 0) {
      for (const Cell& cell : m_cells) {
        selectCell(cell, true);
      }
    }
    break;
  case XKB_KEY_c:
  case XKB_KEY_C:
  case XKB_KEY_x:
  case XKB_KEY_X:
    if ((event.modifiers & KeyMod::Ctrl) != 0 && !m_selected.empty()) {
      copyToClipboard(selectedPaths(), event.sym == XKB_KEY_x || event.sym == XKB_KEY_X);
    }
    return;
  default:
    return;
  }
  requestBuild();
}

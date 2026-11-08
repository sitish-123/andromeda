#include "shell/desktop/desktop_icons.h"

#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
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
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/drag_and_drop.h"
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
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr Logger kLog("desktop-icons");

  // DESIGN.md "Desktop icons": 96 x 104 cells 16 from the usable-area edge; icon on a 72 plate (r 8) with the name 4
  // below, 2 lines (all lines while selected) in a 92 wide pill (r 4).
  constexpr int kMargin = 16;
  // Thumbnail long edge: the 80 icon at scale 2.
  constexpr int kThumbPx = 160;
  constexpr int kPlateRound = 8;
  constexpr int kNameGap = 4;
  constexpr int kNamePad = 4;
  constexpr int kNameRound = 4;
  constexpr int kBandRound = 4;
  constexpr int kMenuW = 220;
  constexpr float kDragSlop = 8.0F; // rulebook 7.7
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
  };

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

DesktopIcons::~DesktopIcons() { close(); }

void DesktopIcons::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
                              ThumbnailService& thumbnails) {
  m_wayland = &wayland;
  m_config = &config;
  m_renderContext = &renderContext;
  m_thumbnails = &thumbnails;
  m_thumbnailUploads = thumbnails.subscribePendingUpload([this] {
    if (!m_thumbPaths.empty()) {
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
    m_view->setChangedCallback([this] { rebuildCells(); });
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
        .motion = [](float, float) -> std::uint32_t { return DragAndDrop::Copy; },
        .leave = {},
        .drop = [this](float, float, std::uint32_t, const std::string&, std::string data) { copyIn(data); },
    });
  }
  loadSlots();
}

void DesktopIcons::copyIn(const std::string& uriList) {
  std::error_code error;
  std::filesystem::create_directories(m_dir, error);
  std::istringstream lines(uriList);
  std::set<std::filesystem::path> picked;
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    gchar* file = g_filename_from_uri(line.c_str(), nullptr, nullptr);
    if (file == nullptr) {
      continue;
    }
    const std::filesystem::path source = file;
    g_free(file);
    if (source.parent_path() == m_dir) {
      continue;
    }
    // Finder names a clash "name 2.ext".
    std::filesystem::path target = m_dir / source.filename();
    // Copies run async, so a name picked earlier in this drop counts as taken before its file exists.
    for (int n = 2; std::filesystem::exists(target, error) || picked.contains(target); ++n) {
      const bool dir = std::filesystem::is_directory(source, error);
      const std::string stem = dir ? source.filename().string() : source.stem().string();
      const std::string ext = dir ? std::string{} : source.extension().string();
      target = m_dir / std::format("{} {}{}", stem, n, ext);
    }
    picked.insert(target);
    // cp runs detached so a large copy never blocks the shell; the directory watcher shows the result.
    const std::string sourceArg = source.string();
    const std::string targetArg = target.string();
    const gchar* argv[] = {"cp", "-r", "--no-target-directory", "--", sourceArg.c_str(), targetArg.c_str(), nullptr};
    GError* spawnError = nullptr;
    if (!g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &spawnError)) {
      kLog.warn("copying {} to the desktop failed: {}", sourceArg, spawnError != nullptr ? spawnError->message : "");
      g_clear_error(&spawnError);
    }
  }
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
  releaseThumbnails({});
}

void DesktopIcons::releaseThumbnails(const std::set<std::string>& keep) {
  for (auto it = m_thumbPaths.begin(); it != m_thumbPaths.end();) {
    if (keep.contains(*it)) {
      ++it;
      continue;
    }
    m_thumbnails->release(*it, kThumbPx);
    it = m_thumbPaths.erase(it);
  }
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
  std::set<std::pair<int, int>> taken;
  std::vector<std::shared_ptr<const FileInfo>> unplaced;
  // Saved places first; the rest fill free places column by column from the corner (more than fit go on inward).
  for (const auto& info : m_view->rows()) {
    const auto saved = m_slots.find(info->name);
    if (saved != m_slots.end() && saved->second.column < columns && saved->second.row < rows
        && taken.insert({saved->second.column, saved->second.row}).second) {
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
  for (Cell& cell : m_cells) {
    cell.x = slotX(cell.slot.column);
    cell.y = slotY(cell.slot.row);
  }
  std::erase_if(m_selected, [&](const std::string& name) {
    return std::ranges::none_of(m_cells, [&](const Cell& cell) { return cell.info->name == name; });
  });
  requestBuild();
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
  std::ifstream in(slotsFile());
  const auto json = nlohmann::json::parse(in, nullptr, false);
  const auto output = json.is_object() ? json.find(outputKey()) : json.end();
  if (!json.is_object() || output == json.end() || !output->is_object()) {
    return;
  }
  for (const auto& [name, place] : output->items()) {
    if (place.is_array() && place.size() == 2 && place[0].is_number_integer() && place[1].is_number_integer()) {
      m_slots[name] = {.column = place[0].get<int>(), .row = place[1].get<int>()};
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
    output[name] = {slot.column, slot.row};
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
  m_dragging = true;
  // Moves the selected cells in place: a rebuild now would drop the pressed area and end the drag.
  for (const std::size_t i : selection()) {
    if (m_cells[i].area != nullptr) {
      m_cells[i].area->setPosition(m_cells[i].x + dx, m_cells[i].y + dy);
    }
  }
  m_surface->requestRedraw();
}

void DesktopIcons::drop() {
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
  // Each moved icon snaps to the nearest place; a place another icon holds keeps the moved one where it was.
  for (const std::size_t i : selected) {
    const Cell& cell = m_cells[i];
    if (cell.area != nullptr) {
      m_shownAt[cell.info->name] = {cell.area->x(), cell.area->y()};
    }
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

std::vector<std::size_t> DesktopIcons::selection() const {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < m_cells.size(); ++i) {
    if (m_selected.contains(m_cells[i].info->name)) {
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

  const auto selected = [&](const Cell& cell) { return m_selected.contains(cell.info->name); };
  (void)m_thumbnails->uploadPending(renderer.textureManager());
  std::set<std::string> shown;
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
                }
                if (data.button == BTN_RIGHT) {
                  openItemMenu(i, m_cells[i].x + data.localX, m_cells[i].y + data.localY);
                }
              }
            },
    }));
    m_cells[i].area = area;
    glide(*area, cell.info->name, cell.x, cell.y);
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
    const std::string& type = cell.info->contentType;
    const bool previewable = !cell.info->isDir
        && (type.starts_with("image/") || type.starts_with("video/") || type == "application/pdf");
    TextureHandle thumb{};
    if (previewable) {
      const std::string file = pathOf(*cell.info).string();
      shown.insert(file);
      if (m_thumbPaths.insert(file).second) {
        thumb = m_thumbnails->acquire(file, kThumbPx);
      } else {
        thumb = m_thumbnails->peek(file, kThumbPx);
      }
    }
    if (thumb.valid()) {
      auto image = ui::image({
          .fit = ImageFit::Contain,
          .width = static_cast<float>(iconSize),
          .height = static_cast<float>(iconSize),
          .participatesInLayout = false,
      });
      image->setExternalTexture(renderer, thumb);
      image->setPosition(static_cast<float>(m_cellW - iconSize) * 0.5F, static_cast<float>(m_plate - iconSize) * 0.5F);
      area->addChild(std::move(image));
    } else if (const std::string path = iconPathFor(*cell.info); !path.empty()) {
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
    // Names: white with a soft shadow on any wallpaper; the accent pill and every line while selected.
    auto name = ui::label({
        .text = cell.info->displayName,
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
    if (m_renaming == cell.info->name) {
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
  releaseThumbnails(shown);

  for (const auto& child : m_sceneRoot->children()) {
    child->layout(renderer);
  }
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
  for (const Cell& cell : m_cells) {
    const bool hit = cell.x < right && cell.x + m_cellW > left && cell.y < bottom && cell.y + m_cellH > top;
    if (hit) {
      m_selected.insert(cell.info->name);
    } else if ((m_wayland->keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Shift)) == 0) {
      m_selected.erase(cell.info->name);
    }
  }
  for (const Cell& cell : m_cells) {
    if (cell.plate != nullptr) {
      cell.plate->setVisible(m_selected.contains(cell.info->name));
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
  m_selected = {m_cells[index].info->name};
  m_anchor = index;
}

void DesktopIcons::onCellPress(std::size_t index, bool rightButton) {
  if (index >= m_cells.size()) {
    return;
  }
  const std::string& name = m_cells[index].info->name;
  const std::uint32_t mods = m_wayland->keyboardModifiers();
  if (rightButton) {
    if (!m_selected.contains(name)) {
      selectOnly(index);
    }
  } else if ((mods & KeyMod::Ctrl) != 0) {
    if (!m_selected.erase(name)) {
      m_selected.insert(name);
    }
    m_anchor = index;
  } else if ((mods & KeyMod::Shift) != 0 && m_anchor.has_value() && *m_anchor < m_cells.size()) {
    const auto [lo, hi] = std::minmax(*m_anchor, index);
    for (std::size_t i = lo; i <= hi; ++i) {
      m_selected.insert(m_cells[i].info->name);
    }
  } else {
    const auto now = std::chrono::steady_clock::now();
    const bool second = m_lastClick == index && now - m_lastClickAt < kDoubleClick;
    m_lastClick = second ? std::nullopt : std::optional(index);
    m_lastClickAt = now;
    selectOnly(index);
    if (second) {
      activate({index});
    }
  }
  if (rightButton) {
    requestBuild();
    return;
  }
  // The plates change in place; the name pills follow on release (a rebuild now would end a drag).
  for (const Cell& cell : m_cells) {
    if (cell.plate != nullptr) {
      cell.plate->setVisible(m_selected.contains(cell.info->name));
    }
  }
  m_surface->requestRedraw();
}

void DesktopIcons::activate(const std::vector<std::size_t>& indices) {
  for (const std::size_t i : indices) {
    const FileInfo& info = *m_cells[i].info;
    if (!info.isDir && info.name.ends_with(".desktop")) {
      if (const auto entry = loadDesktopFile(pathOf(info)); entry.has_value()) {
        (void)desktop_entry_launch::launchEntry(*entry);
        continue;
      }
    }
    if (!net::openInBrowser(fileUri(pathOf(info)))) {
      kLog.warn("opening {} failed", pathOf(info).string());
    }
  }
}

void DesktopIcons::trash(const std::vector<std::size_t>& indices) {
  for (const std::size_t i : indices) {
    GFile* file = g_file_new_for_path(pathOf(*m_cells[i].info).c_str());
    GError* error = nullptr;
    if (!g_file_trash(file, nullptr, &error)) {
      kLog.warn("moving {} to Trash failed: {}", pathOf(*m_cells[i].info).string(), error != nullptr ? error->message : "");
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
      {.separator = true},
      {.label = i18n::tr("desktop-icons.sort-by"), .header = true},
      {.id = SortName, .label = i18n::tr("desktop-icons.sort-name"), .radio = true,
       .toggleState = key == FileSortKey::Name ? 1 : 0, .indentLevel = 1},
      {.id = SortKind, .label = i18n::tr("desktop-icons.sort-kind"), .radio = true,
       .toggleState = key == FileSortKey::Kind ? 1 : 0, .indentLevel = 1},
      {.id = SortDate, .label = i18n::tr("desktop-icons.sort-date"), .radio = true,
       .toggleState = key == FileSortKey::Modified ? 1 : 0, .indentLevel = 1},
      {.id = CleanUp, .label = i18n::tr("desktop-icons.clean-up")},
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
  m_menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
    const auto sortBy = [this](FileSortKey key) {
      // Sorting re-arranges every icon, as Finder's Sort By does.
      m_slots.clear();
      saveSlots();
      m_view->setSort({.key = key, .descending = key == FileSortKey::Modified, .foldersFirst = true});
    };
    switch (entry.id) {
    case NewFolder: {
      std::filesystem::path path = m_dir / i18n::tr("desktop-icons.untitled-folder");
      for (int n = 2; std::filesystem::exists(path); ++n) {
        path = m_dir / std::format("{} {}", i18n::tr("desktop-icons.untitled-folder"), n);
      }
      std::error_code error;
      std::filesystem::create_directories(path, error);
      break;
    }
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
    case CleanUp:
      // Forgets the arranged places: every icon goes back into sort order from the corner.
      m_slots.clear();
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
      .maxVisible = 16,
      .anchor = PopupAnchorRect{.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)},
      .parent = PopupSurfaceParent{.layerSurface = m_surface->layerSurface(), .output = m_output},
      .pointerParentSurface = m_surface->wlSurface(),
  });
}

void DesktopIcons::openItemMenu(std::size_t index, float x, float y) {
  (void)index;
  std::vector<ContextMenuControlEntry> entries{
      {.id = Open, .label = i18n::tr("desktop-icons.open")},
      {.id = Rename, .label = i18n::tr("desktop-icons.rename"), .enabled = selection().size() == 1},
      {.separator = true},
      {.id = Trash, .label = i18n::tr("desktop-icons.move-to-trash")},
  };
  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
  }
  m_menu->setShadowConfig(m_config->config().shell.shadow);
  m_menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
    if (entry.id == Open) {
      activate(selection());
    } else if (entry.id == Trash) {
      trash(selection());
    } else if (entry.id == Rename && selection().size() == 1) {
      m_renaming = m_cells[selection().front()].info->name;
      m_renameText.reset();
      requestBuild();
    }
  });
  m_menu->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .minMenuWidth = static_cast<float>(kMenuW),
      .maxVisible = 8,
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
  if (!event.pressed || m_cells.empty()) {
    return;
  }
  switch (event.sym) {
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    activate(selection());
    break;
  case XKB_KEY_Delete:
    trash(selection());
    break;
  case XKB_KEY_F2:
    if (const auto selected = selection(); selected.size() == 1) {
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
        m_selected.insert(cell.info->name);
      }
    }
    break;
  default:
    return;
  }
  requestBuild();
}

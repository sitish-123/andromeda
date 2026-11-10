#include "shell/wallpaper/panel/wallpaper_panel.h"

#include "config/config_service.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/random.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/accessible.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "core/deferred_call.h"
#include "shell/panel/panel_manager.h"
#include "shell/wallpaper/wallpaper_window.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/wallpaper/panel/wallpaper_tile.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "theme/theme_service.h"
#include "ui/builders.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr)
      return false;
    for (const Node* p = node->parent(); p != nullptr; p = p->parent()) {
      if (p == ancestor)
        return true;
    }
    return false;
  }

  constexpr Logger kLog("wp-panel");
  constexpr auto kFilterDebounceInterval = std::chrono::milliseconds(120);
  constexpr float kMinTileWidth = 180.0F;
  // Search by file name only needs room for a short name (macOS Finder search field).
  constexpr float kSearchWidth = 240.0F;
  constexpr float kPreviewWidth = 192.0F;  // 16:10, on the 4 grid
  constexpr float kPreviewHeight = 120.0F;
  constexpr auto kLoadingDelay = std::chrono::milliseconds(200); // rulebook 9.13: no spinner for fast scans
  constexpr float kTileAspect = 0.78F; // height / width, leaves room for label under widescreen thumb

  [[nodiscard]] std::optional<ThemeMode> favoriteThemeBadge(const WallpaperFavorite* favorite) {
    if (favorite == nullptr) {
      return std::nullopt;
    }
    if (favorite->themeMode == ThemeMode::Light || favorite->themeMode == ThemeMode::Dark) {
      return favorite->themeMode;
    }
    return std::nullopt;
  }

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  std::string colorWallpaperPath(const Color& color) { return "color:" + formatRgbHex(color); }

  bool wallpaperEntryIsCurrent(const WallpaperEntry& entry, std::string_view currentWallpaperPath) {
    if (entry.isDir) {
      return false;
    }
    return FileUtils::normalizeWallpaperPath(entry.absPath.string())
        == FileUtils::normalizeWallpaperPath(currentWallpaperPath);
  }

  [[nodiscard]] WallpaperFavorite wallpaperFavoriteFromTheme(const ThemeConfig& theme) {
    WallpaperFavorite favorite;
    favorite.themeMode = theme.mode;
    favorite.paletteSource = theme.source;
    favorite.builtinPalette = theme.builtinPalette;
    favorite.customPalette = theme.customPalette;
    favorite.wallpaperScheme = theme.wallpaperScheme;
    return favorite;
  }

  [[nodiscard]] std::filesystem::path canonicalWallpaperDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) {
      return {};
    }
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(dir, ec);
    return ec ? dir.lexically_normal() : canonical;
  }

  [[nodiscard]] bool wallpaperDirectoriesEquivalent(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) {
      return a.empty() && b.empty();
    }
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) {
      return true;
    }
    return canonicalWallpaperDirectory(a) == canonicalWallpaperDirectory(b);
  }

  [[nodiscard]] bool
  wallpaperPathInsideDirectory(const std::filesystem::path& file, const std::filesystem::path& directory) {
    const std::filesystem::path dir = canonicalWallpaperDirectory(directory);
    if (dir.empty()) {
      return false;
    }
    std::error_code ec;
    const auto filePath = std::filesystem::weakly_canonical(file, ec);
    if (ec) {
      return false;
    }
    const auto rel = std::filesystem::relative(filePath.parent_path(), dir, ec);
    if (ec || rel.empty()) {
      return wallpaperDirectoriesEquivalent(filePath.parent_path(), dir);
    }
    const std::string relText = rel.generic_string();
    return !relText.starts_with("..");
  }

  [[nodiscard]] int caseInsensitiveNameOrder(std::string_view a, std::string_view b) {
    return StringUtils::naturalCaseInsensitiveCompare(a, b);
  }

  [[nodiscard]] std::optional<std::filesystem::file_time_type> entryModifiedTime(const WallpaperEntry& entry) {
    if (entry.isDir || entry.absPath.string().starts_with("color:")) {
      return std::nullopt;
    }
    // Scanned entries carry the mtime captured during the directory walk, so the
    // sort comparator never stats. Pinned favorites lack it and stat once here.
    if (entry.hasMtime) {
      return entry.mtime;
    }
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(entry.absPath, ec);
    if (ec) {
      return std::nullopt;
    }
    return mtime;
  }

  // Ordering key for SortMode::Random. Hashing the path against a seed keeps the
  // shuffled order fixed until the seed changes, so filtering or starring a tile
  // rebuilds the visible entries without reordering the grid.
  [[nodiscard]] std::uint64_t randomOrderKey(const WallpaperEntry& entry, std::uint64_t seed) {
    std::uint64_t hash = std::filesystem::hash_value(entry.absPath) ^ seed;
    hash ^= hash >> 30U;
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= hash >> 27U;
    hash *= 0x94D049BB133111EBULL;
    return hash ^ (hash >> 31U);
  }

  [[nodiscard]] bool favoriteVisibleInBrowseContext(
      std::string_view favoritePath, const std::filesystem::path& activeDir, const std::filesystem::path& rootDir,
      bool flatten
  ) {
    if (favoritePath.starts_with("color:")) {
      return wallpaperDirectoriesEquivalent(activeDir, rootDir);
    }

    const std::filesystem::path favoriteFile = std::filesystem::path(FileUtils::normalizeWallpaperPath(favoritePath));
    if (wallpaperDirectoriesEquivalent(activeDir, rootDir)) {
      return true;
    }
    if (!flatten) {
      return wallpaperDirectoriesEquivalent(favoriteFile.parent_path(), activeDir);
    }
    return wallpaperPathInsideDirectory(favoriteFile, activeDir);
  }

} // namespace

class WallpaperGridAdapter : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(const WallpaperEntry&)>;
  using StarCallback = std::function<void(const WallpaperEntry&)>;

  explicit WallpaperGridAdapter(float scale) : m_scale(scale) {}

  void setEntries(const std::vector<WallpaperEntry>* entries) { m_entries = entries; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setConfig(ConfigService* config) { m_config = config; }
  void setCurrentWallpaperPath(std::string path) { m_currentWallpaperPath = std::move(path); }
  void setThumbnailService(ThumbnailService* service) {
    m_thumbnails = service;
    for (WallpaperTile* tile : m_pool) {
      if (tile != nullptr) {
        tile->setThumbnailService(service);
      }
    }
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnStarToggle(StarCallback callback) { m_onStarToggle = std::move(callback); }
  void setShowNames(bool showNames) {
    if (m_showNames == showNames) {
      return;
    }
    m_showNames = showNames;
    for (WallpaperTile* tile : m_pool) {
      if (tile != nullptr) {
        tile->setShowName(showNames);
      }
    }
  }

  void refreshVisibleThumbnails(Renderer& renderer) {
    for (WallpaperTile* tile : m_pool) {
      if (tile != nullptr && tile->visible()) {
        tile->refreshThumbnail(renderer);
      }
    }
  }

  [[nodiscard]] std::size_t itemCount() const override { return m_entries == nullptr ? 0U : m_entries->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    auto tile = std::make_unique<WallpaperTile>(0.0F, 0.0F, m_scale);
    tile->setThumbnailService(m_thumbnails);
    tile->setShowName(m_showNames);
    tile->setOnStarClick([this](const WallpaperEntry& entry) {
      if (m_onStarToggle) {
        m_onStarToggle(entry);
      }
    });
    m_pool.push_back(tile.get());
    return tile;
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    auto* wt = static_cast<WallpaperTile*>(&tile);
    wt->setShowName(m_showNames);
    wt->setCellSize(wt->width(), wt->height());
    if (m_renderer != nullptr && m_entries != nullptr && index < m_entries->size()) {
      const auto& entry = (*m_entries)[index];
      wt->setEntry(entry, *m_renderer);
      if (m_config != nullptr && !entry.isDir) {
        const std::string path = entry.absPath.string();
        wt->setFavoriteState(
            m_config->isWallpaperFavorite(path), favoriteThemeBadge(m_config->wallpaperFavorite(path))
        );
      } else {
        wt->setFavoriteState(false, std::nullopt);
      }
    }
    wt->setSelected(selected);
    wt->setCurrent(
        m_entries != nullptr
        && index < m_entries->size()
        && wallpaperEntryIsCurrent((*m_entries)[index], m_currentWallpaperPath)
    );
    wt->setHoveredVisual(hovered && !selected);
  }

  // Grid overlay owns clicks; star toggles dispatch here.
  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (m_entries == nullptr || index >= m_entries->size()) {
      return false;
    }
    const auto& entry = (*m_entries)[index];
    if (entry.isDir) {
      return false;
    }
    if (!WallpaperTile::hitTestStarRegion(cellWidth, cellHeight, m_scale, cellLocalX, cellLocalY)) {
      return false;
    }
    if (m_onStarToggle) {
      m_onStarToggle(entry);
    }
    return true;
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight
  ) const override {
    if (m_entries == nullptr || index >= m_entries->size()) {
      return false;
    }
    if ((*m_entries)[index].isDir) {
      return false;
    }
    return WallpaperTile::hitTestStarRegion(cellWidth, cellHeight, m_scale, cellLocalX, cellLocalY);
  }

  void applyOverlayHover(Node& tile, bool hovered) override {
    static_cast<WallpaperTile&>(tile).setStarHovered(hovered);
  }

  [[nodiscard]] std::string overlayTooltip(std::size_t index) const override {
    if (m_config == nullptr || m_entries == nullptr || index >= m_entries->size() || (*m_entries)[index].isDir) {
      return {};
    }
    return i18n::tr(
        m_config->isWallpaperFavorite((*m_entries)[index].absPath.string()) ? "wallpaper.panel.favorite-remove"
                                                                            : "wallpaper.panel.favorite-add"
    );
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets> itemTooltipAnchorInsets(
      std::size_t index, float cellWidth, float cellHeight, bool /*overlayHovered*/
  ) const override {
    if (m_entries == nullptr || index >= m_entries->size() || (*m_entries)[index].isDir) {
      return std::nullopt;
    }
    return WallpaperTile::starTooltipAnchorInsets(cellWidth, cellHeight, m_scale);
  }

  void onActivate(std::size_t index) override {
    if (!m_onActivate || m_entries == nullptr || index >= m_entries->size()) {
      return;
    }
    m_onActivate((*m_entries)[index]);
  }

private:
  float m_scale;
  const std::vector<WallpaperEntry>* m_entries = nullptr;
  std::string m_currentWallpaperPath;
  Renderer* m_renderer = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  ConfigService* m_config = nullptr;
  bool m_showNames = true;

  std::vector<WallpaperTile*> m_pool;
  ActivateCallback m_onActivate;
  StarCallback m_onStarToggle;
};

WallpaperPanel::WallpaperPanel(
    WaylandConnection* wayland, ConfigService* config, ThumbnailService* thumbnails, WallpaperScanner* scanner,
    noctalia::theme::ThemeService* themeService
)
    : m_wayland(wayland), m_config(config), m_thumbnails(thumbnails), m_scanner(scanner), m_themeService(themeService) {
  if (m_config != nullptr) {
    m_flatten = m_config->stateBool("wallpaper_panel", "flatten").value_or(false);
    m_showNames = m_config->config().shell.panel.wallpaperShowNames;
    if (const std::optional<std::string> sort = m_config->stateString("wallpaper_panel", "sort")) {
      m_sortMode = sortModeFromState(*sort);
    }
  }
  if (m_scanner != nullptr) {
    m_scanner->setOnComplete([this] { onScanComplete(); });
  }
}

WallpaperPanel::~WallpaperPanel() {
  if (m_scanner != nullptr) {
    m_scanner->setOnComplete(nullptr);
  }
}

void WallpaperPanel::refreshHost() {
  if (m_window != nullptr) {
    m_window->refresh();
  }
}

void WallpaperPanel::updateHost() {
  if (m_window != nullptr) {
    m_window->requestUpdateOnly();
  }
}

PanelPlacement WallpaperPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.wallpaperPlacement;
}

void WallpaperPanel::create() {
  const float scale = contentScale();
  const float control = Tokens::sizeControlM * scale;
  const auto iconButton = [&](Button** out, std::string glyph, std::string tooltip, std::function<void()> onClick) {
    return ui::button({
        .out = out,
        .glyph = std::move(glyph),
        .glyphSize = Tokens::sizeSymbol * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::move(tooltip),
        .minWidth = control,
        .minHeight = control,
        .padding = Tokens::spaceP8 * scale,
        .radius = Tokens::componentButtonRadius * scale,
        .onClick = std::move(onClick),
    });
  };

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Tokens::spaceP12 * scale,
  });

  // Header: title and close.
  auto header = ui::row({.out = &m_toolbar, .align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .fillWidth = true});
  header->setMinHeight(control);
  header->addChild(ui::label({
      .out = &m_title,
      .text = i18n::tr("wallpaper.panel.title"),
      .fontSize = Tokens::typeTitle3.size * scale,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
  }));
  header->addChild(ui::spacer());
  header->addChild(iconButton(&m_closeButton, "close", i18n::tr("wallpaper.panel.close"), [this] {
    if (m_window != nullptr) {
      DeferredCall::callLater([window = m_window] { window->close(); });
    }
  }));
  root->addChild(std::move(header));

  // Preview card: the focused wallpaper (or the current one) with its display and fill choices.
  auto card = ui::row({.out = &m_favoritesOptionsColumn, .align = FlexAlign::Center, .gap = Tokens::spaceP16 * scale, .fillWidth = true});
  card->setPadding(Tokens::spaceP12 * scale);
  card->setRadius(Tokens::radiusR12 * scale);
  card->setFill(colorSpecFromRole(ColorRole::GlassCard, panelCardOpacity()));
  card->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  card->addChild(ui::box({
      .out = &m_previewFrame,
      .fill = colorSpecFromRole(ColorRole::Fill1),
      .radius = Tokens::radiusR8 * scale,
      .configure = [](Box& box) { box.setClipChildren(true); },
  }));
  m_previewFrame->setFrameSize(kPreviewWidth * scale, kPreviewHeight * scale);
  m_previewFrame->addChild(ui::image({
      .out = &m_previewImage,
      .fit = ImageFit::Cover,
      .radius = Tokens::radiusR8 * scale,
      .participatesInLayout = false,
  }));
  m_previewImage->setFrameSize(kPreviewWidth * scale, kPreviewHeight * scale);

  auto info = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * scale, .flexGrow = 1.0F});
  info->addChild(ui::label({
      .out = &m_previewName,
      .fontSize = Tokens::typeHeadline.size * scale,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
  }));
  m_previewName->setEllipsize(TextEllipsize::Middle);
  info->addChild(ui::label({
      .out = &m_previewFolder,
      .fontSize = Tokens::typeFootnote.size * scale,
      .color = colorSpecFromRole(ColorRole::Label2),
      .maxLines = 1,
  }));
  m_previewFolder->setEllipsize(TextEllipsize::Middle);
  info->addChild(ui::spacer());

  auto choices = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .fillWidth = true});
  choices->addChild(ui::select({
      .out = &m_monitorSelect,
      .fontSize = Tokens::typeBody.size * scale,
      .controlHeight = control,
      .surfaceOpacity = panelCardOpacity(),
      .onSelectionChanged =
          [this](std::size_t idx, std::string_view) {
            m_selectedMonitorIndex = idx;
            m_navStack.clear();
            refreshVisibleEntries();
            resetSelection();
            rebindGrid();
            syncBackButton();
            syncBrowseChrome();
            m_dirty = true;
            refreshHost();
          },
  }));
  {
    std::vector<std::string> fillLabels;
    std::size_t fillIndex = 0;
    for (std::size_t i = 0; i < std::size(kWallpaperFillModes); ++i) {
      fillLabels.push_back(i18n::tr(kWallpaperFillModes[i].labelKey));
      if (m_config != nullptr && kWallpaperFillModes[i].value == m_config->config().wallpaper.fillMode) {
        fillIndex = i;
      }
    }
    choices->addChild(ui::select({
        .out = &m_fillSelect,
        .options = std::move(fillLabels),
        .selectedIndex = fillIndex,
        .fontSize = Tokens::typeBody.size * scale,
        .controlHeight = control,
        .surfaceOpacity = panelCardOpacity(),
        .onSelectionChanged =
            [this](std::size_t idx, std::string_view) {
              if (m_config != nullptr && idx < std::size(kWallpaperFillModes)) {
                (void)m_config->setOverride({"wallpaper", "fill_mode"}, std::string(kWallpaperFillModes[idx].key));
              }
            },
    }));
    m_fillSelect->setAccessible(
        {.role = AccessibleRole::Button, .name = i18n::tr("settings.schema.wallpaper.fill-mode.label"), .actionName = "press"}
    );
  }
  choices->addChild(ui::spacer());
  choices->addChild(ui::button({
      .out = &m_favoriteCurrentButton,
      .text = i18n::tr("wallpaper.panel.favorite"),
      .glyph = "star",
      .fontSize = Tokens::typeBody.size * scale,
      .glyphSize = Tokens::sizeSymbol * scale,
      .controlHeight = control,
      .variant = ButtonVariant::Default,
      .paddingH = Tokens::componentButtonInsetM * scale,
      .radius = Tokens::componentButtonRadius * scale,
      .onClick = [this] { toggleFavoriteForPath(previewWallpaperPath()); },
  }));
  info->addChild(std::move(choices));
  card->addChild(std::move(info));
  root->addChild(std::move(card));

  // Toolbar: search, source chips, then the list tools at the trailing edge.
  auto toolbar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .fillWidth = true});
  toolbar->addChild(iconButton(&m_backButton, "chevron-left", i18n::tr("wallpaper.panel.navigate-up"), [this] {
    navigateUp();
  }));
  toolbar->addChild(ui::input({
      .out = &m_filterInput,
      .placeholder = i18n::tr("wallpaper.panel.filter-placeholder"),
      .fontSize = Tokens::typeBody.size * scale,
      .controlHeight = control,
      .horizontalPadding = Tokens::spaceP12 * scale,
      .surfaceOpacity = panelCardOpacity(),
      .surfaceRole = ColorRole::Fill2,
      .width = kSearchWidth * scale,
      .onChange =
          [this](const std::string& text) {
            if (text == m_pendingFilterQuery) {
              return;
            }
            m_pendingFilterQuery = text;
            m_filterDebounceTimer.start(kFilterDebounceInterval, [this] {
              if (m_pendingFilterQuery == m_filterQuery) {
                return;
              }
              m_filterQuery = m_pendingFilterQuery;
              applyFilter();
              resetSelection();
              rebindGrid();
              syncBrowseChrome();
              m_dirty = true;
              refreshHost();
            });
          },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
  }));
  m_filterInput->setCapsule(true);
  toolbar->addChild(ui::segmented({
      .out = &m_sourceChips,
      .options =
          std::vector<ui::SegmentedOption>{
              {.label = i18n::tr("wallpaper.panel.all")},
              {.label = i18n::tr("wallpaper.panel.favorites")},
          },
      .selectedIndex = static_cast<std::size_t>(m_favoritesOnly ? 1 : 0),
      .scale = scale,
      .compact = true,
      .surfaceOpacity = panelCardOpacity(),
      .onChange =
          [this](std::size_t index) {
            m_favoritesOnly = index == 1;
            refreshVisibleEntries();
            resetSelection();
            rebindGrid();
            syncBrowseChrome();
            m_dirty = true;
            refreshHost();
          },
  }));
  toolbar->addChild(ui::spacer());
  toolbar->addChild(ui::button({
      .out = &m_flattenButton,
      .glyph = "folders",
      .glyphSize = Tokens::sizeSymbol * scale,
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("wallpaper.panel.subfolders"),
      .minWidth = control,
      .minHeight = control,
      .padding = Tokens::spaceP8 * scale,
      .radius = Tokens::componentButtonRadius * scale,
      .onClick =
          [this] {
            m_flatten = !m_flatten;
            if (m_config != nullptr) {
              (void)m_config->setStateBool("wallpaper_panel", "flatten", m_flatten);
            }
            syncFlattenButton();
            refreshVisibleEntries();
            resetSelection();
            rebindGrid();
            syncBrowseChrome();
            m_dirty = true;
            refreshHost();
          },
  }));
  toolbar->addChild(iconButton(&m_sortButton, std::string(sortModeGlyph(m_sortMode)), i18n::tr(sortModeTooltipKey(m_sortMode)), [this] {
    cycleSortMode();
  }));
  toolbar->addChild(iconButton(&m_colorButton, "color-picker", i18n::tr("wallpaper.panel.choose-color"), [this] {
    applyColorWallpaper();
  }));
  toolbar->addChild(iconButton(&m_refreshButton, "refresh", i18n::tr("wallpaper.panel.refresh"), [this] {
    if (m_scanner != nullptr) {
      m_scanner->invalidate();
    }
    refreshVisibleEntries();
    resetSelection();
    rebindGrid();
    syncBrowseChrome();
    m_dirty = true;
    refreshHost();
  }));
  root->addChild(std::move(toolbar));
  syncFlattenButton();

  // ── Body: virtualized scrolling grid ──────────────────────────────────
  m_adapter = std::make_unique<WallpaperGridAdapter>(scale);
  m_adapter->setThumbnailService(m_thumbnails);
  m_adapter->setShowNames(m_showNames);
  m_adapter->setConfig(m_config);
  m_adapter->setEntries(&m_visibleEntries);
  m_adapter->setOnActivate([this](const WallpaperEntry& entry) {
    if (entry.isDir) {
      navigateInto(entry.absPath);
    } else {
      applyWallpaperFromEntry(entry);
    }
  });
  m_adapter->setOnStarToggle([this](const WallpaperEntry& entry) {
    if (!entry.isDir) {
      toggleFavoriteForPath(entry.absPath.string());
    }
  });

  root->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .contentScale = scale,
          .minCellWidth = kMinTileWidth * scale,
          .squareCells = false,
          .columnGap = Tokens::componentGridGap * scale,
          .rowGap = Tokens::componentGridGap * scale,
          .overscanRows = 2,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (m_syncingGridSelectionVisual) {
                  return;
                }
                if (idx.has_value() && *idx < m_visibleEntries.size()) {
                  m_selectedVisibleIndex = *idx;
                } else {
                  m_selectedVisibleIndex = kNoVisibleSelection;
                }
                // The preview card follows the keyboard selection.
                syncBrowseChrome();
                updateHost();
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );

  if (m_grid != nullptr && m_grid->focusArea() != nullptr) {
    m_grid->focusArea()->setOnFocusGain([this] {
      m_gridKeyboardActive = true;
      if (m_grid != nullptr && hasVisibleSelection()) {
        m_syncingGridSelectionVisual = true;
        m_grid->setSelectedIndex(m_selectedVisibleIndex);
        m_syncingGridSelectionVisual = false;
      }
    });
    m_grid->focusArea()->setOnFocusLoss([this] {
      m_gridKeyboardActive = false;
      if (m_grid != nullptr) {
        m_syncingGridSelectionVisual = true;
        m_grid->setSelectedIndex(std::nullopt);
        m_syncingGridSelectionVisual = false;
      }
    });
  }

  // One state view replaces the grid: loading (after a short delay, so fast scans never flash), an empty folder,
  // no matches, no favourites, or a missing folder.
  root->addChild(ui::column(
      {
          .out = &m_loadingBox,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Tokens::spaceP8 * scale,
          .fillWidth = true,
          .flexGrow = 1.0F,
          .visible = false,
      },
      ui::spinner({.out = &m_spinner, .spinnerSize = Tokens::spaceP24 * scale, .spinning = false}),
      ui::glyph({
          .out = &m_stateGlyph,
          .glyph = "photo",
          .glyphSize = Tokens::spaceP32 * scale,
          .color = colorSpecFromRole(ColorRole::Label3),
      }),
      ui::label({
          .out = &m_stateTitle,
          .fontSize = Tokens::typeHeadline.size * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label1),
          .textAlign = TextAlign::Center,
      }),
      ui::label({
          .out = &m_stateBody,
          .fontSize = Tokens::typeFootnote.size * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
          .maxWidth = 320.0F * scale,
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      }),
      ui::button({
          .out = &m_stateAction,
          .fontSize = Tokens::typeBody.size * scale,
          .controlHeight = control,
          .variant = ButtonVariant::Default,
          .paddingH = Tokens::componentButtonInsetM * scale,
          .radius = Tokens::componentButtonRadius * scale,
          .visible = false,
      })
  ));

  setRoot(std::move(root));
  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }

  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this] {
      if (m_rootLayout == nullptr) {
        return;
      }
      m_thumbnailRefreshPending = true;
      updateHost();
    });
  }
}

void WallpaperPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_lastWidth = width;
  m_lastHeight = height;

  if (m_thumbnails != nullptr) {
    (void)m_thumbnails->uploadPending(renderer.textureManager());
    m_thumbnailRefreshPending = false;
  }

  if (m_adapter != nullptr) {
    m_adapter->setRenderer(&renderer);
  }

  // Drive cell height from current tile width via VirtualGridView's resolved
  // geometry: configure the cell height to follow the chosen tile aspect.
  // Hiding the captions only removes the row they reserved, so the thumbnails
  // keep the exact size and framing they have with a caption.
  if (m_grid != nullptr) {
    const float scale = contentScale();
    const float captionH = m_showNames ? 0.0F : WallpaperTile::captionHeight(scale);
    m_grid->setCellHeight(kMinTileWidth * scale * kTileAspect - captionH);
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  syncPreview(renderer);
  m_dirty = false;
}

void WallpaperPanel::doUpdate(Renderer& renderer) {
  if (m_rootLayout == nullptr) {
    return;
  }

  if (m_thumbnailRefreshPending && m_thumbnails != nullptr) {
    const bool changed = m_thumbnails->uploadPending(renderer.textureManager());
    m_thumbnailRefreshPending = false;
    if (changed && m_adapter != nullptr) {
      m_adapter->setRenderer(&renderer);
      m_adapter->refreshVisibleThumbnails(renderer);
    }
  }
  syncPreview(renderer);
}

void WallpaperPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_filterInput != nullptr) {
    m_filterInput->setSurfaceOpacity(opacity);
  }
  if (m_monitorSelect != nullptr) {
    m_monitorSelect->setSurfaceOpacity(opacity);
  }
  if (m_fillSelect != nullptr) {
    m_fillSelect->setSurfaceOpacity(opacity);
  }
  if (m_sourceChips != nullptr) {
    m_sourceChips->setSurfaceOpacity(opacity);
  }
  if (m_favoritesOptionsColumn != nullptr) {
    m_favoritesOptionsColumn->setFill(colorSpecFromRole(ColorRole::GlassCard, opacity));
  }
}

void WallpaperPanel::onOpen(std::string_view /*context*/) {
  m_filterQuery.clear();
  m_pendingFilterQuery.clear();
  m_filterDebounceTimer.stop();
  if (m_filterInput != nullptr) {
    m_filterInput->setValue("");
  }
  syncFlattenButton();
  const bool showNames = m_config == nullptr || m_config->config().shell.panel.wallpaperShowNames;
  if (showNames != m_showNames) {
    m_showNames = showNames;
    if (m_adapter != nullptr) {
      m_adapter->setShowNames(m_showNames);
    }
  }
  m_navStack.clear();
  populateMonitorChoices();
  syncSortButtonGlyph();
  reseedRandomSort();
  refreshVisibleEntries();
  resetSelection();
  rebindGrid();
  syncBrowseChrome();
  syncBackButton();
  m_dirty = true;
}

void WallpaperPanel::onClose() {
  m_filterDebounceTimer.stop();
  m_pendingFilterQuery.clear();
  m_filterQuery.clear();

  m_visibleEntries.clear();

  // Detach adapter from grid before either is destroyed; the pool tiles were
  // minted by the adapter.
  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  m_adapter.reset();
  m_thumbnailPendingSub.disconnect();

  m_rootLayout = nullptr;
  m_toolbar = nullptr;
  m_favoritesOptionsColumn = nullptr;
  m_title = nullptr;
  m_backButton = nullptr;
  m_monitorSelect = nullptr;
  m_favoriteCurrentButton = nullptr;
  m_filterInput = nullptr;
  m_flattenButton = nullptr;
  m_previewFrame = nullptr;
  m_previewImage = nullptr;
  m_previewName = nullptr;
  m_previewFolder = nullptr;
  m_fillSelect = nullptr;
  m_sourceChips = nullptr;
  m_stateGlyph = nullptr;
  m_stateTitle = nullptr;
  m_stateBody = nullptr;
  m_stateAction = nullptr;
  m_loadingDelayTimer.stop();
  m_loadingShown = false;
  if (m_thumbnails != nullptr && !m_previewPath.empty() && m_previewPx > 0) {
    m_thumbnails->release(m_previewPath, m_previewPx);
  }
  m_previewPath.clear();
  m_previewPx = 0;
  m_sortButton = nullptr;
  m_refreshButton = nullptr;
  m_colorButton = nullptr;
  m_closeButton = nullptr;
  m_grid = nullptr;
  m_loadingBox = nullptr;
  m_spinner = nullptr;
  m_scanPending = false;

  clearReleasedRoot();
  m_lastWidth = 0.0F;
  m_lastHeight = 0.0F;
  m_thumbnailRefreshPending = false;
}

bool WallpaperPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  if (m_window == nullptr) {
    return false;
  }
  InputArea* focused = m_window->focusedArea();

  // Down from the search field or the source chips goes into the grid.
  const bool fromToolbar = (m_filterInput != nullptr && focused == m_filterInput->inputArea())
      || (m_sourceChips != nullptr && focused == m_sourceChips->focusArea());
  if (fromToolbar && KeybindMatcher::matches(KeybindAction::Down, sym, modifiers) && m_grid != nullptr
      && m_grid->focusArea() != nullptr && !m_visibleEntries.empty()) {
    m_window->setFocus(m_grid->focusArea());
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    }
    return true;
  }

  if (focused != nullptr && !isDescendantOf(focused, m_grid)) {
    return false;
  }

  return handleKeyEvent(sym, modifiers);
}

InputArea* WallpaperPanel::initialFocusArea() const {
  return m_filterInput != nullptr ? m_filterInput->inputArea() : nullptr;
}

void WallpaperPanel::populateMonitorChoices() {
  m_monitorChoices.clear();
  m_monitorChoices.push_back({"", i18n::tr("wallpaper.panel.all-displays")});
  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (out.connectorName.empty()) {
        continue;
      }
      m_monitorChoices.push_back({out.connectorName, out.connectorName});
    }
  }

  if (m_selectedMonitorIndex >= m_monitorChoices.size()) {
    m_selectedMonitorIndex = 0;
  }

  if (m_monitorSelect != nullptr) {
    std::vector<std::string> labels;
    labels.reserve(m_monitorChoices.size());
    for (const auto& c : m_monitorChoices) {
      labels.push_back(c.label);
    }
    m_monitorSelect->setOptions(std::move(labels));
    m_monitorSelect->setSelectedIndex(m_selectedMonitorIndex);
    // One display: "All Displays" and the display itself are the same choice.
    m_monitorSelect->setVisible(m_monitorChoices.size() > 2);
  }
}

std::filesystem::path WallpaperPanel::rootDirectoryForSelection() const {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return {};
  }
  const auto& wp = m_config->config().wallpaper;
  const ThemeMode configured = shellThemeMode(m_config->config().theme);
  const bool isLight = m_themeService != nullptr ? m_themeService->isLightMode() : configured == ThemeMode::Light;
  const ThemeMode mode = wallpaper::effectiveThemeMode(configured, isLight);

  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  if (choice.connector.empty() || !wp.perMonitorDirectories) {
    return std::filesystem::path(wallpaper::resolveGlobalWallpaperDirectory(wp, mode));
  }

  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (out.connectorName == choice.connector) {
        return std::filesystem::path(wallpaper::resolveWallpaperDirectory(wp, out, mode));
      }
    }
  }
  return std::filesystem::path(wallpaper::resolveGlobalWallpaperDirectory(wp, mode));
}

std::filesystem::path WallpaperPanel::activeDirectoryForSelection() const {
  if (!m_navStack.empty()) {
    return m_navStack.back();
  }
  return rootDirectoryForSelection();
}

std::string WallpaperPanel::currentWallpaperPathForSelection() const {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return {};
  }

  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  return choice.connector.empty() ? m_config->getDefaultWallpaperPath() : m_config->getWallpaperPath(choice.connector);
}

std::optional<Color> WallpaperPanel::selectedFillColor() const {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return std::nullopt;
  }

  const auto& wp = m_config->config().wallpaper;
  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  Color sourceColor;
  const std::string currentPath =
      choice.connector.empty() ? m_config->getDefaultWallpaperPath() : m_config->getWallpaperPath(choice.connector);
  if (parseColorWallpaperPath(currentPath, sourceColor)) {
    return sourceColor;
  }

  if (!choice.connector.empty()) {
    for (const auto& ovr : wp.monitorOverrides) {
      if (ovr.match == choice.connector && ovr.fillColor.has_value()) {
        return resolveColorSpec(*ovr.fillColor);
      }
    }
  }

  if (wp.fillColor.has_value()) {
    return resolveColorSpec(*wp.fillColor);
  }
  return std::nullopt;
}

std::vector<std::string> WallpaperPanel::allMonitorConnectors() const {
  std::vector<std::string> connectors;
  if (m_wayland == nullptr) {
    return connectors;
  }
  for (const auto& out : m_wayland->outputs()) {
    if (!out.connectorName.empty()) {
      connectors.push_back(out.connectorName);
    }
  }
  return connectors;
}

std::string WallpaperPanel::displayNameForWallpaperPath(std::string_view path) {
  if (path.starts_with("color:")) {
    return std::string(path.substr(std::string_view("color:").size()));
  }
  const std::filesystem::path filePath(path);
  if (!filePath.filename().empty()) {
    return filePath.filename().string();
  }
  return std::string(path);
}

void WallpaperPanel::syncBrowseChrome() {
  if (m_backButton != nullptr) {
    m_backButton->setVisible(!m_navStack.empty());
  }
  if (m_favoriteCurrentButton != nullptr && m_config != nullptr) {
    const std::string path = previewWallpaperPath();
    const bool favorite = !path.empty() && m_config->isWallpaperFavorite(path);
    m_favoriteCurrentButton->setEnabled(!path.empty());
    m_favoriteCurrentButton->setGlyph(favorite ? "star-filled" : "star");
    m_favoriteCurrentButton->setText(i18n::tr(favorite ? "wallpaper.panel.favorited" : "wallpaper.panel.favorite"));
  }
  syncStateView();
}

std::string WallpaperPanel::previewWallpaperPath() const {
  const std::string selected = selectedWallpaperPath();
  return selected.empty() ? currentWallpaperPathForSelection() : selected;
}

void WallpaperPanel::syncFlattenButton() {
  if (m_flattenButton == nullptr) {
    return;
  }
  m_flattenButton->setVariant(m_flatten ? ButtonVariant::Default : ButtonVariant::Ghost);
  m_flattenButton->setAccessible({
      .role = AccessibleRole::ToggleButton,
      .name = i18n::tr("wallpaper.panel.subfolders"),
      .checked = m_flatten,
      .actionName = "press",
  });
}

void WallpaperPanel::syncPreview(Renderer& renderer) {
  if (m_previewImage == nullptr || m_previewFrame == nullptr) {
    return;
  }
  const std::string path = previewWallpaperPath();
  if (m_previewName != nullptr) {
    m_previewName->setText(path.empty() ? i18n::tr("wallpaper.panel.none") : displayNameForWallpaperPath(path));
  }
  if (m_previewFolder != nullptr) {
    m_previewFolder->setText(
        path.empty() || path.starts_with("color:") ? std::string{} : std::filesystem::path(path).parent_path().string()
    );
  }

  Color colorFill;
  const bool colorWallpaper = parseColorWallpaperPath(path, colorFill);
  m_previewFrame->setFill(colorWallpaper ? fixedColorSpec(colorFill) : colorSpecFromRole(ColorRole::Fill1));
  const std::string thumbPath = colorWallpaper ? std::string{} : path;
  const int px = static_cast<int>(std::lround(kPreviewWidth * contentScale() * std::max(1.0F, renderer.renderScale())));
  if (thumbPath != m_previewPath || px != m_previewPx) {
    if (m_thumbnails != nullptr && !m_previewPath.empty() && m_previewPx > 0) {
      m_thumbnails->release(m_previewPath, m_previewPx);
    }
    m_previewPath = thumbPath;
    m_previewPx = px;
    if (m_thumbnails != nullptr && !m_previewPath.empty()) {
      (void)m_thumbnails->acquire(m_previewPath, m_previewPx);
    }
  }
  const TextureHandle handle =
      (m_thumbnails != nullptr && !m_previewPath.empty()) ? m_thumbnails->peek(m_previewPath, m_previewPx) : TextureHandle{};
  if (handle.id != 0) {
    m_previewImage->setExternalTexture(renderer, handle);
    m_previewImage->setVisible(true);
  } else {
    m_previewImage->clear(renderer);
    m_previewImage->setVisible(false);
  }
}

void WallpaperPanel::syncStateView() {
  if (m_loadingBox == nullptr || m_grid == nullptr) {
    return;
  }
  const bool loading = m_scanPending && m_loadingShown;
  const bool scanning = m_scanPending && !m_loadingShown;
  const auto dir = activeDirectoryForSelection();
  std::error_code ec;
  const bool missing = !m_favoritesOnly && !dir.empty() && !std::filesystem::is_directory(dir, ec);
  const bool empty = !m_scanPending && m_visibleEntries.empty();
  const bool showState = loading || (!scanning && (missing || empty));

  m_grid->setVisible(!showState && !scanning);
  m_loadingBox->setVisible(showState);
  if (m_spinner != nullptr) {
    m_spinner->setVisible(loading);
    loading ? m_spinner->start() : m_spinner->stop();
  }
  if (!showState || loading) {
    if (m_stateGlyph != nullptr) {
      m_stateGlyph->setVisible(false);
    }
    if (m_stateTitle != nullptr) {
      m_stateTitle->setText(loading ? i18n::tr("wallpaper.panel.loading") : std::string{});
    }
    if (m_stateBody != nullptr) {
      m_stateBody->setVisible(false);
    }
    if (m_stateAction != nullptr) {
      m_stateAction->setVisible(false);
    }
    return;
  }

  std::string key;
  std::string glyph = "photo";
  std::function<void()> action;
  std::string actionKey;
  if (missing) {
    key = "missing-folder";
    glyph = "folder-off";
    actionKey = "wallpaper.panel.open-settings";
    action = [] { PanelManager::instance().openSettingsWindow("wallpaper"); };
  } else if (!m_filterQuery.empty()) {
    key = "no-matches";
    glyph = "search";
    actionKey = "wallpaper.panel.clear-search";
    action = [this] {
      if (m_filterInput != nullptr) {
        m_filterInput->setValue("");
      }
      m_pendingFilterQuery.clear();
      m_filterQuery.clear();
      applyFilter();
      rebindGrid();
      syncBrowseChrome();
      m_dirty = true;
      refreshHost();
    };
  } else if (m_favoritesOnly) {
    key = "no-favorites";
    glyph = "star";
  } else {
    key = "empty-folder";
    actionKey = "wallpaper.panel.open-settings";
    action = [] { PanelManager::instance().openSettingsWindow("wallpaper"); };
  }
  if (m_stateGlyph != nullptr) {
    m_stateGlyph->setGlyph(glyph);
    m_stateGlyph->setVisible(true);
  }
  if (m_stateTitle != nullptr) {
    m_stateTitle->setText(i18n::tr("wallpaper.panel.state." + key + ".title"));
  }
  if (m_stateBody != nullptr) {
    m_stateBody->setText(i18n::tr("wallpaper.panel.state." + key + ".body", "folder", dir.string()));
    m_stateBody->setVisible(true);
  }
  if (m_stateAction != nullptr) {
    m_stateAction->setVisible(static_cast<bool>(action));
    if (action) {
      m_stateAction->setText(i18n::tr(actionKey));
      m_stateAction->setOnClick(std::move(action));
    }
  }
}

std::string WallpaperPanel::selectedWallpaperPath() const {
  if (m_visibleEntries.empty() || m_selectedVisibleIndex >= m_visibleEntries.size()) {
    return {};
  }
  const auto& entry = m_visibleEntries[m_selectedVisibleIndex];
  if (entry.isDir) {
    return {};
  }
  return entry.absPath.string();
}

void WallpaperPanel::appendFilteredFavoriteEntries(
    std::vector<WallpaperEntry>& out, std::unordered_set<std::string>& favoritePaths,
    const std::filesystem::path& activeDir, const std::filesystem::path& rootDir
) const {
  if (m_config == nullptr) {
    return;
  }

  const std::string filterNeedle = StringUtils::toLower(m_filterQuery);
  for (const auto& favorite : m_config->wallpaperFavorites()) {
    if (!favoriteVisibleInBrowseContext(favorite.path, activeDir, rootDir, m_flatten)) {
      continue;
    }

    const std::string displayName = displayNameForWallpaperPath(favorite.path);
    if (!filterNeedle.empty() && !StringUtils::toLower(displayName).contains(filterNeedle)) {
      continue;
    }

    const std::string normalized = FileUtils::normalizeWallpaperPath(favorite.path);
    if (!favoritePaths.insert(normalized).second) {
      continue;
    }

    WallpaperEntry entry;
    entry.absPath = std::filesystem::path(favorite.path);
    entry.name = displayName;
    entry.isDir = false;
    out.push_back(std::move(entry));
  }
}

void WallpaperPanel::refreshScan() {
  const auto dir = activeDirectoryForSelection();
  if (dir.empty() || m_scanner == nullptr) {
    m_scanPending = false;
    return;
  }
  // requestScan() returns false when a worker scan was queued; the entries
  // arrive later via onScanComplete(). A cached/fresh dir returns true.
  m_scanPending = !m_scanner->requestScan(dir, m_flatten);
}

void WallpaperPanel::refreshVisibleEntries() {
  refreshScan();
  applyFilter();
  syncLoadingState();
}

void WallpaperPanel::onScanComplete() {
  // The scanner outlives the panel's UI; ignore late results after teardown.
  if (m_rootLayout == nullptr) {
    return;
  }
  const auto dir = activeDirectoryForSelection();
  m_scanPending = !dir.empty() && m_scanner != nullptr && m_scanner->scanning(dir, m_flatten);

  applyFilter();
  rebindGrid();
  syncBrowseChrome();
  syncLoadingState();
  m_dirty = true;
  refreshHost();
}

void WallpaperPanel::syncLoadingState() {
  if (!m_scanPending) {
    m_loadingDelayTimer.stop();
    m_loadingShown = false;
  } else if (!m_loadingShown && !m_loadingDelayTimer.active()) {
    m_loadingDelayTimer.start(kLoadingDelay, [this] {
      if (m_rootLayout == nullptr || !m_scanPending) {
        return;
      }
      m_loadingShown = true;
      syncStateView();
      m_dirty = true;
      refreshHost();
    });
  }
  syncStateView();
}

void WallpaperPanel::applyFilter() {
  m_visibleEntries.clear();
  std::unordered_set<std::string> favoritePaths;
  const auto dir = activeDirectoryForSelection();
  const auto rootDir = rootDirectoryForSelection();
  // Favourites show from every folder of this display, so the Favourites chip lists them all.
  appendFilteredFavoriteEntries(m_visibleEntries, favoritePaths, m_favoritesOnly ? rootDir : dir, rootDir);
  m_pinnedFavoriteCount = m_visibleEntries.size();

  const WallpaperScanResult* result =
      (m_favoritesOnly || dir.empty() || m_scanner == nullptr) ? nullptr : m_scanner->cached(dir, m_flatten);
  if (result != nullptr) {
    const std::string needle = StringUtils::toLower(m_filterQuery);
    const bool filterActive = !needle.empty();
    m_visibleEntries.reserve(m_visibleEntries.size() + result->entries.size());
    for (const auto& entry : result->entries) {
      if (!entry.isDir) {
        const std::string normalized = FileUtils::normalizeWallpaperPath(entry.absPath.string());
        if (favoritePaths.contains(normalized)) {
          continue;
        }
      }
      if (filterActive && !StringUtils::toLower(entry.name).contains(needle)) {
        continue;
      }
      m_visibleEntries.push_back(entry);
    }
  }

  sortVisibleEntries();
  if (m_selectedVisibleIndex >= m_visibleEntries.size()) {
    resetSelection();
  }
}

void WallpaperPanel::rebindGrid(bool resetScroll) {
  if (m_grid == nullptr) {
    return;
  }
  if (m_adapter != nullptr) {
    m_adapter->setCurrentWallpaperPath(currentWallpaperPathForSelection());
  }
  m_grid->notifyDataChanged();
  if (resetScroll || m_visibleEntries.empty()) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_visibleEntries.empty() || !hasVisibleSelection() || !m_gridKeyboardActive) {
    m_grid->setSelectedIndex(std::nullopt);
  } else {
    m_grid->setSelectedIndex(m_selectedVisibleIndex);
  }
}

void WallpaperPanel::resetSelection() { m_selectedVisibleIndex = kNoVisibleSelection; }

bool WallpaperPanel::hasVisibleSelection() const { return m_selectedVisibleIndex < m_visibleEntries.size(); }

void WallpaperPanel::toggleFavoriteForPath(const std::string& path) {
  if (m_config == nullptr || path.empty()) {
    return;
  }

  if (m_config->isWallpaperFavorite(path)) {
    m_config->removeWallpaperFavorite(path);
  } else {
    std::optional<WallpaperFavorite> preset;
    const std::string currentPath = currentWallpaperPathForSelection();
    if (!currentPath.empty()
        && FileUtils::normalizeWallpaperPath(path) == FileUtils::normalizeWallpaperPath(currentPath)) {
      preset = wallpaperFavoriteFromTheme(m_config->config().theme);
    }
    m_config->addWallpaperFavorite(path, preset);
  }

  refreshVisibleEntries();
  rebindGrid();
  syncBrowseChrome();
  m_dirty = true;
  refreshHost();
}

void WallpaperPanel::applyWallpaperPath(const std::string& path, const WallpaperFavorite* applyTheme) {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return;
  }

  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  const std::optional<std::string> connector =
      choice.connector.empty() ? std::optional<std::string>{} : std::optional<std::string>{choice.connector};
  m_config->applyWallpaperSelection(connector, path, applyTheme, allMonitorConnectors());
  rebindGrid();
  syncBrowseChrome();
}

const WallpaperFavorite* WallpaperPanel::favoriteThemeToApply(std::string_view path) const {
  if (m_config == nullptr || !m_config->isWallpaperFavorite(path)) {
    return nullptr;
  }
  return m_config->wallpaperFavorite(path);
}

void WallpaperPanel::selectVisibleIndex(std::size_t index) {
  if (m_visibleEntries.empty() || index >= m_visibleEntries.size()) {
    return;
  }

  m_selectedVisibleIndex = index;
  if (m_grid != nullptr) {
    m_grid->setSelectedIndex(index);
    m_grid->scrollToIndex(index);
  }
  syncBrowseChrome();

  m_dirty = true;
  refreshHost();
}

void WallpaperPanel::activateSelectedEntry() {
  if (!hasVisibleSelection()) {
    return;
  }

  const auto& entry = m_visibleEntries[m_selectedVisibleIndex];
  if (entry.isDir) {
    navigateInto(entry.absPath);
  } else {
    applyWallpaperFromEntry(entry);
  }
}

bool WallpaperPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (m_visibleEntries.empty()) {
    return false;
  }

  const std::size_t columns = m_grid != nullptr ? std::max<std::size_t>(1, m_grid->layoutColumns()) : 1;

  // F toggles the favourite of the selected wallpaper (the star is pointer-only otherwise).
  if ((sym == XKB_KEY_f || sym == XKB_KEY_F) && (modifiers & ~KeyMod::Shift) == 0 && hasVisibleSelection()
      && !m_visibleEntries[m_selectedVisibleIndex].isDir) {
    toggleFavoriteForPath(m_visibleEntries[m_selectedVisibleIndex].absPath.string());
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      return true;
    }
    if (m_selectedVisibleIndex > 0) {
      selectVisibleIndex(m_selectedVisibleIndex - 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    } else if (m_selectedVisibleIndex + 1 < m_visibleEntries.size()) {
      selectVisibleIndex(m_selectedVisibleIndex + 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    } else if (m_selectedVisibleIndex >= columns) {
      selectVisibleIndex(m_selectedVisibleIndex - columns);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    } else {
      const std::size_t nextIndex = m_selectedVisibleIndex + columns;
      if (nextIndex < m_visibleEntries.size()) {
        selectVisibleIndex(nextIndex);
      }
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelectedEntry();
    return true;
  }

  return false;
}

void WallpaperPanel::syncBackButton() {
  uiAssertNotRendering("WallpaperPanel::syncBackButton");
  if (m_backButton == nullptr) {
    return;
  }
  const bool canNavigateUp = !m_navStack.empty();
  m_backButton->setEnabled(canNavigateUp);
  m_backButton->setVisible(canNavigateUp);
}

void WallpaperPanel::navigateInto(const std::filesystem::path& dir) {
  m_navStack.push_back(dir);
  refreshVisibleEntries();
  resetSelection();
  rebindGrid(true);
  syncBackButton();
  syncBrowseChrome();
  m_dirty = true;
  refreshHost();
}

void WallpaperPanel::navigateUp() {
  if (m_navStack.empty()) {
    return;
  }
  m_navStack.pop_back();
  refreshVisibleEntries();
  resetSelection();
  rebindGrid(true);
  syncBackButton();
  syncBrowseChrome();
  m_dirty = true;
  refreshHost();
}

WallpaperPanel::SortMode WallpaperPanel::sortModeFromState(std::string_view value) {
  if (value == "name_desc") {
    return SortMode::NameDesc;
  }
  if (value == "date_asc") {
    return SortMode::DateAsc;
  }
  if (value == "date_desc") {
    return SortMode::DateDesc;
  }
  if (value == "random") {
    return SortMode::Random;
  }
  return SortMode::NameAsc;
}

std::string_view WallpaperPanel::sortModeStateValue(SortMode mode) {
  switch (mode) {
  case SortMode::NameDesc:
    return "name_desc";
  case SortMode::DateAsc:
    return "date_asc";
  case SortMode::DateDesc:
    return "date_desc";
  case SortMode::Random:
    return "random";
  case SortMode::NameAsc:
  default:
    return "name_asc";
  }
}

std::string_view WallpaperPanel::sortModeGlyph(SortMode mode) {
  switch (mode) {
  case SortMode::NameDesc:
    return "sort-z-a";
  case SortMode::DateAsc:
    return "sort-ascending-2";
  case SortMode::DateDesc:
    return "sort-descending-2";
  case SortMode::Random:
    return "arrows-random";
  case SortMode::NameAsc:
  default:
    return "sort-a-z";
  }
}

const char* WallpaperPanel::sortModeTooltipKey(SortMode mode) {
  switch (mode) {
  case SortMode::NameDesc:
    return "wallpaper.panel.sort-name-desc";
  case SortMode::DateAsc:
    return "wallpaper.panel.sort-date-asc";
  case SortMode::DateDesc:
    return "wallpaper.panel.sort-date-desc";
  case SortMode::Random:
    return "wallpaper.panel.sort-random";
  case SortMode::NameAsc:
  default:
    return "wallpaper.panel.sort-name-asc";
  }
}

void WallpaperPanel::syncSortButtonGlyph() {
  if (m_sortButton == nullptr) {
    return;
  }
  m_sortButton->setGlyph(sortModeGlyph(m_sortMode));
  m_sortButton->setTooltip(i18n::tr(sortModeTooltipKey(m_sortMode)));

  if (!m_sortButton->hovered()) {
    return;
  }
  InputArea* area = m_sortButton->inputArea();
  if (area == nullptr || !area->hasTooltip()) {
    return;
  }
  const auto parentCtx = (m_window != nullptr ? m_window->fallbackPopupParentContext() : std::nullopt);
  if (!parentCtx.has_value() || parentCtx->xdgSurface == nullptr || parentCtx->output == nullptr) {
    return;
  }
  TooltipManager::instance().onHoverChange(area, parentCtx->xdgSurface, parentCtx->output);
}

void WallpaperPanel::cycleSortMode() {
  SortMode next = SortMode::NameAsc;
  switch (m_sortMode) {
  case SortMode::NameAsc:
    next = SortMode::NameDesc;
    break;
  case SortMode::NameDesc:
    next = SortMode::DateAsc;
    break;
  case SortMode::DateAsc:
    next = SortMode::DateDesc;
    break;
  case SortMode::DateDesc:
    next = SortMode::Random;
    break;
  case SortMode::Random:
    next = SortMode::NameAsc;
    break;
  }
  setSortMode(next);
}

void WallpaperPanel::setSortMode(SortMode mode) {
  if (m_sortMode == mode) {
    return;
  }
  m_sortMode = mode;
  if (mode == SortMode::Random) {
    reseedRandomSort();
  }
  if (m_config != nullptr) {
    (void)m_config->setStateString("wallpaper_panel", "sort", sortModeStateValue(mode));
  }
  syncSortButtonGlyph();
  sortVisibleEntries();
  rebindGrid();
  m_dirty = true;
  refreshHost();
}

void WallpaperPanel::reseedRandomSort() {
  m_randomSeed = std::uniform_int_distribution<std::uint64_t>{}(Random::rng());
}

void WallpaperPanel::sortVisibleEntries() {
  if (m_visibleEntries.empty()) {
    return;
  }

  const auto compareEntries = [this](const WallpaperEntry& a, const WallpaperEntry& b) {
    switch (m_sortMode) {
    case SortMode::NameDesc: {
      const int order = caseInsensitiveNameOrder(a.name, b.name);
      return order > 0;
    }
    case SortMode::DateAsc: {
      const auto aTime = entryModifiedTime(a);
      const auto bTime = entryModifiedTime(b);
      if (aTime.has_value() && bTime.has_value()) {
        if (*aTime != *bTime) {
          return *aTime < *bTime;
        }
      } else if (aTime.has_value() != bTime.has_value()) {
        return !aTime.has_value();
      }
      return caseInsensitiveNameOrder(a.name, b.name) < 0;
    }
    case SortMode::DateDesc: {
      const auto aTime = entryModifiedTime(a);
      const auto bTime = entryModifiedTime(b);
      if (aTime.has_value() && bTime.has_value()) {
        if (*aTime != *bTime) {
          return *aTime > *bTime;
        }
      } else if (aTime.has_value() != bTime.has_value()) {
        return aTime.has_value();
      }
      return caseInsensitiveNameOrder(a.name, b.name) < 0;
    }
    case SortMode::Random: {
      const std::uint64_t aKey = randomOrderKey(a, m_randomSeed);
      const std::uint64_t bKey = randomOrderKey(b, m_randomSeed);
      if (aKey != bKey) {
        return aKey < bKey;
      }
      return caseInsensitiveNameOrder(a.name, b.name) < 0;
    }
    case SortMode::NameAsc:
    default: {
      const int order = caseInsensitiveNameOrder(a.name, b.name);
      return order < 0;
    }
    }
  };

  const auto sortRange = [&](const std::size_t beginIndex, const std::size_t endIndex, const bool foldersFirst) {
    if (beginIndex >= endIndex || beginIndex >= m_visibleEntries.size()) {
      return;
    }
    const std::size_t end = std::min(endIndex, m_visibleEntries.size());
    const auto begin = m_visibleEntries.begin() + static_cast<std::ptrdiff_t>(beginIndex);
    const auto endIt = m_visibleEntries.begin() + static_cast<std::ptrdiff_t>(end);
    if (foldersFirst) {
      const auto folderEnd = std::partition(begin, endIt, [](const WallpaperEntry& entry) { return entry.isDir; });
      std::sort(begin, folderEnd, compareEntries);
      std::sort(folderEnd, endIt, compareEntries);
    } else {
      std::sort(begin, endIt, compareEntries);
    }
  };

  sortRange(0, m_pinnedFavoriteCount, false);
  sortRange(m_pinnedFavoriteCount, m_visibleEntries.size(), !m_flatten);
}

void WallpaperPanel::applyWallpaperFromEntry(const WallpaperEntry& entry) {
  if (entry.isDir) {
    return;
  }

  const std::string path = entry.absPath.string();
  applyWallpaperPath(path, favoriteThemeToApply(path));
  kLog.info(
      "applied wallpaper {} to {}", path,
      m_selectedMonitorIndex == 0 ? "ALL" : m_monitorChoices[m_selectedMonitorIndex].connector
  );
}

void WallpaperPanel::applyColorWallpaper() {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return;
  }

  ColorPickerDialogOptions options;
  options.title = i18n::tr("wallpaper.panel.color-title");
  if (auto color = selectedFillColor()) {
    options.initialColor = *color;
  } else if (auto last = ColorPickerDialog::lastResult()) {
    options.initialColor = *last;
  }

  (void)ColorPickerDialog::open(std::move(options), [this](std::optional<Color> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }

    Color rgb = *result;
    rgb.a = 1.0F;
    applyWallpaperPath(colorWallpaperPath(rgb), nullptr);
    syncBrowseChrome();
    kLog.info("applied color wallpaper {}", colorWallpaperPath(rgb));
  });
}

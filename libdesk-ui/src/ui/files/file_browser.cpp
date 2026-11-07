#include "ui/files/file_browser.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/files/file_cells.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <glib.h>
#include <sys/statvfs.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("file-browser");
  constexpr auto kSkeletonDelay = std::chrono::milliseconds(200);
  constexpr auto kSkeletonHold = std::chrono::milliseconds(400);
  constexpr auto kDoubleClick = std::chrono::milliseconds(400);
  constexpr float kSlideGap = 32.0F;     // P12: incoming and outgoing 32 apart
  constexpr float kParallax = 0.30F;     // the page underneath moves 30 %
  constexpr float kUnderDim = 0.90F;     // and dims 10 %
  constexpr float kSidebarRow = 32.0F;
  constexpr float kPathBar = 32.0F;      // FILES-UX §0.2: compact row, not 28
  constexpr float kListHeader = 28.0F;
  constexpr float kIconGap = 8.0F;
  constexpr float kContentPad = 12.0F;

  std::string homeName() {
    const fs::path home = g_get_home_dir();
    return home.filename().string();
  }

  // A full path, displayed from the home folder ("sitish › Documents"), or from / outside home.
  std::vector<std::pair<std::string, fs::path>> pathSegments(const fs::path& dir) {
    std::vector<std::pair<std::string, fs::path>> out;
    const fs::path home = g_get_home_dir();
    fs::path p = dir;
    while (true) {
      if (p == home) {
        out.emplace_back(homeName(), p);
        break;
      }
      if (!p.has_relative_path()) {
        out.emplace_back("/", p);
        break;
      }
      out.emplace_back(p.filename().string(), p);
      p = p.parent_path();
    }
    std::ranges::reverse(out);
    return out;
  }

  std::optional<std::uint64_t> freeBytes(const fs::path& dir) {
    struct statvfs st {};
    if (dir.empty() || ::statvfs(dir.c_str(), &st) != 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
  }

  // Lays its children out to fill it; the browser positions the pages for the slide.
  class FillNode final : public Node {
  public:
    std::function<void(Renderer&, float, float)> onLayout;

  protected:
    void doLayout(Renderer& renderer) override {
      if (onLayout) {
        onLayout(renderer, width(), height());
      }
    }
    LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
      return constraints.constrain(LayoutSize{});
    }
  };
} // namespace

// Binds the rows of one page to one view's pooled cells.
class FileItemAdapter final : public VirtualGridAdapter {
public:
  FileItemAdapter(FileBrowser& browser, bool icons) : m_browser(browser), m_icons(icons) {}

  void setRows(const std::vector<std::shared_ptr<const FileInfo>>* rows) { m_rows = rows; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setScale(float scale) { m_scale = scale; }
  void setStateFn(std::function<FileCellState(const FileInfo&, std::size_t)> fn) { m_state = std::move(fn); }
  void setLive(bool live) { m_live = live; }

  [[nodiscard]] std::size_t itemCount() const override { return m_rows != nullptr ? m_rows->size() : 0; }
  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    if (m_icons) {
      return std::make_unique<FileIconCell>(m_scale);
    }
    return std::make_unique<FileListRow>(m_scale);
  }
  void bindTile(Node& tile, std::size_t index, bool /*selected*/, bool hovered) override {
    if (m_renderer == nullptr || m_rows == nullptr || index >= m_rows->size()) {
      return;
    }
    const FileInfo& info = *(*m_rows)[index];
    FileCellState state = m_state && m_live ? m_state(info, index) : FileCellState{};
    state.hovered = hovered && !state.selected && !state.disabled;
    if (m_icons) {
      static_cast<FileIconCell&>(tile).bind(*m_renderer, info, tile.width(), tile.height(), state);
    } else {
      static_cast<FileListRow&>(tile).bind(*m_renderer, info, tile.width(), state);
    }
  }
  bool onPointerPress(std::size_t index, float, float, float, float) override {
    if (m_live) {
      m_browser.onItemPress(index);
    }
    return true;
  }
  PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
    PointerReleaseResult result;
    if (m_live && index.has_value() && m_activate == index) {
      result.activateIndex = index;
    }
    m_activate.reset();
    return result;
  }
  void onActivate(std::size_t index) override {
    if (m_live) {
      m_browser.onItemActivate(index);
    }
  }
  void onSecondaryActivate(std::size_t index, float x, float y) override {
    if (m_live && m_menu) {
      m_menu(index, x, y);
    }
  }
  void armActivate(std::size_t index) { m_activate = index; }
  void setMenu(std::function<void(std::size_t, float, float)> menu) { m_menu = std::move(menu); }
  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    return m_rows != nullptr && index < m_rows->size() ? (*m_rows)[index]->displayName : std::string();
  }

private:
  FileBrowser& m_browser;
  bool m_icons;
  bool m_live = false;
  float m_scale = 1.0F;
  Renderer* m_renderer = nullptr;
  const std::vector<std::shared_ptr<const FileInfo>>* m_rows = nullptr;
  std::function<FileCellState(const FileInfo&, std::size_t)> m_state;
  std::function<void(std::size_t, float, float)> m_menu;
  std::optional<std::size_t> m_activate;
};

struct FileBrowser::Page {
  std::vector<std::shared_ptr<const FileInfo>> rows;
  std::unique_ptr<FileItemAdapter> iconsAdapter;
  std::unique_ptr<FileItemAdapter> listAdapter;
  ScrollViewState iconsScroll;
  ScrollViewState listScroll;
  std::string status; // empty-state key: "" = rows, "empty", "denied", "missing", "error"

  // Nodes of the latest build.
  Node* root = nullptr;
  VirtualGridView* icons = nullptr;
  Flex* listBox = nullptr;
  VirtualGridView* list = nullptr;
  Node* skeleton = nullptr;
  Flex* empty = nullptr;
  Glyph* emptyGlyph = nullptr;
  Label* emptyTitle = nullptr;
  Label* emptyBody = nullptr;
  std::array<Button*, 4> sortButtons{};

  float x = 0.0F; // slide offset
  float opacity = 1.0F;
  AnimationManager::Id xAnim = 0;
  AnimationManager::Id fadeAnim = 0;
};

struct FileBrowser::Ui {
  std::array<Page, 2> pages;
  std::size_t current = 0;
  float scale = 1.0F;
  float stageWidth = 0.0F;
  float viewMix = 1.0F; // icons opacity during a view switch fades; list = 1 - mix for the other
  AnimationManager::Id viewAnim = 0;

  InputArea* focus = nullptr;
  FillNode* stage = nullptr;
  Flex* pathRow = nullptr;
  Label* status = nullptr;
  Segmented* viewSwitch = nullptr;
  Button* sortButton = nullptr;
  Flex* sidebarList = nullptr;
  float sidebarWidth = 0.0F;

  Page& cur() { return pages[current]; }
  Page& other() { return pages[1 - current]; }
};

FileBrowser::FileBrowser(DirectoryWatcher& watcher, AnimationManager& animations, Host host, Options options)
    : m_watcher(watcher), m_animations(animations), m_host(std::move(host)), m_options(std::move(options)),
      m_ui(std::make_unique<Ui>()) {
  m_ui->scale = m_options.scale;
  for (Page& page : m_ui->pages) {
    page.iconsAdapter = std::make_unique<FileItemAdapter>(*this, true);
    page.listAdapter = std::make_unique<FileItemAdapter>(*this, false);
    for (FileItemAdapter* a : {page.iconsAdapter.get(), page.listAdapter.get()}) {
      a->setRows(&page.rows);
      a->setScale(m_ui->scale);
      a->setStateFn([this](const FileInfo& info, std::size_t index) {
        FileCellState state;
        state.selected = m_selected.contains(info.name);
        state.cursor = m_keyboardCursor && m_cursor == info.name;
        state.disabled = !rowEnabled(info);
        state.ghost = info.hidden;
        (void)index;
        return state;
      });
      a->setMenu([this](std::size_t index, float x, float y) {
        if (m_host.menu) {
          m_host.menu(index, x, y);
        }
      });
    }
  }
  m_ui->cur().iconsAdapter->setLive(true);
  m_ui->cur().listAdapter->setLive(true);
}

FileBrowser::~FileBrowser() { *m_alive = false; }

void FileBrowser::setScale(float scale) {
  m_options.scale = scale;
  m_ui->scale = scale;
  for (Page& page : m_ui->pages) {
    page.iconsAdapter->setScale(scale);
    page.listAdapter->setScale(scale);
  }
}

InputArea* FileBrowser::focusArea() const noexcept { return m_ui->focus; }

// ---- builds -------------------------------------------------------------------------------------------------------

std::unique_ptr<Node> FileBrowser::buildHeaderAccessory() {
  const float s = m_ui->scale;
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s});
  row->addChild(ui::segmented({
      .out = &m_ui->viewSwitch,
      .options = std::vector<ui::SegmentedOption>{{.glyph = "layout-grid", .tooltip = "Icons (Ctrl+1)"},
                                                  {.glyph = "list", .tooltip = "List (Ctrl+2)"}},
      .selectedIndex = static_cast<std::size_t>(m_options.view),
      .scale = s,
      .compact = true,
      .onChange = [this](std::size_t index) { setView(static_cast<View>(index)); },
  }));
  row->addChild(ui::button({
      .out = &m_ui->sortButton,
      .glyph = std::string("arrows-sort"),
      .glyphSize = 16.0F * s,
      .variant = ButtonVariant::Ghost,
      .tooltip = std::string("Sort"),
      .radius = Style::scaledRadiusSm(s),
      .width = 32.0F * s,
      .height = 32.0F * s,
      .onClick =
          [this] {
            // Cycles Name, Date, Size, Kind until the sort menu (FB4).
            static constexpr std::array kOrder{FileSortKey::Name, FileSortKey::Modified, FileSortKey::Size, FileSortKey::Kind};
            const auto* it = std::ranges::find(kOrder, m_sort.key);
            const FileSortKey next = it + 1 < kOrder.end() ? *(it + 1) : kOrder.front();
            m_sort.descending = next == FileSortKey::Modified || next == FileSortKey::Size;
            m_sort.key = next;
            if (m_modelView != nullptr) {
              m_modelView->setSort(m_sort);
            } else {
              refreshRows();
            }
          },
  }));
  return row;
}

std::unique_ptr<Node> FileBrowser::buildSidebar(float width) {
  const float s = m_ui->scale;
  m_ui->sidebarWidth = width;
  auto list = ui::column({.out = &m_ui->sidebarList, .align = FlexAlign::Stretch, .gap = 2.0F * s,
                          .padding = Style::spaceSm * s, .width = width});
  refreshSidebar();
  return list;
}

void FileBrowser::refreshSidebar() {
  Flex* list = m_ui->sidebarList;
  if (list == nullptr) {
    return;
  }
  const float s = m_ui->scale;
  while (!list->children().empty()) {
    (void)list->removeChild(list->children().back().get());
  }
  const auto caption = [&](const char* text) {
    list->addChild(ui::label({
        .text = std::string(text),
        .fontSize = Style::fontSizeCaption * s,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::Label2),
    }));
  };
  const auto row = [&](const Place& place) {
    const bool current = place.id.empty() ? (!m_listing.has_value() && !place.path.empty() && place.path == m_location)
                                          : place.id == m_listingId;
    auto button = ui::button({
        .text = place.label,
        .glyph = place.glyph,
        .glyphSize = 16.0F * s,
        .selected = current,
        .contentAlign = ButtonContentAlign::Start,
        .variant = current ? ButtonVariant::TabActive : ButtonVariant::Ghost,
        .radius = Style::scaledRadiusSm(s),
        .height = kSidebarRow * s,
        .onClick =
            [this, place] {
              if (place.onClick) {
                place.onClick();
              } else {
                jump(place.path);
              }
            },
    });
    if (!place.caption.empty()) {
      button->setBadge(place.caption);
    }
    list->addChild(std::move(button));
  };

  caption("Favourites");
  const fs::path home = g_get_home_dir();
  row({.label = homeName(), .glyph = "home", .path = home});
  const std::pair<GUserDirectory, std::pair<const char*, const char*>> dirs[] = {
      {G_USER_DIRECTORY_DESKTOP, {"Desktop", "device-desktop"}},
      {G_USER_DIRECTORY_DOCUMENTS, {"Documents", "file-text"}},
      {G_USER_DIRECTORY_DOWNLOAD, {"Downloads", "download"}},
      {G_USER_DIRECTORY_PICTURES, {"Pictures", "photo"}},
      {G_USER_DIRECTORY_MUSIC, {"Music", "music"}},
      {G_USER_DIRECTORY_VIDEOS, {"Videos", "movie"}},
  };
  for (const auto& [dir, label] : dirs) {
    const char* path = g_get_user_special_dir(dir);
    std::error_code ec;
    // An unset XDG user dir falls back to $HOME, which is already listed.
    if (path != nullptr && fs::path(path) != home && fs::is_directory(path, ec)) {
      row({.label = label.first, .glyph = label.second, .path = path});
    }
  }
  if (!m_options.extraPlaces.empty()) {
    caption("Locations");
    for (const Place& place : m_options.extraPlaces) {
      row(place);
    }
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

std::unique_ptr<Node> FileBrowser::buildContent() {
  const float s = m_ui->scale;
  auto content = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});

  auto focus = ui::inputArea({.out = &m_ui->focus, .focusable = true, .participatesInLayout = false});
  focus->setVisible(false);
  content->addChild(std::move(focus));

  auto stage = std::make_unique<FillNode>();
  stage->setClipChildren(true);
  stage->setFlexGrow(1.0F);
  m_ui->stage = stage.get();

  for (std::size_t i = 0; i < 2; ++i) {
    Page& page = m_ui->pages[i];
    auto root = ui::node({.clipChildren = true});
    page.root = root.get();

    root->addChild(ui::virtualGridView({
        .out = &page.icons,
        .state = &page.iconsScroll,
        .contentScale = s,
        .columns = 0,
        .minCellWidth = FileIconCell::kWidth * s,
        .cellHeight = FileIconCell::kHeight * s,
        .squareCells = false,
        .columnGap = kIconGap * s,
        .rowGap = kIconGap * s,
        .overscanRows = 1,
        .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        .scrollbarVisible = true,
        .adapter = page.iconsAdapter.get(),
    }));

    auto listBox = ui::column({.out = &page.listBox, .align = FlexAlign::Stretch});
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s,
                           .paddingH = (4.0F + Style::spaceSm) * s, .height = kListHeader * s});
    const std::array<std::pair<const char*, FileSortKey>, 4> cols{
        {{"Name", FileSortKey::Name}, {"Date Modified", FileSortKey::Modified}, {"Size", FileSortKey::Size},
         {"Kind", FileSortKey::Kind}}};
    for (std::size_t c = 0; c < cols.size(); ++c) {
      header->addChild(ui::button({
          .out = &page.sortButtons[c],
          .text = std::string(cols[c].first),
          .fontSize = Style::fontSizeCaption * s,
          .contentAlign = c == 2 ? ButtonContentAlign::End : ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .paddingH = 0.0F,
          .height = kListHeader * s,
          .flexGrow = c == 0 ? 1.0F : 0.0F,
          .onClick = [this, key = cols[c].second] { setSort(key); },
      }));
    }
    listBox->addChild(std::move(header));
    listBox->addChild(ui::separator({.gradientEdges = false}));
    listBox->addChild(ui::virtualGridView({
        .out = &page.list,
        .state = &page.listScroll,
        .contentScale = s,
        .columns = 1,
        .cellHeight = std::ceil(FileListRow::kHeight * s),
        .squareCells = false,
        .columnGap = 0.0F,
        .rowGap = 0.0F,
        .overscanRows = 3,
        .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        .scrollbarVisible = true,
        .adapter = page.listAdapter.get(),
        .flexGrow = 1.0F,
    }));
    root->addChild(std::move(listBox));

    auto skeleton = ui::node({.visible = false});
    page.skeleton = skeleton.get();
    root->addChild(std::move(skeleton));

    auto empty = ui::column({.out = &page.empty, .align = FlexAlign::Center, .justify = FlexJustify::Center,
                             .gap = Style::spaceSm * s, .visible = false});
    empty->addChild(ui::glyph({.out = &page.emptyGlyph, .glyph = std::string("folder"), .glyphSize = 48.0F * s,
                               .color = colorSpecFromRole(ColorRole::Label3)}));
    empty->addChild(ui::label({.out = &page.emptyTitle, .fontSize = Tokens::typeTitle3.size * s,
                               .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
    empty->addChild(ui::label({.out = &page.emptyBody, .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 3,
                               .textAlign = TextAlign::Center}));
    root->addChild(std::move(empty));
    stage->addChild(std::move(root));
  }

  stage->onLayout = [this](Renderer& renderer, float w, float h) {
    m_ui->stageWidth = w;
    const float pad = kContentPad * m_ui->scale;
    for (std::size_t i = 0; i < 2; ++i) {
      Page& page = m_ui->pages[i];
      if (page.root == nullptr) {
        continue;
      }
      page.iconsAdapter->setRenderer(&renderer);
      page.listAdapter->setRenderer(&renderer);
      page.root->setPosition(page.x, 0.0F);
      page.root->setSize(w, h);
      page.root->setOpacity(page.opacity);
      const bool icons = m_options.view == View::Icons;
      page.icons->setVisible(icons || m_ui->viewAnim != 0);
      page.listBox->setVisible(!icons || m_ui->viewAnim != 0);
      page.icons->setOpacity(icons ? m_ui->viewMix : 1.0F - m_ui->viewMix);
      page.listBox->setOpacity(icons ? 1.0F - m_ui->viewMix : m_ui->viewMix);
      page.icons->arrange(renderer, LayoutRect{.x = pad, .y = pad, .width = w - 2.0F * pad, .height = h - pad});
      page.listBox->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = w, .height = h});
      page.empty->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = w, .height = h});
      page.skeleton->setSize(w, h);
      page.skeleton->layout(renderer);
    }
  };
  content->addChild(std::move(stage));

  content->addChild(ui::separator({.gradientEdges = false}));
  auto path = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * s, .paddingH = Style::spaceMd * s,
                       .height = kPathBar * s});
  path->addChild(ui::row({.out = &m_ui->pathRow, .align = FlexAlign::Center, .gap = 2.0F * s, .clipChildren = true,
                          .flexGrow = 1.0F}));
  path->addChild(ui::label({.out = &m_ui->status, .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
  content->addChild(std::move(path));

  for (Page& page : m_ui->pages) {
    page.icons->notifyDataChanged();
    page.list->notifyDataChanged();
  }
  if (m_location.empty() && !m_listing.has_value()) {
    navigate({.path = g_get_home_dir()}, Motion::None);
  } else {
    showSkeleton(m_skeletonShown);
    refreshChrome();
  }
  return content;
}

// ---- navigation ---------------------------------------------------------------------------------------------------

FileBrowser::HistoryEntry FileBrowser::current() const { return {.path = m_location, .listing = m_listing}; }

void FileBrowser::go(const fs::path& dir) {
  if (!m_listing.has_value() && dir == m_location) {
    return;
  }
  const bool deeper = !m_location.empty() && dir.parent_path() == m_location;
  const bool shallower = !m_location.empty() && m_location.string().starts_with(dir.string() + "/");
  m_back.push_back(current());
  m_forward.clear();
  navigate({.path = dir}, deeper ? Motion::Deeper : shallower ? Motion::Back : Motion::Fade);
}

void FileBrowser::jump(const fs::path& dir) {
  if (!m_listing.has_value() && dir == m_location) {
    return;
  }
  m_back.push_back(current());
  m_forward.clear();
  navigate({.path = dir}, Motion::Fade);
}

void FileBrowser::showListing(Listing listing) {
  if (m_listingId == listing.id && m_listing.has_value()) {
    m_listing = std::move(listing);
    refreshRows();
    return;
  }
  m_back.push_back(current());
  m_forward.clear();
  navigate({.path = m_location, .listing = std::move(listing)}, Motion::Fade);
}

void FileBrowser::back() {
  if (m_back.empty()) {
    return;
  }
  m_forward.push_back(current());
  HistoryEntry target = std::move(m_back.back());
  m_back.pop_back();
  navigate(std::move(target), Motion::Back);
}

void FileBrowser::forward() {
  if (m_forward.empty()) {
    return;
  }
  m_back.push_back(current());
  HistoryEntry target = std::move(m_forward.back());
  m_forward.pop_back();
  navigate(std::move(target), Motion::Deeper);
}

void FileBrowser::up() {
  if (m_listing.has_value() || !m_location.has_relative_path()) {
    return;
  }
  const std::string child = m_location.filename().string();
  go(m_location.parent_path());
  m_pendingSelect = {child}; // the folder we came from gets the cursor
}

void FileBrowser::navigate(HistoryEntry target, Motion motion) {
  m_location = std::move(target.path);
  m_listing = std::move(target.listing);
  m_listingId = m_listing.has_value() ? m_listing->id : std::string();
  m_selected.clear();
  m_anchor.reset();
  m_cursor.reset();
  m_pendingSelect.clear();
  m_typeAhead.reset();
  m_pendingMotion = motion;
  m_skeletonTimer.stop();
  kLog.debug("location {}", m_listing.has_value() ? m_listingId : m_location.string());
  attachModel();
  refreshSidebar();
  notifyChanged();
}

void FileBrowser::attachModel() {
  m_modelView.reset();
  m_model.reset();
  if (m_listing.has_value()) {
    present(m_pendingMotion);
    return;
  }
  m_model = FileModel::open(m_location, m_watcher);
  m_modelView = std::make_unique<FileModelView>(m_model);
  m_modelView->setSort(m_sort);
  m_modelView->setShowHidden(m_options.showHidden);
  m_modelView->setChangedCallback([this] { onModelChanged(); });
  if (m_model->status() != FileModel::Status::Loading) {
    present(m_pendingMotion);
    return;
  }
  // Keep the old listing on screen until the new one lands; a slow listing gets a skeleton after 200 ms.
  m_waiting = true;
  m_skeletonTimer.start(kSkeletonDelay, [this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired() || !m_waiting) {
      return;
    }
    present(Motion::None);
    showSkeleton(true);
  });
}

void FileBrowser::onModelChanged() {
  if (m_model == nullptr || m_model->status() == FileModel::Status::Loading) {
    return;
  }
  if (m_waiting) {
    present(m_pendingMotion);
    return;
  }
  if (m_skeletonShown && m_skeletonHeld) {
    return; // the hold timer presents
  }
  refreshRows();
}

void FileBrowser::present(Motion motion) {
  const bool loaded = m_listing.has_value() || (m_model != nullptr && m_model->status() != FileModel::Status::Loading);
  if (loaded) {
    m_waiting = false;
    m_skeletonTimer.stop();
    if (m_skeletonShown && m_skeletonHeld) {
      return; // the hold timer presents
    }
    showSkeleton(false);
  }
  m_pendingMotion = Motion::None;
  if (MotionService::instance().reduced() && (motion == Motion::Deeper || motion == Motion::Back)) {
    motion = Motion::Fade;
  }
  Ui& ui = *m_ui;
  if (motion != Motion::None && ui.stage != nullptr && ui.stageWidth > 0.0F) {
    // The current page becomes the outgoing one; the other page takes the new listing.
    Page& out = ui.cur();
    out.iconsAdapter->setLive(false);
    out.listAdapter->setLive(false);
    ui.current = 1 - ui.current;
  }
  Page& in = ui.cur();
  in.iconsAdapter->setLive(true);
  in.listAdapter->setLive(true);
  in.iconsScroll = {};
  in.listScroll = {};
  refreshRows();

  Page& out = ui.other();
  const float w = ui.stageWidth;
  const float gap = kSlideGap * ui.scale;
  const auto animateX = [this](Page& page, float from, float to, std::string_view label) {
    page.x = from;
    MotionTraceLabel trace(label);
    page.xAnim = m_animations.drive(
        page.xAnim, from, to, Style::spring(Tokens::springPage),
        [this, &page](float v) {
          page.x = v;
          if (page.root != nullptr) {
            page.root->setPosition(v, 0.0F);
          }
        },
        {}, page.root, 0.0F, Unit::Px, true
    );
  };
  const auto animateOpacity = [this](Page& page, float from, float to, const Tokens::Spring& spring,
                                     std::string_view label, std::function<void()> done) {
    page.opacity = from;
    MotionTraceLabel trace(label);
    page.fadeAnim = m_animations.drive(
        page.fadeAnim, from, to, Style::spring(spring),
        [&page](float v) {
          page.opacity = v;
          if (page.root != nullptr) {
            page.root->setOpacity(v);
          }
        },
        std::move(done), page.root
    );
  };
  const auto hideOut = [this] {
    Page& gone = m_ui->other();
    gone.root->setVisible(false);
    gone.rows.clear();
    gone.icons->notifyDataChanged();
    gone.list->notifyDataChanged();
  };

  if (in.root != nullptr) {
    in.root->setVisible(true);
    in.root->setZIndex(1);
  }
  if (motion == Motion::None || out.root == nullptr || in.root == nullptr) {
    in.x = 0.0F;
    in.opacity = 1.0F;
    if (in.root != nullptr) {
      in.root->setPosition(0.0F, 0.0F);
      in.root->setOpacity(1.0F);
    }
  } else if (motion == Motion::Fade) {
    in.x = 0.0F;
    out.x = 0.0F;
    out.root->setZIndex(0);
    animateOpacity(out, out.opacity, 0.0F, Tokens::springFadeOut, "files.page.out", {});
    animateOpacity(in, 0.0F, 1.0F, Tokens::springFade, "files.page.in", hideOut);
  } else {
    const bool deeper = motion == Motion::Deeper;
    // Deeper: the new page enters from the trailing edge over the old one, which drifts 30 % and dims 10 %.
    out.root->setZIndex(deeper ? 0 : 2);
    in.root->setZIndex(deeper ? 2 : 0);
    out.opacity = 1.0F;
    out.root->setOpacity(1.0F);
    if (deeper) {
      animateX(in, w + gap, 0.0F, "files.page");
      animateX(out, 0.0F, -w * kParallax, "files.page.under");
      animateOpacity(out, 1.0F, kUnderDim, Tokens::springPage, "files.page.dim", {});
      in.opacity = 1.0F;
      in.root->setOpacity(1.0F);
    } else {
      animateX(in, -w * kParallax, 0.0F, "files.page.under");
      animateOpacity(in, kUnderDim, 1.0F, Tokens::springPage, "files.page.dim", {});
      animateX(out, 0.0F, w + gap, "files.page");
    }
    // Settle: the outgoing page leaves the scene once the incoming one is home.
    m_animations.animateSpring(0.0F, 1.0F, Style::spring(Tokens::springPage), {}, hideOut, in.root);
  }
  refreshChrome();
  notifyChanged();
}

void FileBrowser::showSkeleton(bool show) {
  Page& page = m_ui->cur();
  m_skeletonShown = show;
  if (show && !m_skeletonHeld) {
    m_skeletonHeld = true;
    m_skeletonHoldTimer.start(kSkeletonHold, [this, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      m_skeletonHeld = false;
      if (!m_waiting) {
        present(Motion::Fade);
      }
    });
  }
  if (!show) {
    m_skeletonHeld = false;
    m_skeletonHoldTimer.stop();
  }
  if (page.skeleton == nullptr) {
    return;
  }
  page.skeleton->setVisible(show);
  while (!page.skeleton->children().empty()) {
    (void)page.skeleton->removeChild(page.skeleton->children().back().get());
  }
  if (!show) {
    return;
  }
  const float s = m_ui->scale;
  const float pad = kContentPad * s;
  const float w = std::max(m_ui->stageWidth, 1.0F);
  if (m_options.view == View::Icons) {
    const float cw = FileIconCell::kWidth * s;
    const float ch = FileIconCell::kHeight * s;
    const float g = kIconGap * s;
    const auto cols = static_cast<std::size_t>(std::max(1.0F, std::floor((w - 2.0F * pad + g) / (cw + g))));
    for (std::size_t i = 0; i < cols * 3; ++i) {
      page.skeleton->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Fill1),
          .radius = Style::scaledRadius(8.0F, s),
          .width = cw,
          .height = ch,
          .configure =
              [=](Box& b) {
                b.setPosition(pad + static_cast<float>(i % cols) * (cw + g), pad + static_cast<float>(i / cols) * (ch + g));
              },
      }));
    }
  } else {
    const float rh = std::ceil(FileListRow::kHeight * s);
    for (std::size_t i = 0; i < 8; ++i) {
      const float y = (kListHeader + 1.0F) * s + static_cast<float>(i) * rh + 10.0F * s;
      for (const auto& [x, bw] : {std::pair{16.0F, 0.40F}, std::pair{0.62F, 0.14F}, std::pair{0.82F, 0.10F}}) {
        const float left = x < 1.0F ? w * x : x * s;
        page.skeleton->addChild(ui::box({
            .fill = colorSpecFromRole(ColorRole::Fill1),
            .radius = Style::scaledRadius(4.0F, s),
            .width = w * bw,
            .height = 12.0F * s,
            .configure = [=](Box& b) { b.setPosition(left, y); },
        }));
      }
    }
  }
  if (show) {
    page.icons->setVisible(false);
    page.listBox->setVisible(false);
    page.empty->setVisible(false);
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

// ---- rows ---------------------------------------------------------------------------------------------------------

FileModel::Status FileBrowser::status() const {
  if (m_listing.has_value()) {
    return FileModel::Status::Ready;
  }
  return m_model != nullptr ? m_model->status() : FileModel::Status::Loading;
}

void FileBrowser::refreshRows() {
  if (m_listing.has_value()) {
    auto rows = m_listing->rows;
    std::ranges::stable_sort(rows, [this](const auto& a, const auto& b) { return FileModelView::before(*a, *b, m_sort); });
    if (!m_options.showHidden) {
      std::erase_if(rows, [](const auto& r) { return r->hidden; });
    }
    m_rows = std::move(rows);
  } else {
    m_rows = m_modelView != nullptr && !m_waiting ? m_modelView->rows() : std::vector<std::shared_ptr<const FileInfo>>{};
  }
  std::erase_if(m_selected, [this](const std::string& name) { return !indexOf(name).has_value(); });

  Page& page = m_ui->cur();
  page.rows = m_rows;
  const FileModel::Status st = status();
  page.status = st == FileModel::Status::Denied   ? "denied"
      : st == FileModel::Status::Missing          ? "missing"
      : st == FileModel::Status::Error            ? "error"
      : st == FileModel::Status::Ready && m_rows.empty() ? "empty"
                                                  : "";
  if (page.icons != nullptr) {
    page.icons->notifyDataChanged();
    page.list->notifyDataChanged();
    const bool showRows = page.status.empty() && !m_skeletonShown;
    page.empty->setVisible(!page.status.empty() && !m_skeletonShown);
    page.icons->setVisible(showRows && (m_options.view == View::Icons || m_ui->viewAnim != 0));
    page.listBox->setVisible(showRows && (m_options.view == View::List || m_ui->viewAnim != 0));
    const auto setEmpty = [&](const char* glyph, std::string title, std::string body) {
      page.emptyGlyph->setGlyph(glyph);
      page.emptyTitle->setText(title);
      page.emptyBody->setText(body);
    };
    if (page.status == "empty") {
      setEmpty(m_listingId == "trash" ? "trash" : "folder", m_listingId == "trash" ? "Trash is empty" : "Folder is empty",
               m_listing.has_value() ? std::string() : std::string("Drop files here or press Ctrl+V."));
    } else if (page.status == "denied") {
      setEmpty("lock", "No permission", "You can’t see the contents of this folder.");
    } else if (page.status == "missing") {
      setEmpty("folder-off", "Folder not found", std::format("“{}” no longer exists.", m_location.filename().string()));
    } else if (page.status == "error") {
      setEmpty("alert-triangle", "Can’t read this folder", m_model != nullptr ? m_model->error() : std::string());
    }
  }

  if (!m_pendingSelect.empty() && st != FileModel::Status::Loading) {
    const auto names = std::exchange(m_pendingSelect, {});
    selectNames(names);
    return;
  }
  refreshChrome();
  notifyChanged();
}

void FileBrowser::rebindPages() {
  Page& page = m_ui->cur();
  if (page.icons != nullptr) {
    page.icons->notifyDataChanged();
    page.list->notifyDataChanged();
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileBrowser::refreshChrome() {
  Ui& ui = *m_ui;
  const float s = ui.scale;
  if (ui.viewSwitch != nullptr) {
    ui.viewSwitch->setSelectedIndex(static_cast<std::size_t>(m_options.view));
  }
  Page& page = ui.cur();
  static constexpr std::array kKeys{FileSortKey::Name, FileSortKey::Modified, FileSortKey::Size, FileSortKey::Kind};
  static constexpr std::array kNames{"Name", "Date Modified", "Size", "Kind"};
  const FileListRow::Columns cols = FileListRow::columnsFor(ui.stageWidth > 0.0F ? ui.stageWidth : 1000.0F, s);
  const std::array<float, 4> widths{0.0F, cols.date, cols.size, cols.kind};
  for (std::size_t c = 0; c < kKeys.size(); ++c) {
    Button* b = page.sortButtons[c];
    if (b == nullptr) {
      continue;
    }
    const bool active = m_sort.key == kKeys[c];
    b->setText(active ? std::format("{} {}", kNames[c], m_sort.descending ? "↓" : "↑") : std::string(kNames[c]));
    if (c > 0) {
      b->setVisible(widths[c] > 0.0F);
      b->setParticipatesInLayout(widths[c] > 0.0F);
      b->setMinWidth(widths[c]);
      b->setMaxWidth(widths[c]);
    }
  }

  if (ui.pathRow != nullptr) {
    while (!ui.pathRow->children().empty()) {
      (void)ui.pathRow->removeChild(ui.pathRow->children().back().get());
    }
    if (m_listing.has_value()) {
      ui.pathRow->addChild(ui::label({.text = m_listing->title, .fontSize = Style::fontSizeCaption * s,
                                      .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
    } else {
      const auto segments = pathSegments(m_location);
      for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
          ui.pathRow->addChild(ui::glyph({.glyph = std::string("chevron-right"), .glyphSize = 12.0F * s,
                                          .color = colorSpecFromRole(ColorRole::Label3)}));
        }
        const bool last = i + 1 == segments.size();
        ui.pathRow->addChild(ui::button({
            .text = segments[i].first,
            .fontSize = Style::fontSizeCaption * s,
            .variant = ButtonVariant::Ghost,
            .paddingH = 4.0F * s,
            .radius = Style::scaledRadius(4.0F, s),
            .height = 24.0F * s,
            .onClick = [this, path = segments[i].second, last] {
              if (!last) {
                go(path);
              }
            },
        }));
      }
    }
  }
  if (ui.status != nullptr) {
    std::string text = m_selected.empty() ? (m_rows.size() == 1 ? std::string("1 item") : std::format("{} items", m_rows.size()))
                                          : std::format("{} of {} selected", m_selected.size(), m_rows.size());
    if (!m_listing.has_value()) {
      if (const auto free = freeBytes(m_location)) {
        text += std::format(" · {} free", formatFileSize(*free));
      }
    }
    ui.status->setText(text);
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

std::string FileBrowser::title() const {
  if (m_listing.has_value()) {
    return m_listing->title;
  }
  if (m_location == fs::path(g_get_home_dir())) {
    return homeName();
  }
  return m_location.has_filename() ? m_location.filename().string() : m_location.string();
}

fs::path FileBrowser::pathOf(const FileInfo& info) const {
  const fs::path name(info.name);
  return name.is_absolute() ? name : m_location / name;
}

std::vector<fs::path> FileBrowser::selectedPaths() const {
  std::vector<fs::path> out;
  for (const auto& row : m_rows) {
    if (m_selected.contains(row->name)) {
      out.push_back(pathOf(*row));
    }
  }
  return out;
}

std::vector<std::shared_ptr<const FileInfo>> FileBrowser::selectedRows() const {
  std::vector<std::shared_ptr<const FileInfo>> out;
  for (const auto& row : m_rows) {
    if (m_selected.contains(row->name)) {
      out.push_back(row);
    }
  }
  return out;
}

std::optional<std::size_t> FileBrowser::indexOf(const std::string& name) const {
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    if (m_rows[i]->name == name) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FileBrowser::cursorIndex() const {
  return m_cursor.has_value() ? indexOf(*m_cursor) : std::nullopt;
}

bool FileBrowser::rowEnabled(const FileInfo& info) const {
  return info.isDir || !m_options.enabled || m_options.enabled(info);
}

void FileBrowser::setView(View view) {
  if (view == m_options.view) {
    return;
  }
  m_options.view = view;
  Ui& ui = *m_ui;
  MotionTraceLabel trace("files.view");
  ui.viewMix = 0.0F;
  ui.viewAnim = m_animations.drive(
      ui.viewAnim, 0.0F, 1.0F, Style::spring(Tokens::springFade),
      [this](float v) {
        m_ui->viewMix = v;
        if (m_host.invalidate) {
          m_host.invalidate();
        }
      },
      [this] {
        m_ui->viewAnim = 0;
        refreshRows();
      },
      m_ui->stage
  );
  refreshRows();
  if (const auto c = cursorIndex()) {
    setCursor(*c, true);
  }
  if (m_skeletonShown) {
    showSkeleton(true);
  }
  refreshChrome();
}

void FileBrowser::setSort(FileSortKey key) {
  if (m_sort.key == key) {
    m_sort.descending = !m_sort.descending;
  } else {
    m_sort.key = key;
    m_sort.descending = key == FileSortKey::Modified || key == FileSortKey::Size;
  }
  if (m_modelView != nullptr) {
    m_modelView->setSort(m_sort);
  } else {
    refreshRows();
  }
}

void FileBrowser::setShowHidden(bool show) {
  m_options.showHidden = show;
  if (m_modelView != nullptr) {
    m_modelView->setShowHidden(show);
  } else {
    refreshRows();
  }
}

void FileBrowser::setExtraPlaces(std::vector<Place> places) {
  m_options.extraPlaces = std::move(places);
  refreshSidebar();
}

void FileBrowser::setEnabledPredicate(std::function<bool(const FileInfo&)> enabled) {
  m_options.enabled = std::move(enabled);
  rebindPages();
}

void FileBrowser::reload() {
  if (m_model != nullptr) {
    m_model->reload();
  }
}

void FileBrowser::notifyChanged() {
  if (m_host.changed) {
    m_host.changed();
  }
}

// ---- selection ----------------------------------------------------------------------------------------------------

void FileBrowser::select(std::size_t index, std::uint32_t modifiers) {
  if (index >= m_rows.size() || !rowEnabled(*m_rows[index])) {
    return;
  }
  const std::string& name = m_rows[index]->name;
  const std::optional<std::size_t> anchor = m_anchor.has_value() ? indexOf(*m_anchor) : std::nullopt;
  if ((modifiers & KeyMod::Shift) != 0 && anchor.has_value()) {
    if ((modifiers & KeyMod::Ctrl) == 0) {
      m_selected.clear();
    }
    const auto [lo, hi] = std::minmax(*anchor, index);
    for (std::size_t i = lo; i <= hi; ++i) {
      if (rowEnabled(*m_rows[i])) {
        m_selected.insert(m_rows[i]->name);
      }
    }
  } else if ((modifiers & KeyMod::Ctrl) != 0) {
    if (m_selected.erase(name) == 0) {
      m_selected.insert(name);
    }
    m_anchor = name;
  } else {
    m_selected = {name};
    m_anchor = name;
  }
  setCursor(index, true);
  kLog.debug("cursor {} selected {}", name, m_selected.size());
  refreshChrome();
  notifyChanged();
}

void FileBrowser::setCursor(std::size_t index, bool scroll) {
  if (index >= m_rows.size()) {
    return;
  }
  m_cursor = m_rows[index]->name;
  Page& page = m_ui->cur();
  if (scroll && page.icons != nullptr) {
    (m_options.view == View::Icons ? page.icons : page.list)->scrollToIndex(index);
  }
  rebindPages();
}

void FileBrowser::selectNames(const std::vector<std::string>& names) {
  if (status() == FileModel::Status::Loading || m_waiting) {
    m_pendingSelect = names;
    return;
  }
  m_selected.clear();
  std::optional<std::size_t> first;
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    const auto& row = m_rows[i];
    if (rowEnabled(*row) && (std::ranges::contains(names, row->name) || std::ranges::contains(names, row->displayName))) {
      m_selected.insert(row->name);
      m_anchor = row->name;
      first = first.value_or(i);
    }
  }
  if (first.has_value()) {
    setCursor(*first, true);
  }
  kLog.debug("selected {} of {} in {}", m_selected.size(), names.size(), m_listing.has_value() ? m_listingId : m_location.string());
  refreshChrome();
  notifyChanged();
}

void FileBrowser::selectAll() {
  for (const auto& row : m_rows) {
    if (rowEnabled(*row)) {
      m_selected.insert(row->name);
    }
  }
  rebindPages();
  refreshChrome();
  notifyChanged();
}

void FileBrowser::clearSelection() {
  if (m_selected.empty()) {
    return;
  }
  m_selected.clear();
  rebindPages();
  refreshChrome();
  notifyChanged();
}

void FileBrowser::onItemPress(std::size_t index) {
  if (index >= m_rows.size()) {
    return;
  }
  m_keyboardCursor = false;
  const auto now = std::chrono::steady_clock::now();
  const std::string& name = m_rows[index]->name;
  const bool second = name == m_lastPressName && now - m_lastPressAt < kDoubleClick;
  m_lastPressName = second ? std::string() : name;
  m_lastPressAt = now;
  const std::uint32_t mods = m_host.modifiers ? m_host.modifiers() : 0;
  if (!second || !m_selected.contains(name)) {
    select(index, mods);
  }
  if (second) {
    Page& page = m_ui->cur();
    (m_options.view == View::Icons ? page.iconsAdapter : page.listAdapter)->armActivate(index);
  }
}

void FileBrowser::onItemActivate(std::size_t index) {
  if (index >= m_rows.size()) {
    return;
  }
  const FileInfo& info = *m_rows[index];
  if (!rowEnabled(info)) {
    return;
  }
  if (info.isDir && !m_listing.has_value()) {
    go(pathOf(info));
  } else if (info.isDir) {
    jump(pathOf(info));
  } else if (m_host.openFile) {
    m_host.openFile(pathOf(info));
  }
}

std::size_t FileBrowser::columns() const {
  const Page& page = m_ui->pages[m_ui->current];
  if (m_options.view == View::Icons && page.icons != nullptr) {
    return std::max<std::size_t>(1, page.icons->layoutColumns());
  }
  return 1;
}

void FileBrowser::moveCursor(std::ptrdiff_t delta, std::uint32_t modifiers) {
  if (m_rows.empty()) {
    return;
  }
  const auto cur = cursorIndex();
  std::ptrdiff_t next = cur.has_value() ? static_cast<std::ptrdiff_t>(*cur) + delta : 0;
  next = std::clamp<std::ptrdiff_t>(next, 0, static_cast<std::ptrdiff_t>(m_rows.size()) - 1);
  const auto index = static_cast<std::size_t>(next);
  if ((modifiers & KeyMod::Ctrl) != 0 && (modifiers & KeyMod::Shift) == 0) {
    setCursor(index, true); // Ctrl+Arrows move the cursor only
    return;
  }
  select(index, modifiers & KeyMod::Shift);
}

bool FileBrowser::onKey(const KeyboardEvent& event) {
  if (!event.pressed) {
    return false;
  }
  const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt);
  m_keyboardCursor = true;
  const bool ctrl = (mods & KeyMod::Ctrl) != 0;
  const bool alt = (mods & KeyMod::Alt) != 0;
  const auto cols = static_cast<std::ptrdiff_t>(columns());
  const Page& page = m_ui->cur();
  const auto pageRows = static_cast<std::ptrdiff_t>(
      page.icons != nullptr ? (m_options.view == View::Icons ? page.icons : page.list)->pageItemStride() : 10);

  switch (event.sym) {
  case XKB_KEY_Left:
    if (alt) {
      back();
    } else if (m_options.view == View::Icons) {
      moveCursor(-1, mods);
    } else {
      return false;
    }
    return true;
  case XKB_KEY_Right:
    if (alt) {
      forward();
    } else if (m_options.view == View::Icons) {
      moveCursor(1, mods);
    } else {
      return false;
    }
    return true;
  case XKB_KEY_Up:
    if (alt) {
      up();
    } else {
      moveCursor(-cols, mods);
    }
    return true;
  case XKB_KEY_Down:
    if (alt || ctrl) {
      if (const auto c = cursorIndex()) {
        onItemActivate(*c);
      }
    } else {
      moveCursor(cols, mods);
    }
    return true;
  case XKB_KEY_Home:
    if (alt) {
      jump(g_get_home_dir());
    } else {
      moveCursor(-static_cast<std::ptrdiff_t>(m_rows.size()), mods);
    }
    return true;
  case XKB_KEY_End:
    moveCursor(static_cast<std::ptrdiff_t>(m_rows.size()), mods);
    return true;
  case XKB_KEY_Page_Up:
    moveCursor(-pageRows, mods);
    return true;
  case XKB_KEY_Page_Down:
    moveCursor(pageRows, mods);
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (m_selected.size() == 1 || !m_selected.empty()) {
      const auto rows = selectedRows();
      if (rows.size() == 1) {
        if (const auto i = indexOf(rows.front()->name)) {
          onItemActivate(*i);
        }
      } else if (m_host.openFile) {
        for (const auto& row : rows) {
          if (!row->isDir) {
            m_host.openFile(pathOf(*row));
          }
        }
      }
    }
    return true;
  case XKB_KEY_BackSpace:
    up();
    return true;
  case XKB_KEY_Escape:
    if (m_selected.empty()) {
      return false;
    }
    clearSelection();
    return true;
  case XKB_KEY_F5:
    reload();
    return true;
  case XKB_KEY_space:
    if (ctrl) {
      if (const auto c = cursorIndex()) {
        select(*c, KeyMod::Ctrl);
      }
      return true;
    }
    break;
  default:
    break;
  }
  if (ctrl && !alt) {
    switch (event.sym) {
    case XKB_KEY_1:
      setView(View::Icons);
      return true;
    case XKB_KEY_2:
      setView(View::List);
      return true;
    case XKB_KEY_a:
    case XKB_KEY_A:
      selectAll();
      return true;
    case XKB_KEY_h:
    case XKB_KEY_H:
      setShowHidden(!m_options.showHidden);
      return true;
    case XKB_KEY_r:
    case XKB_KEY_R:
      reload();
      return true;
    default:
      return false;
    }
  }
  if (event.utf32 >= 0x20 && event.utf32 != 0x7F && !ctrl && !alt) {
    if (event.utf32 == U' ' && !m_typeAhead.active()) {
      return false; // Quick Look (FB2)
    }
    const auto hit = m_typeAhead.feed(
        static_cast<char32_t>(event.utf32), m_rows.size(),
        [this](std::size_t i) -> std::string_view { return m_rows[i]->displayName; }, cursorIndex()
    );
    if (hit.has_value()) {
      select(*hit, 0);
    }
    return true;
  }
  return false;
}

#include "ui/files/file_browser.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/accessible.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/files/file_cells.h"
#include "ui/files/file_format.h"
#include "ui/files/file_columns.h"
#include "ui/files/file_gallery.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <glib.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  std::string whereLabel(const fs::path& dir);
  bool matchesKind(const FileInfo& info, FileBrowser::SearchKind kind);
  constexpr Logger kLog("file-browser");
  constexpr auto kSkeletonDelay = std::chrono::milliseconds(200);
  constexpr auto kSkeletonHold = std::chrono::milliseconds(400);
  constexpr auto kDoubleClick = std::chrono::milliseconds(400);
  constexpr auto kSpringLoad = std::chrono::milliseconds(700); // FILES-UX §2.3
  const float& kSlideGap = Tokens::spaceP32; // P12: incoming and outgoing 32 apart
  constexpr float kParallax = 0.30F;          // the page underneath moves 30 %
  constexpr float kUnderDim = 0.90F;          // and dims 10 %
  const float& kSidebarRow = Tokens::componentSidebarRow;
  const float& kPathBar = Tokens::sizeRowCompact; // FILES-UX §0.2: compact row, not 28
  const float& kListHeader = Tokens::componentListTableHeader;
  const float& kIconGap = Tokens::componentGridGap;
  const float& kContentPad = Tokens::componentFilesGridPadding;

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
  // A sidebar row: a flex body with an input area over all of it (hover, click, drop target).
  class SidebarRow final : public Flex {
  public:
    InputArea* area = nullptr;

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      placeArea();
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      placeArea();
    }

  private:
    void placeArea() {
      if (area != nullptr) {
        area->setPosition(0.0F, 0.0F);
        area->setSize(width(), height());
      }
    }
  };

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
    if (info.pendingStat && m_live) {
      m_browser.wantStat((*m_rows)[index]);
    }
    FileCellState state = m_state && m_live ? m_state(info, index) : FileCellState{};
    state.hovered = hovered && !state.selected && !state.disabled;
    if (m_icons) {
      static_cast<FileIconCell&>(tile).bind(*m_renderer, info, tile.width(), tile.height(), state);
    } else {
      static_cast<FileListRow&>(tile).bind(*m_renderer, info, tile.width(), state);
    }
  }
  bool onPointerPress(std::size_t index, float x, float y, float, float) override {
    m_band = false;
    if (!m_icons && m_live && m_rows != nullptr && index < m_rows->size() && (*m_rows)[index]->isDir &&
        m_browser.listExpandable()) {
      const FileInfo& row = *(*m_rows)[index];
      const auto [from, to] = FileListRow::disclosureSpan(static_cast<int>(std::ranges::count(row.name, '/')), m_scale);
      if (x >= from && x < to) {
        m_pressIndex.reset();
        m_browser.setExpanded(index, !m_browser.expanded(row.name));
        return true;
      }
    }
    m_pressIndex = index;
    if (m_live) {
      m_browser.onItemPress(index);
      if (m_icons) {
        m_browser.pressItem(index);
      }
    }
    (void)x;
    (void)y;
    return true;
  }
  bool onEmptyPointerPress(float x, float y) override {
    if (!m_live) {
      return false;
    }
    m_band = true;
    m_pressIndex.reset();
    m_browser.onBandStart(m_icons, x, y);
    return true;
  }
  bool onPointerDrag(std::optional<std::size_t> /*index*/, float x, float y, float, float) override {
    if (!m_live) {
      return false;
    }
    if (m_band) {
      m_browser.onBandMove(m_icons, x, y);
    } else if (m_pressIndex.has_value()) {
      m_browser.pressItem(std::nullopt);
      m_browser.onItemDrag(*m_pressIndex);
      m_pressIndex.reset();
    }
    return false;
  }
  PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
    PointerReleaseResult result;
    if (m_live && m_icons) {
      m_browser.pressItem(std::nullopt);
    }
    if (m_band) {
      m_band = false;
      m_browser.onBandEnd(m_icons);
      return result;
    }
    if (m_live && index.has_value() && m_pressIndex == index) {
      m_browser.onItemRelease(*index);
    }
    if (m_live && index.has_value() && m_activate == index) {
      result.activateIndex = index;
    }
    m_activate.reset();
    m_pressIndex.reset();
    return result;
  }
  void onPointerCancel() override {
    if (m_live && m_icons) {
      m_browser.pressItem(std::nullopt);
    }
    if (m_band) {
      m_band = false;
      m_browser.onBandEnd(m_icons);
    }
    m_pressIndex.reset();
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
  void onSecondaryEmpty(float x, float y) override {
    if (m_live) {
      m_browser.backgroundMenu(x, y);
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
  std::optional<std::size_t> m_pressIndex; // a press on an item that may become a drag
  bool m_band = false;
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
  Node* custom = nullptr;  // Listing::page while that listing shows
  std::string customId;    // its listing id
  Node* banner = nullptr;  // Listing::banner
  std::string bannerId;
  Glyph* emptyGlyph = nullptr;
  Label* emptyTitle = nullptr;
  Label* emptyBody = nullptr;
  Button* emptyAction = nullptr; // "Search This PC" under no results
  std::array<Button*, 4> sortButtons{};
  Button* whereHeader = nullptr; // search results (mockup §5): Where after Name

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
  Flex* crumbRow = nullptr; // split pane only
  Label* status = nullptr;
  std::array<Button*, 4> viewButtons{};
  std::array<Box*, 4> viewShadows{};
  std::array<Box*, 4> galleryMarks{}; // the screen outline, then the three filmstrip dashes
  Flex* search = nullptr;
  Slider* zoom = nullptr;
  Button* sortButton = nullptr;
  Flex* sidebarList = nullptr;
  float sidebarWidth = 0.0F;
  bool searchFocused = false;
  std::vector<std::pair<Node*, Place>> sidebarRows;           // drop targets
  std::vector<std::pair<Node*, fs::path>> segments;           // path-bar drop targets
  Node* renameBox = nullptr;
  Node* columnsNode = nullptr;
  Flex* tabStrip = nullptr;
  Flex* chips = nullptr; // search scope + kind chips (FILES-UI-SPEC §15)
  std::vector<std::pair<Node*, std::string>> chipNodes;
  FileGallery* gallery = nullptr;
  Input* renameField = nullptr;
  Input* searchField = nullptr;
  Button* searchClear = nullptr; // the field's × while it has text (mockup §5)
  Label* renameError = nullptr;

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
        state.dropTarget = m_dropItem == info.name;
        state.path = pathOf(info);
        state.thumbnails = m_options.thumbnails;
        state.inactive = !m_active;
        state.iconSize = m_iconSize;
        if (searchingEverywhere()) {
          state.where = whereLabel(pathOf(info).parent_path());
        }
        if (expandable()) {
          state.depth = static_cast<int>(std::ranges::count(info.name, '/'));
          state.disclosure = !info.isDir                    ? FileCellState::Disclosure::Leaf
                             : m_expanded.contains(info.name) ? FileCellState::Disclosure::Expanded
                                                              : FileCellState::Disclosure::Collapsed;
        }
        if (!m_pressName.empty() && info.name == m_pressName) {
          state.pressScale = m_pressScale;
        }
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
  m_peekListener = FileIcon::addPeekListener([this] { rebindPages(); });
  if (m_options.thumbnails != nullptr) {
    // A finished decode only wakes us: the next layout uploads it with the render context current.
    auto sub = std::make_shared<ThumbnailService::Subscription>(m_options.thumbnails->subscribePendingUpload([this] {
      if (m_host.invalidate) {
        m_host.invalidate();
      }
    }));
    m_thumbWake = sub;
  }
  m_columns = std::make_unique<FileColumns>(
      m_watcher, m_animations,
      FileColumns::Host{
          .rows = [this]() -> const std::vector<std::shared_ptr<const FileInfo>>& { return m_rows; },
          .state =
              [this](const FileInfo& info, std::size_t) {
                FileCellState state;
                state.selected = m_selected.contains(info.name);
                state.cursor = m_keyboardCursor && m_cursor == info.name;
                state.disabled = !rowEnabled(info);
                state.ghost = info.hidden;
                state.dropTarget = m_dropItem == info.name;
                state.path = pathOf(info);
                state.thumbnails = m_options.thumbnails;
                state.inactive = state.inactive || !m_active;
                return state;
              },
          .press = [this](std::size_t index) { onItemPress(index); },
          .activate = [this](std::size_t index) { onItemActivate(index); },
          .goSelect =
              [this](const fs::path& dir, const std::string& name) {
                if (dir == m_location && !m_listing.has_value()) {
                  selectNames({name});
                } else {
                  goSelecting(dir, {name});
                }
              },
          .invalidate =
              [this] {
                if (m_host.invalidate) {
                  m_host.invalidate();
                }
              },
          .thumbnails = m_options.thumbnails,
      });
}

FileBrowser::~FileBrowser() {
  m_thumbWake.reset();
  FileIcon::removePeekListener(m_peekListener);
  *m_alive = false;
}

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
  // Right-aligned in the header; its minimum keeps the search at search_width_min (the header ellipsizes the title
  // only past that).
  const float viewsW = (Tokens::componentFilesViewItemWidth * 4.0F + Tokens::componentFilesViewPadding * 5.0F) * s;
  const float gap = Tokens::spaceP8 * s;
  auto row = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = gap,
                      .minWidth = viewsW + Tokens::sizeControlM * s + Tokens::componentFilesSearchWidthMin * s + gap * 2.0F});
  // Mockup .seg: 32 high r16 fill_1, padding 2, gap 2; items 36 x 28 r14; the current one = lens + 0.5 stroke.
  const float itemH = Tokens::componentFilesViewItemHeight * s;
  auto views = ui::row({.align = FlexAlign::Center, .gap = Tokens::componentFilesViewPadding * s,
                        .padding = Tokens::componentFilesViewPadding * s, .fill = colorSpecFromRole(ColorRole::Fill1),
                        .radius = Tokens::sizeControlM * s / 2.0F, .height = Tokens::sizeControlM * s});
  const std::array<std::pair<const char*, const char*>, 4> items{{{"layout-grid", "Icons (Ctrl+1)"},
                                                                 {"list", "List (Ctrl+2)"},
                                                                 {"columns-3", "Columns (Ctrl+3)"},
                                                                 {"", "Gallery (Ctrl+4)"}}};
  const float itemW = Tokens::componentFilesViewItemWidth * s;
  for (std::size_t i = 0; i < items.size(); ++i) {
    // The current item's shadow (mockup .seg span.on: 0 1 2) is a soft box under the button.
    auto slot = ui::row({.align = FlexAlign::Center, .width = itemW, .height = itemH});
    auto shadow = ui::box({.out = &m_ui->viewShadows[i], .radius = itemH / 2.0F, .participatesInLayout = false});
    shadow->setPosition(0.0F, s); // 1 px down
    shadow->setSize(itemW, itemH);
    shadow->setSoftness(Tokens::spaceP2 * s);
    slot->addChild(std::move(shadow));
    slot->addChild(ui::button({
        .out = &m_ui->viewButtons[i],
        .glyph = *items[i].first != '\0' ? std::optional<std::string>(items[i].first) : std::nullopt,
        .glyphSize = Tokens::sizeSymbol * s,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::string(items[i].second),
        .radius = itemH / 2.0F,
        .width = itemW,
        .height = itemH,
        .onClick = [this, i] { setView(static_cast<View>(i)); },
    }));
    if (*items[i].first == '\0') {
      // Mockup #gal: a screen with a filmstrip under it (no tabler glyph draws it); drawn over the button, centred in
      // the slot, and presses go to the button below.
      const float screenW = Tokens::sizeSymbolInline * s * 1.25F;
      const float screenH = Tokens::spaceP8 * s;
      const float dashW = Tokens::spaceP2 * s * 1.25F;
      const float dashH = Tokens::spaceP2 * s * 0.75F;
      const float dashGap = Tokens::spaceP2 * s * 1.5F;
      const float x0 = std::round((itemW - screenW) / 2.0F);
      const float y0 = std::round((itemH - screenH - Tokens::spaceP2 * s - dashH) / 2.0F);
      const auto place = [&](std::size_t d, float x, float y, float w, float h, float r) {
        auto box = ui::box({.out = &m_ui->galleryMarks[d], .radius = r, .participatesInLayout = false});
        box->setHitTestVisible(false);
        box->setPosition(x, y);
        box->setSize(w, h);
        slot->addChild(std::move(box));
      };
      place(0, x0, y0, screenW, screenH, Tokens::spaceP2 * s);
      const float stripW = dashW * 3.0F + dashGap * 2.0F;
      for (std::size_t d = 1; d < m_ui->galleryMarks.size(); ++d) {
        place(d, std::round((itemW - stripW) / 2.0F) + static_cast<float>(d - 1) * (dashW + dashGap),
              y0 + screenH + Tokens::spaceP2 * s, dashW, dashH, 0.0F);
      }
    }
    views->addChild(std::move(slot));
    Accessible& a = m_ui->viewButtons[i]->accessibleState();
    a.role = AccessibleRole::Tab;
  }
  row->addChild(std::move(views));
  refreshViewButtons();
  if (m_host.headerButtons) {
    m_host.headerButtons(*row, s);
  }
  const Button::ButtonStateColors circle{.bg = clearColorSpec(), .border = clearColorSpec(),
                                         .label = colorSpecFromRole(ColorRole::Label1)};
  Button::ButtonStateColors circleHover = circle;
  circleHover.bg = colorSpecFromRole(ColorRole::Fill1);
  Button::ButtonStateColors circlePressed = circle;
  circlePressed.bg = colorSpecFromRole(ColorRole::Fill2);
  row->addChild(ui::button({
      .out = &m_ui->sortButton,
      .glyph = std::string("sort-descending"),
      .glyphSize = Tokens::sizeSymbol * s,
      .variant = ButtonVariant::Ghost,
      .customPalette = Button::ButtonPalette{.normal = circle, .hover = circleHover, .pressed = circlePressed,
                                             .disabled = circle},
      .tooltip = std::string("Sort"),
      .radius = Tokens::sizeControlM * s / 2.0F,
      .width = Tokens::sizeControlM * s,
      .height = Tokens::sizeControlM * s,
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
  // The folder filter (mockup .srch, Dolphin filter): 240 x 32 r16 fill_2, padding 0 12, gap 8; 320 while focused.
  const float searchH = Tokens::sizeControlM * s;
  const float searchMax = (m_ui->searchFocused ? Tokens::componentFilesSearchWidthFocus : Tokens::componentFilesSearchWidth) * s;
  const float searchMin = m_ui->searchFocused ? searchMax : Tokens::componentFilesSearchWidthMin * s;
  auto search = ui::row({.out = &m_ui->search, .align = FlexAlign::Center, .gap = Tokens::spaceP8 * s,
                         .paddingH = Tokens::spaceP12 * s, .fill = colorSpecFromRole(ColorRole::Fill2),
                         .radius = searchH / 2.0F, .minWidth = searchMin, .maxWidth = searchMax, .height = searchH,
                         .flexGrow = 1.0F});
  search->addChild(ui::glyph({.glyph = std::string("search"), .glyphSize = Tokens::sizeSymbol * s,
                              .color = colorSpecFromRole(ColorRole::Label1)}));
  search->addChild(ui::input({
      .out = &m_ui->searchField,
      .value = m_filter,
      .placeholder = "Search " + title(),
      .fontSize = Style::fontSizeBody * s,
      .controlHeight = searchH,
      .horizontalPadding = 0.0F,
      .frameVisible = false,
      .flexGrow = 1.0F,
      .onChange = [this](const std::string& text) { setFilter(text); },
      .onSubmit = [this](const std::string&) { focusList(); },
      .onKeyEvent =
          [this](std::uint32_t sym, std::uint32_t) {
            if (sym == XKB_KEY_Escape) {
              if (m_filter.empty()) {
                focusList();
              } else {
                m_ui->searchField->setValue({});
                setFilter({});
              }
              return true;
            }
            if (sym == XKB_KEY_Down) {
              focusList();
              return true;
            }
            return false;
          },
  }));
  search->addChild(ui::button({
      .out = &m_ui->searchClear,
      .glyph = std::string("x"),
      .glyphSize = Tokens::sizeSymbolInline * s,
      .variant = ButtonVariant::Ghost,
      .tooltip = std::string("Clear"),
      .radius = Tokens::spaceP20 * s / 2.0F,
      .width = Tokens::spaceP20 * s,
      .height = Tokens::spaceP20 * s,
      .visible = !m_filter.empty(),
      .onClick =
          [this] {
            if (m_ui->searchField != nullptr) {
              m_ui->searchField->setValue({});
            }
            setFilter({});
          },
  }));
  // Letters belong to the field, not to the window's type-ahead.
  m_ui->searchField->inputArea()->setCapturesAllKeys(true);
  m_ui->searchField->setTextColorOverride(std::nullopt, colorForRole(ColorRole::Label2));
  m_ui->searchField->setOnFocusGain([this] { widenSearch(true); });
  m_ui->searchField->setOnFocusLoss([this] { widenSearch(false); });
  row->addChild(std::move(search));
  return row;
}

void FileBrowser::refreshViewButtons() {
  const float s = m_ui->scale;
  for (std::size_t i = 0; i < m_ui->viewButtons.size(); ++i) {
    Button* b = m_ui->viewButtons[i];
    if (b == nullptr) {
      continue;
    }
    const bool on = i == static_cast<std::size_t>(m_options.view);
    const Button::ButtonStateColors off{.bg = clearColorSpec(), .border = clearColorSpec(),
                                        .label = colorSpecFromRole(ColorRole::Label2)};
    const Button::ButtonStateColors lens{.bg = colorSpecFromRole(ColorRole::Lens),
                                         .border = colorSpecFromRole(ColorRole::LensEdge),
                                         .label = colorSpecFromRole(ColorRole::Label1)};
    Button::ButtonStateColors hover = off;
    hover.label = colorSpecFromRole(ColorRole::Label1);
    const auto& base = on ? lens : off;
    b->setCustomPalette(Button::ButtonPalette{.borderWidth = on ? 0.5F * s : 0.0F, .normal = base,
                                              .hover = on ? lens : hover, .pressed = base, .disabled = base});
    b->accessibleState().selected = on;
    if (i == static_cast<std::size_t>(View::Gallery)) {
      const ColorSpec ink = colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2);
      for (std::size_t d = 0; d < m_ui->galleryMarks.size(); ++d) {
        if (Box* mark = m_ui->galleryMarks[d]; mark != nullptr) {
          if (d == 0) {
            mark->setFill(clearColorSpec());
            mark->setBorder(ink, Style::focusRingWidth * s * 0.75F);
          } else {
            mark->setFill(ink);
          }
        }
      }
    }
    if (Box* shadow = m_ui->viewShadows[i]; shadow != nullptr) {
      shadow->setFill(on ? colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? 0.12F : 0.30F) : clearColorSpec());
    }
  }
}

void FileBrowser::widenSearch(bool focused) {
  m_ui->searchFocused = focused;
  showChips();
  const float s = m_ui->scale;
  const float from = m_ui->search != nullptr ? m_ui->search->width() : 0.0F;
  const float to = (focused ? Tokens::componentFilesSearchWidthFocus : Tokens::componentFilesSearchWidth) * s;
  if (m_ui->search == nullptr || from <= 0.0F) {
    return;
  }
  MotionTraceLabel trace("files.search");
  m_searchAnim = m_animations.drive(
      m_searchAnim, from, to, Style::spring(Tokens::springFollow),
      [this, focused](float v) {
        if (m_ui->search != nullptr) {
          // Growing holds the width; shrinking only lowers the cap, so a narrow header can take it below 240.
          if (focused) {
            m_ui->search->setMinWidth(v);
          } else {
            m_ui->search->setMinWidth(std::min(m_ui->search->minWidth(), Tokens::componentFilesSearchWidthMin * m_ui->scale));
          }
          m_ui->search->setMaxWidth(v);
          m_ui->search->markLayoutDirty();
        }
        if (m_host.invalidate) {
          m_host.invalidate();
        }
      },
      [this] { m_searchAnim = 0; }, m_ui->search, 0.0F, Unit::Px, true);
}

bool FileBrowser::searchingEverywhere() const {
  return m_searchScope == SearchScope::Everywhere && m_host.search && !m_filter.empty();
}

void FileBrowser::setSearchScope(SearchScope scope) {
  if (scope == m_searchScope) {
    return;
  }
  m_searchScope = scope;
  kLog.debug("search scope {}", scope == SearchScope::Everywhere ? "pc" : "folder");
  refreshChips();
  querySearch();
  if (m_modelView != nullptr && !m_listing.has_value()) {
    m_modelView->setFilter(scope == SearchScope::Everywhere ? std::string() : m_filter);
  }
  refreshRows();
  refreshChrome();
  syncSearchView();
}

void FileBrowser::setSearchKind(SearchKind kind) {
  if (kind == m_searchKind) {
    return;
  }
  m_searchKind = kind;
  static constexpr std::array kNames{"all", "pdf", "image", "doc", "sheet", "recent"};
  kLog.debug("search kind {}", kNames[static_cast<std::size_t>(kind)]);
  refreshChips();
  querySearch();
  refreshRows();
}

void FileBrowser::setSearchResults(const std::string& text, std::vector<std::shared_ptr<const FileInfo>> rows) {
  if (text != m_filter) {
    return;
  }
  m_searchText = text;
  m_searchRows = std::move(rows);
  kLog.debug("search results {}", m_searchRows.size());
  if (searchingEverywhere()) {
    refreshRows();
    refreshChrome();
  }
}

void FileBrowser::syncSearchView() {
  // This PC results are a list with a Where column (mockup §5); the folder's own view comes back after.
  m_searchViewSwitch = true;
  if (searchingEverywhere() && !m_viewBeforeSearch.has_value()) {
    m_viewBeforeSearch = m_options.view;
    setView(View::List);
  } else if (!searchingEverywhere() && m_viewBeforeSearch.has_value()) {
    const View back = *m_viewBeforeSearch;
    m_viewBeforeSearch.reset();
    setView(back);
  }
  m_searchViewSwitch = false;
}

void FileBrowser::querySearch() {
  if (!searchingEverywhere()) {
    return;
  }
  static constexpr std::array<std::string_view, 6> kWords{"", "pdf", "image", "doc", "sheet", ""};
  m_host.search(m_filter, kWords[static_cast<std::size_t>(m_searchKind)]);
}

void FileBrowser::showChips() {
  Flex* chips = m_ui->chips;
  const bool show = m_ui->searchFocused || !m_filter.empty();
  if (chips == nullptr || chips->visible() == show) {
    return;
  }
  chips->setVisible(show);
  chips->setParticipatesInLayout(show);
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

// Rebuilds the chips for a new scope or kind; a focus change only shows or hides them (showChips), so a press on a
// chip that takes focus from the field keeps its button.
void FileBrowser::refreshChips() {
  Flex* chips = m_ui->chips;
  if (chips == nullptr) {
    return;
  }
  chips->setVisible(false);
  showChips();
  while (!chips->children().empty()) {
    (void)chips->removeChild(chips->children().back().get());
  }
  m_ui->chipNodes.clear();
  const float s = m_ui->scale;
  const auto chip = [&](std::string text, bool on, std::function<void()> click) {
    // Mockup .chip: 24 high r12, padding 0 10, 500 12; off fill_1 label_2, on the selection tint label_1.
    const ColorSpec bg = on ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F)
                            : colorSpecFromRole(ColorRole::Fill1);
    const Button::ButtonStateColors colors{.bg = bg, .border = clearColorSpec(),
                                           .label = colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2)};
    Button* button = nullptr;
    chips->addChild(ui::button({
        .out = &button,
        .text = std::move(text),
        .fontSize = Style::fontSizeCaption * s,
        .customPalette = Button::ButtonPalette{.normal = colors, .hover = colors, .pressed = colors, .disabled = colors},
        .paddingH = Tokens::spaceP8 * s + 2.0F * s,
        .radius = Tokens::spaceP12 * s,
        .height = Tokens::spaceP24 * s,
        .onClick = std::move(click),
    }));
    if (Label* label = button->label(); label != nullptr) {
      label->setFontWeight(FontWeight::Medium);
    }
    m_ui->chipNodes.emplace_back(button, button->label() != nullptr ? button->label()->text() : std::string());
  };
  if (m_host.search) {
    chip("This Folder", m_searchScope == SearchScope::Folder, [this] { setSearchScope(SearchScope::Folder); });
    chip("This PC", m_searchScope == SearchScope::Everywhere, [this] { setSearchScope(SearchScope::Everywhere); });
    chips->addChild(ui::node({.width = Tokens::spaceP8 * s}));
  }
  static constexpr std::array<std::pair<const char*, SearchKind>, 6> kKinds{
      {{"All", SearchKind::All}, {"PDFs", SearchKind::Pdf}, {"Images", SearchKind::Image}, {"Docs", SearchKind::Doc},
       {"Sheets", SearchKind::Sheet}, {"Last 30 days", SearchKind::Recent}}};
  for (const auto& [name, kind] : kKinds) {
    chip(name, m_searchKind == kind, [this, kind] { setSearchKind(kind); });
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileBrowser::setFilter(std::string text) {
  if (text == m_filter) {
    return;
  }
  m_filter = std::move(text);
  if (m_ui->searchClear != nullptr) {
    m_ui->searchClear->setVisible(!m_filter.empty());
  }
  showChips();
  querySearch();
  const auto start = std::chrono::steady_clock::now();
  if (m_modelView != nullptr && !m_listing.has_value()) {
    m_modelView->setFilter(searchingEverywhere() ? std::string() : m_filter);
  }
  if (m_modelView == nullptr || m_listing.has_value() || m_host.search || m_searchKind != SearchKind::All) {
    refreshRows();
    refreshChrome();
  }
  syncSearchView();
  kLog.debug("filter took {:.2f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

void FileBrowser::focusSearch() {
  if (m_ui->searchField != nullptr && m_host.focus) {
    m_host.focus(m_ui->searchField->inputArea());
    m_ui->searchField->selectAll();
  }
}

void FileBrowser::focusList() {
  if (m_host.focus) {
    m_host.focus(m_ui->focus);
  }
}

std::unique_ptr<Node> FileBrowser::buildSidebar(float width) {
  const float s = m_ui->scale;
  m_ui->sidebarWidth = width;
  auto list = ui::column({.out = &m_ui->sidebarList, .align = FlexAlign::Stretch,
                          .padding = Tokens::componentSidebarPadding * s, .width = width});
  refreshSidebar();
  return list;
}

void FileBrowser::releaseChrome() {
  m_ui->sidebarList = nullptr;
  m_ui->sidebarRows.clear();
  m_ui->viewButtons = {};
  m_ui->viewShadows = {};
  m_ui->galleryMarks = {};
  m_ui->sortButton = nullptr;
  m_ui->search = nullptr;
  m_ui->searchField = nullptr;
  m_ui->searchClear = nullptr;
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
  m_ui->sidebarRows.clear();
  // Mockup .ghd: 600 11/16 label_2, padding 16 8 4 (12 on top for the first); the last group has no caption.
  const auto caption = [&](const char* text) {
    const bool first = list->children().empty();
    auto head = ui::row({.align = FlexAlign::Center});
    const float top = first ? Tokens::spaceP12 : Tokens::componentSidebarGroupTop;
    head->setPadding(top * s, Tokens::spaceP8 * s, Tokens::componentSidebarGroupBottom * s, Tokens::spaceP8 * s);
    if (*text != '\0') {
      head->addChild(ui::label({
          .text = std::string(text),
          .fontSize = Tokens::typeCaption.size * s,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label2),
          .maxLines = 1,
      }));
    } else {
      head->setPadding(top * s, 0.0F, 0.0F, 0.0F);
    }
    list->addChild(std::move(head));
  };
  const auto row = [&](const Place& place) {
    const bool current = place.id.empty() ? (!m_listing.has_value() && !place.path.empty() && place.path == m_location)
                                          : place.id == m_listingId;
    // Mockup .srow: 32 high, r 8, place art 20, label 13/500, trailing meta label_3; selected = accent tint.
    auto flex = std::make_unique<SidebarRow>();
    flex->setDirection(FlexDirection::Horizontal);
    flex->setAlign(FlexAlign::Center);
    flex->setGap(Tokens::spaceP8 * s);
    flex->setPadding(0.0F, Tokens::spaceP8 * s);
    flex->setFill(current ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F) : clearColorSpec());
    flex->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
    flex->setMinHeight(kSidebarRow * s);
    flex->setMaxHeight(kSidebarRow * s);
    auto areaOwned = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, .participatesInLayout = false});
    InputArea* area = areaOwned.get();
    flex->area = area;
    if (place.dot.has_value()) {
      // Tags: a 12 px dot centred in the 20 px icon slot.
      auto slot = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .width = Tokens::spaceP20 * s,
                           .height = Tokens::spaceP20 * s});
      const float dot = Tokens::componentFilesTagDot * s;
      slot->addChild(ui::box({.fill = fixedColorSpec(*place.dot), .radius = dot / 2.0F, .width = dot, .height = dot}));
      flex->addChild(std::move(slot));
    } else if (!place.art.empty()) {
      flex->addChild(std::make_unique<PlaceIcon>(place.art, Tokens::spaceP20 * s));
    } else {
      flex->addChild(ui::glyph({.glyph = place.glyph, .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(current ? ColorRole::Label1 : ColorRole::AccentText)}));
    }
    flex->addChild(ui::label({.text = place.label, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::Medium,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                              .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
    if (!place.caption.empty()) {
      flex->addChild(ui::label({.text = place.caption, .fontSize = Style::fontSizeCaption * s,
                                .color = colorSpecFromRole(ColorRole::Label3)}));
    }
    if (place.usage.has_value()) {
      // Mockup .use: 40 x 4 r2 fill_2 track, accent level.
      const float useW = Tokens::componentFilesUsageWidth * s;
      const float useH = Tokens::componentFilesUsageHeight * s;
      auto track = ui::row({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Fill2), .radius = useH / 2.0F,
                            .clipChildren = true, .width = useW, .height = useH});
      track->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Accent), .radius = useH / 2.0F,
                               .width = std::round(useW * std::clamp(*place.usage, 0.0F, 1.0F)), .height = useH}));
      flex->addChild(std::move(track));
    }
    Button* eject = nullptr;
    if (place.eject && place.onEject) {
      flex->addChild(ui::button({
          .out = &eject,
          .glyph = std::string("player-eject"),
          .glyphSize = Tokens::sizeSymbolInline * s,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::string("Eject"),
          .radius = Tokens::spaceP20 * s / 2.0F,
          .width = Tokens::spaceP20 * s,
          .height = Tokens::spaceP20 * s,
          .opacity = 0.0F,
          .onClick = [run = place.onEject] { DeferredCall::callLater(run); },
      }));
      eject->setZIndex(1); // above the row's input area
    }
    Flex* rowBody = flex.get();
    if (eject != nullptr) {
      eject->setOnEnter([this, rowBody, eject, current] {
        eject->setOpacity(1.0F);
        if (!current) {
          rowBody->setFill(colorSpecFromRole(ColorRole::Fill1));
        }
        if (m_host.invalidate) {
          m_host.invalidate();
        }
      });
      eject->setOnLeave([this, rowBody, eject, current] {
        eject->setOpacity(0.0F);
        if (!current) {
          rowBody->setFill(clearColorSpec());
        }
        if (m_host.invalidate) {
          m_host.invalidate();
        }
      });
    }
    area->setOnEnter([this, rowBody, current, eject](const InputArea::PointerData&) {
      if (!current) {
        rowBody->setFill(colorSpecFromRole(ColorRole::Fill1));
      }
      if (eject != nullptr) {
        eject->setOpacity(1.0F);
      }
      if (m_host.invalidate) {
        m_host.invalidate();
      }
    });
    area->setOnLeave([this, rowBody, current, eject] {
      if (!current) {
        rowBody->setFill(clearColorSpec());
      }
      if (eject != nullptr) {
        eject->setOpacity(0.0F);
      }
      if (m_host.invalidate) {
        m_host.invalidate();
      }
    });
    area->setOnPress([rowBody, current](const InputArea::PointerData&) {
      if (!current) {
        rowBody->setFill(colorSpecFromRole(ColorRole::Fill2));
      }
    });
    // The navigation rebuilds the sidebar, which destroys this row: run it after the click returns.
    area->setOnClick([this, place](const InputArea::PointerData&) {
      DeferredCall::callLater([this, place, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired()) {
          return;
        }
        if (place.onClick) {
          place.onClick();
        } else {
          jump(place.path);
        }
      });
    });
    Accessible& a = area->accessibleState();
    a.role = AccessibleRole::ListItem;
    a.name = place.label;
    a.selected = current;
    flex->addChild(std::move(areaOwned));
    m_ui->sidebarRows.emplace_back(flex.get(), place);
    list->addChild(std::move(flex));
  };

  const auto rowsOf = [&](Place::Group group) {
    for (const Place& place : m_options.extraPlaces) {
      if (place.group == group) {
        row(place);
      }
    }
  };
  caption("Favourites");
  rowsOf(Place::Group::Favourites);
  const fs::path home = g_get_home_dir();
  row({.label = homeName(), .glyph = "home", .path = home, .art = "home"});
  const std::pair<GUserDirectory, std::pair<const char*, const char*>> dirs[] = {
      {G_USER_DIRECTORY_DESKTOP, {"Desktop", "desktop"}},
      {G_USER_DIRECTORY_DOCUMENTS, {"Documents", "documents"}},
      {G_USER_DIRECTORY_DOWNLOAD, {"Downloads", "downloads"}},
      {G_USER_DIRECTORY_PICTURES, {"Pictures", "pictures"}},
      {G_USER_DIRECTORY_MUSIC, {"Music", "music"}},
      {G_USER_DIRECTORY_VIDEOS, {"Videos", "videos"}},
  };
  for (const auto& [dir, label] : dirs) {
    const char* path = g_get_user_special_dir(dir);
    std::error_code ec;
    // An unset XDG user dir falls back to $HOME, which is already listed.
    if (path != nullptr && fs::path(path) != home && fs::is_directory(path, ec)) {
      row({.label = label.first, .glyph = "folder", .path = path, .art = label.second});
    }
  }
  if (std::ranges::any_of(m_options.extraPlaces, [](const Place& p) { return p.group == Place::Group::Locations; })) {
    caption("Locations");
    rowsOf(Place::Group::Locations);
  }
  if (std::ranges::any_of(m_options.extraPlaces, [](const Place& p) { return p.group == Place::Group::Tags; })) {
    caption("Tags");
    rowsOf(Place::Group::Tags);
  }
  if (std::ranges::any_of(m_options.extraPlaces, [](const Place& p) { return p.group == Place::Group::Last; })) {
    caption(""); // a gap: the last group has no title
    rowsOf(Place::Group::Last);
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

std::unique_ptr<Node> FileBrowser::buildContent(bool pathBar, bool crumbs) {
  const float s = m_ui->scale;
  auto content = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  m_ui->crumbRow = nullptr;
  if (crumbs) {
    content->addChild(ui::row({.out = &m_ui->crumbRow, .align = FlexAlign::Center, .gap = Tokens::spaceP4 * s,
                               .paddingH = Tokens::spaceP16 * s, .clipChildren = true, .height = kPathBar * s}));
  }
  // Mockup .tabs: 40 high, padding 4 8, gap 4; shown with 2 or more tabs.
  content->addChild(ui::row({.out = &m_ui->tabStrip, .align = FlexAlign::Center, .gap = Tokens::spaceP4 * s,
                             .padding = Tokens::spaceP4 * s, .minHeight = Tokens::spaceP40 * s,
                             .maxHeight = Tokens::spaceP40 * s, .visible = false}));
  m_ui->tabStrip->setPadding(Tokens::spaceP4 * s, Tokens::spaceP8 * s);
  // Mockup .chips: padding 0 16 8, gap 8; shown while the search field has focus or text.
  content->addChild(ui::row({.out = &m_ui->chips, .align = FlexAlign::Center, .gap = Tokens::spaceP8 * s,
                             .paddingH = Tokens::spaceP16 * s, .visible = false}));
  m_ui->chips->setPadding(0.0F, Tokens::spaceP16 * s, Tokens::spaceP8 * s, Tokens::spaceP16 * s);
  refreshChips();

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
    page.custom = nullptr;
    page.customId.clear();
    page.banner = nullptr;
    page.bannerId.clear();

    root->addChild(ui::virtualGridView({
        .out = &page.icons,
        .state = &page.iconsScroll,
        .contentScale = s,
        .columns = 0,
        .minCellWidth = FileIconCell::cellWidth(m_iconSize, s),
        .cellHeight = FileIconCell::cellHeight(m_iconSize, s),
        .squareCells = false,
        .columnGap = kIconGap * s,
        .rowGap = kIconGap * s,
        .overscanRows = 1,
        .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        .scrollbarVisible = true,
        .adapter = page.iconsAdapter.get(),
        .configure = [](VirtualGridView& grid) { grid.setFixedCellWidth(true); },
    }));

    auto listBox = ui::column({.out = &page.listBox, .align = FlexAlign::Stretch});
    // Mockup .thead: 32 high, text 16 from the pane edge like the rows, 500 12 label_2, columns as FileListRow.
    auto header = ui::row({.align = FlexAlign::Center, .paddingH = FileListRow::textInset(s), .height = kListHeader * s});
    const std::array<std::pair<const char*, FileSortKey>, 4> cols{
        {{"Name", FileSortKey::Name}, {"Date Modified", FileSortKey::Modified}, {"Size", FileSortKey::Size},
         {"Kind", FileSortKey::Kind}}};
    for (std::size_t c = 0; c < cols.size(); ++c) {
      const Button::ButtonStateColors colors{.bg = clearColorSpec(), .border = clearColorSpec(),
                                             .label = colorSpecFromRole(ColorRole::Label2)};
      header->addChild(ui::button({
          .out = &page.sortButtons[c],
          .text = std::string(cols[c].first),
          .fontSize = Style::fontSizeCaption * s,
          .contentAlign = c == 2 ? ButtonContentAlign::End : ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .customPalette = Button::ButtonPalette{.normal = colors, .hover = colors, .pressed = colors, .disabled = colors},
          .paddingH = 0.0F,
          .paddingLeft = c == 3 ? Tokens::componentFilesColumnKindPad * s : 0.0F,
          .height = kListHeader * s,
          .flexGrow = c == 0 ? 1.0F : 0.0F,
          .onClick = [this, key = cols[c].second] { setSort(key); },
      }));
      if (Label* text = page.sortButtons[c]->label(); text != nullptr) {
        text->setFontWeight(FontWeight::Medium);
      }
      if (c == 0) {
        header->addChild(ui::button({
            .out = &page.whereHeader,
            .text = std::string("Where"),
            .fontSize = Style::fontSizeCaption * s,
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .customPalette = Button::ButtonPalette{.normal = colors, .hover = colors, .pressed = colors, .disabled = colors},
            .paddingH = 0.0F,
            .height = kListHeader * s,
            .visible = false,
            .participatesInLayout = false,
        }));
        if (Label* text = page.whereHeader->label(); text != nullptr) {
          text->setFontWeight(FontWeight::Medium);
        }
      }
    }
    listBox->addChild(std::move(header));
    listBox->addChild(ui::separator({.gradientEdges = false}));
    // Mockup .trows: 4 above the first row.
    listBox->addChild(ui::row({.minHeight = Tokens::spaceP4 * s, .maxHeight = Tokens::spaceP4 * s}));
    listBox->addChild(ui::virtualGridView({
        .out = &page.list,
        .state = &page.listScroll,
        .contentScale = s,
        .columns = 1,
        .cellHeight = std::ceil(Tokens::sizeRowCompact * s),
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
    for (VirtualGridView* grid : {page.icons, page.list}) {
      Accessible& a = grid->accessibleState();
      a.role = AccessibleRole::List;
      a.name = "Files";
    }

    auto skeleton = ui::node({.visible = false});
    page.skeleton = skeleton.get();
    root->addChild(std::move(skeleton));

    auto empty = ui::column({.out = &page.empty, .align = FlexAlign::Center, .justify = FlexJustify::Center,
                             .gap = Style::spaceSm * s, .visible = false});
    empty->addChild(ui::glyph({.out = &page.emptyGlyph, .glyph = std::string("folder"), .glyphSize = Tokens::spaceP48 * s,
                               .color = colorSpecFromRole(ColorRole::Label3)}));
    empty->addChild(ui::label({.out = &page.emptyTitle, .fontSize = Tokens::typeTitle3.size * s,
                               .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
    empty->addChild(ui::label({.out = &page.emptyBody, .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 3,
                               .textAlign = TextAlign::Center}));
    empty->addChild(ui::button({.out = &page.emptyAction, .text = std::string("Search This PC"),
                                .variant = ButtonVariant::Default, .visible = false,
                                .onClick = [this] { setSearchScope(SearchScope::Everywhere); }}));
    root->addChild(std::move(empty));
    stage->addChild(std::move(root));
  }

  auto renameBox = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * s, .visible = false});
  renameBox->setZIndex(50);
  m_ui->renameBox = renameBox.get();
  renameBox->addChild(ui::input({
      .out = &m_ui->renameField,
      .fontSize = Style::fontSizeCaption * s,
      .controlHeight = Tokens::sizeControlS * s,
      .horizontalPadding = Tokens::spaceP4 * s,
      .onChange =
          [this](const std::string& value) {
            kLog.debug("rename field {}", value);
            if (m_rename.has_value()) {
              m_rename->text = value;
              m_rename->edited = true;
            }
            if (m_rename.has_value() && !m_rename->error.empty()) {
              m_rename->error.clear();
              m_ui->renameField->setInvalid(false);
              m_ui->renameError->setVisible(false);
            }
          },
      .onSubmit = [this](const std::string&) { commitRename(false); },
      .onKeyEvent =
          [this](std::uint32_t sym, std::uint32_t mods) {
            if (sym == XKB_KEY_Escape) {
              cancelRename();
              return true;
            }
            if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
              commitRename(true);
              return true;
            }
            if (sym == XKB_KEY_F2 && m_rename.has_value() && m_ui->renameField != nullptr) {
              m_ui->renameField->selectAll(); // F2 twice: the whole name
              return true;
            }
            (void)mods;
            return false;
          },
  }));
  // The field owns plain keys while focused: the window's list keys and type-ahead must not take them.
  m_ui->renameField->inputArea()->setCapturesAllKeys(true);
  renameBox->addChild(ui::label({.out = &m_ui->renameError, .fontSize = Style::fontSizeCaption * s,
                                 .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 2, .visible = false}));
  stage->addChild(std::move(renameBox));
  auto columnsNode = m_columns->build(s);
  columnsNode->setVisible(m_options.view == View::Columns);
  m_ui->columnsNode = columnsNode.get();
  stage->addChild(std::move(columnsNode));
  auto gallery = std::make_unique<FileGallery>(s, m_options.thumbnails,
                                               FileGallery::Host{.invalidate = [this] {
                                                                   if (m_host.invalidate) {
                                                                     m_host.invalidate();
                                                                   }
                                                                 },
                                                                 .select = [this](std::size_t i) {
                                                                   galleryStep(static_cast<int>(i) -
                                                                               static_cast<int>(m_galleryCurrent));
                                                                 }});
  gallery->setVisible(m_options.view == View::Gallery);
  gallery->setZIndex(2);
  m_ui->gallery = gallery.get();
  m_galleryItems.clear(); // the new node starts empty: the next sync fills it
  stage->addChild(std::move(gallery));

  stage->onLayout = [this](Renderer& renderer, float w, float h) {
    m_ui->stageWidth = w;
    if (m_options.thumbnails != nullptr && m_options.thumbnails->uploadPending(renderer.textureManager())) {
      rebindPages();
    }
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
      if (page.custom != nullptr) {
        page.icons->setVisible(false);
        page.listBox->setVisible(false);
        page.custom->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = w, .height = h});
        continue;
      }
      if (m_options.view == View::Columns || m_options.view == View::Gallery) {
        page.root->setVisible(false);
      }
      page.icons->setVisible(icons || m_ui->viewAnim != 0);
      page.listBox->setVisible(m_options.view == View::List || m_ui->viewAnim != 0);
      page.icons->setOpacity(icons ? m_ui->viewMix : 1.0F - m_ui->viewMix);
      page.listBox->setOpacity(icons ? 1.0F - m_ui->viewMix : m_ui->viewMix);
      float top = 0.0F; // under the banner
      if (page.banner != nullptr) {
        top = page.banner->measure(renderer, LayoutConstraints::available(w, h)).height;
        page.banner->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = w, .height = top});
      }
      page.icons->arrange(renderer, LayoutRect{.x = pad, .y = top + pad, .width = w - pad - pad, .height = h - top - pad});
      page.listBox->arrange(renderer, LayoutRect{.x = 0.0F, .y = top, .width = w, .height = h - top});
      page.empty->arrange(renderer, LayoutRect{.x = 0.0F, .y = top, .width = w, .height = h - top});
      page.skeleton->setSize(w, h);
      page.skeleton->layout(renderer);
    }
    const bool galleryShown = m_options.view == View::Gallery && m_ui->cur().custom == nullptr;
    m_ui->gallery->setVisible(galleryShown);
    if (galleryShown) {
      m_ui->gallery->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = w, .height = h});
    }
    const bool columns = m_options.view == View::Columns && m_ui->cur().custom == nullptr;
    m_ui->columnsNode->setVisible(columns);
    if (columns) {
      m_columns->setRenderer(&renderer);
      m_ui->columnsNode->setZIndex(2);
      m_columns->setViewportHeight(h);
      m_ui->columnsNode->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = w, .height = h});
      m_columns->afterLayout();
    }
    if (m_rename.has_value()) {
      placeRenameField();
      m_ui->renameBox->layout(renderer);
    }
    logGeometry();
  };
  content->addChild(std::move(stage));
  refreshTabs();

  auto rule = ui::separator({.gradientEdges = false});
  rule->setVisible(pathBar);
  content->addChild(std::move(rule));
  // Mockup .pbar: padding 0 16, gap 4, 500 12 label_2, last segment label_1; status, then the icon-size slider.
  auto path = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP16 * s,
                       .height = kPathBar * s});
  path->addChild(ui::row({.out = &m_ui->pathRow, .align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .clipChildren = true,
                          .flexGrow = 1.0F}));
  path->addChild(ui::label({.out = &m_ui->status, .fontSize = Style::fontSizeCaption * s,
                            .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
  path->addChild(ui::slider({
      .out = &m_ui->zoom,
      .minValue = Tokens::componentFilesIconMin,
      .maxValue = Tokens::componentFilesIconMax,
      .step = Tokens::spaceP16,
      .value = iconSize(),
      .trackHeight = Tokens::componentFilesZoomTrack * s,
      .thumbSize = Tokens::componentFilesZoomKnob * s,
      .controlHeight = Tokens::sizeControlS * s,
      .width = Tokens::componentFilesZoomWidth * s,
      .visible = m_options.view == View::Icons,
      .onValueChanged = [this](double v) { setIconSize(static_cast<float>(v)); },
  }));
  m_ui->zoom->accessibleState().name = "Icon size";
  path->setVisible(pathBar);
  content->addChild(std::move(path));

  // The old nodes owned the running springs, and their completions were dropped with them: settle at the targets.
  ++m_presentGen;
  for (std::size_t i = 0; i < 2; ++i) {
    Page& page = m_ui->pages[i];
    const bool current = i == m_ui->current;
    page.xAnim = page.fadeAnim = 0;
    page.x = 0.0F;
    page.opacity = 1.0F;
    page.root->setVisible(current);
    page.root->setZIndex(current ? 1 : 0);
    if (!current) {
      page.rows.clear();
    }
    page.icons->notifyDataChanged();
    page.list->notifyDataChanged();
  }
  m_ui->viewAnim = 0;
  m_ui->viewMix = 1.0F;
  applyViewMix();
  if (m_location.empty() && !m_listing.has_value()) {
    navigate({.path = g_get_home_dir()}, Motion::None);
  } else {
    if (m_skeletonShown) {
      showSkeleton(true);
    } else {
      refreshRows();
    }
    refreshChrome();
  }
  restoreRename();
  return content;
}

void FileBrowser::restoreRename() {
  if (!m_rename.has_value() || m_rename->busy) {
    return;
  }
  // The host rebuilt the content (a toast, a resize) while the field was open: the new field takes over.
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired() || !m_rename.has_value() || m_ui->renameField == nullptr) {
      return;
    }
    const auto index = indexOf(m_rename->name);
    if (!index.has_value()) {
      return;
    }
    if (!m_rename->edited) {
      startRename(*index, false);
      return;
    }
    const Rename keep = *m_rename;
    startRename(*index, false);
    m_rename->text = keep.text;
    m_rename->edited = true;
    m_ui->renameField->setValue(keep.text);
  });
}

// ---- navigation ---------------------------------------------------------------------------------------------------

FileBrowser::HistoryEntry FileBrowser::current() const { return {.path = m_location, .listing = m_listing}; }

void FileBrowser::newTab() {
  m_tabs[m_tab] = Tab{.entry = current(), .back = m_back, .forward = m_forward,
                      .selected = std::vector<std::string>(m_selected.begin(), m_selected.end())};
  m_tabs.insert(m_tabs.begin() + static_cast<std::ptrdiff_t>(m_tab) + 1, Tab{.entry = current()});
  ++m_tab;
  kLog.debug("tab new {} of {}", m_tab + 1, m_tabs.size());
  m_back.clear();
  m_forward.clear();
  navigate(current(), Motion::Fade);
  refreshTabs();
}

bool FileBrowser::closeTab() {
  if (m_tabs.size() < 2) {
    return false;
  }
  m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(m_tab));
  m_tab = std::min(m_tab, m_tabs.size() - 1);
  kLog.debug("tab close, now {} of {}", m_tab + 1, m_tabs.size());
  Tab next = std::move(m_tabs[m_tab]);
  m_back = std::move(next.back);
  m_forward = std::move(next.forward);
  navigate(std::move(next.entry), Motion::Fade, std::move(next.selected));
  refreshTabs();
  return true;
}

void FileBrowser::selectTab(std::size_t index) {
  if (index >= m_tabs.size() || index == m_tab) {
    return;
  }
  m_tabs[m_tab] = Tab{.entry = current(), .back = m_back, .forward = m_forward,
                      .selected = std::vector<std::string>(m_selected.begin(), m_selected.end())};
  m_tab = index;
  kLog.debug("tab select {} of {}", m_tab + 1, m_tabs.size());
  Tab next = std::move(m_tabs[m_tab]);
  m_back = std::move(next.back);
  m_forward = std::move(next.forward);
  navigate(std::move(next.entry), Motion::Fade, std::move(next.selected));
  refreshTabs();
}

void FileBrowser::stepTab(int delta) {
  if (m_tabs.size() < 2) {
    return;
  }
  const auto n = static_cast<int>(m_tabs.size());
  selectTab(static_cast<std::size_t>(((static_cast<int>(m_tab) + delta) % n + n) % n));
}

void FileBrowser::refreshTabs() {
  Flex* strip = m_ui->tabStrip;
  if (strip == nullptr) {
    return;
  }
  const float s = m_ui->scale;
  while (!strip->children().empty()) {
    (void)strip->removeChild(strip->children().back().get());
  }
  const bool shown = m_tabs.size() >= 2;
  strip->setVisible(shown);
  strip->setParticipatesInLayout(shown);
  if (!shown) {
    if (m_host.invalidate) {
      m_host.invalidate();
    }
    return;
  }
  const auto titleOf = [this](std::size_t i) {
    const HistoryEntry& e = i == m_tab ? current() : m_tabs[i].entry;
    if (e.listing.has_value()) {
      return e.listing->title;
    }
    return e.path == fs::path(g_get_home_dir()) ? homeName() : e.path.filename().string();
  };
  // Mockup .tab: 32 high r8, padding 0 8, gap 8, 500 12; off = fill_1 label_2, on = lens + 0.5 stroke label_1.
  for (std::size_t i = 0; i < m_tabs.size(); ++i) {
    const bool on = i == m_tab;
    auto tab = std::make_unique<SidebarRow>();
    tab->setDirection(FlexDirection::Horizontal);
    tab->setAlign(FlexAlign::Center);
    tab->setGap(Tokens::spaceP8 * s);
    tab->setPadding(0.0F, Tokens::spaceP8 * s);
    tab->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
    tab->setFill(colorSpecFromRole(on ? ColorRole::Lens : ColorRole::Fill1));
    if (on) {
      tab->setBorder(colorSpecFromRole(ColorRole::LensEdge), 0.5F * s);
    }
    tab->setMinHeight(Tokens::sizeControlM * s);
    tab->setMaxHeight(Tokens::sizeControlM * s);
    tab->setFlexGrow(1.0F);
    tab->setClipChildren(true);
    const ColorSpec ink = colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2);
    tab->addChild(ui::glyph({.glyph = std::string("folder"), .glyphSize = Tokens::sizeSymbolInline * s, .color = ink}));
    tab->addChild(ui::label({.text = titleOf(i), .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                             .color = ink, .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    auto areaOwned = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, .participatesInLayout = false});
    tab->area = areaOwned.get();
    areaOwned->setOnClick([this, i](const InputArea::PointerData&) {
      DeferredCall::callLater([this, i, alive = std::weak_ptr<bool>(m_alive)] {
        if (!alive.expired()) {
          selectTab(i);
        }
      });
    });
    Accessible& a = areaOwned->accessibleState();
    a.role = AccessibleRole::Tab;
    a.name = titleOf(i);
    a.selected = on;
    tab->addChild(std::move(areaOwned));
    Button* close = nullptr;
    tab->addChild(ui::button({
        .out = &close,
        .glyph = std::string("x"),
        .glyphSize = Tokens::sizeSymbolInline * s,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::string("Close Tab"),
        .radius = Tokens::spaceP20 * s / 2.0F,
        .width = Tokens::spaceP20 * s,
        .height = Tokens::spaceP20 * s,
        .onClick =
            [this, i] {
              DeferredCall::callLater([this, i, alive = std::weak_ptr<bool>(m_alive)] {
                if (!alive.expired()) {
                  selectTab(i);
                  (void)closeTab();
                }
              });
            },
    }));
    close->setZIndex(1); // above the tab's input area
    strip->addChild(std::move(tab));
  }
  strip->addChild(ui::button({
      .glyph = std::string("plus"),
      .glyphSize = Tokens::sizeSymbolInline * s,
      .variant = ButtonVariant::Ghost,
      .tooltip = std::string("New Tab (Ctrl+T)"),
      .radius = Style::scaledRadius(Tokens::radiusR8, s),
      .width = Tokens::componentFilesTabAdd * s,
      .height = Tokens::sizeControlM * s,
      .onClick = [this] { DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
                   if (!alive.expired()) {
                     newTab();
                   }
                 }); },
  }));
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

bool FileBrowser::pushHistory() {
  // The first listing of a browser has nothing to go back to and nothing to animate from.
  if (m_location.empty() && !m_listing.has_value()) {
    return false;
  }
  m_back.push_back(current());
  m_forward.clear();
  return true;
}

void FileBrowser::go(const fs::path& dir) { goSelecting(dir, {}); }

void FileBrowser::goSelecting(const fs::path& dir, std::vector<std::string> select) {
  if (!m_listing.has_value() && dir == m_location) {
    return;
  }
  const bool deeper = !m_location.empty() && dir.parent_path() == m_location;
  const bool shallower = !m_location.empty() && m_location.string().starts_with(dir.string() + "/");
  const Motion motion = deeper ? Motion::Deeper : shallower ? Motion::Back : Motion::Fade;
  navigate({.path = dir}, pushHistory() ? motion : Motion::None, std::move(select));
}

void FileBrowser::jump(const fs::path& dir) {
  if (!m_listing.has_value() && dir == m_location) {
    return;
  }
  navigate({.path = dir}, pushHistory() ? Motion::Fade : Motion::None);
}

void FileBrowser::showListing(Listing listing) {
  if (m_listingId == listing.id && m_listing.has_value()) {
    m_listing = std::move(listing);
    refreshRows();
    return;
  }
  const bool moved = pushHistory();
  navigate({.path = m_location, .listing = std::move(listing)}, moved ? Motion::Fade : Motion::None);
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
  // The folder we came from gets the cursor.
  goSelecting(m_location.parent_path(), {m_location.filename().string()});
}

void FileBrowser::navigate(HistoryEntry target, Motion motion, std::vector<std::string> select) {
  if (m_viewBeforeSearch.has_value()) { // leaving This PC results: the folder view comes back first
    m_searchViewSwitch = true;
    const View back = *m_viewBeforeSearch;
    m_viewBeforeSearch.reset();
    setView(back);
    m_searchViewSwitch = false;
  }
  m_location = std::move(target.path);
  m_listing = std::move(target.listing);
  m_searchRows.clear();
  m_searchText.clear();
  if (!m_filter.empty()) { // a filter belongs to the folder it was typed in
    m_filter.clear();
    if (m_ui->searchField != nullptr) {
      m_ui->searchField->setValue({});
    }
  }
  if (m_ui->searchField != nullptr) {
    m_ui->searchField->setPlaceholder("Search " + (m_listing.has_value() ? m_listing->title : m_location.filename().string()));
  }
  m_listingId = m_listing.has_value() ? m_listing->id : std::string();
  m_navigateAt = m_listing.has_value() ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now();
  m_listedLogged = false;
  m_expanded.clear();
  m_selected.clear();
  m_anchor.reset();
  m_cursor.reset();
  m_pendingSelect = std::move(select);
  m_typeAhead.reset();
  m_pendingMotion = motion;
  m_skeletonTimer.stop();
  kLog.debug("location {}", m_listing.has_value() ? m_listingId : m_location.string());
  attachModel();
  refreshSidebar();
  if (m_tabs.size() > 1) {
    refreshTabs();
  }
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
    // The skeleton takes the old listing's place; the new listing then crossfades in over it (M17).
    m_pendingMotion = Motion::Fade;
    showSkeleton(true);
  });
}

void FileBrowser::onModelChanged() {
  if (m_model == nullptr || m_model->status() == FileModel::Status::Loading) {
    return;
  }
  // tests/e2e/files_speed.sh reads these: names listed, then every stat read (FB-SPEED budgets).
  if (m_navigateAt != std::chrono::steady_clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_navigateAt).count();
    if (!m_listedLogged) {
      m_listedLogged = true;
      kLog.debug("listed {} in {:.1f} ms", m_model->entries().size(), ms);
    }
    if (!m_model->statPending()) {
      kLog.debug("statted {} in {:.1f} ms", m_model->entries().size(), ms);
      m_navigateAt = {};
    }
  }
  if (m_waiting) {
    present(m_pendingMotion);
    return;
  }
  if (m_skeletonShown && m_skeletonHeld) {
    return; // the hold timer presents
  }
  // Size and date sorts hold stat-only updates, so rows do not jump while sizes arrive (FB-SPEED). Structural
  // changes (a new count, a state change) always show, and a stat that never ends (a hung mount) holds 2 s at most.
  const bool statSort = m_sort.key == FileSortKey::Size || m_sort.key == FileSortKey::Modified;
  const bool structural = m_modelView == nullptr || m_modelView->rows().size() != m_rows.size()
      || m_model->status() != FileModel::Status::Ready;
  if (m_model->statPending() && statSort && !structural) {
    const auto now = std::chrono::steady_clock::now();
    if (m_statHoldSince == std::chrono::steady_clock::time_point{}) {
      m_statHoldSince = now;
    }
    if (now - m_statHoldSince < std::chrono::seconds(2)) {
      return;
    }
  }
  m_statHoldSince = {};
  refreshRows();
}

void FileBrowser::wantStat(const std::shared_ptr<const FileInfo>& row) {
  m_statWanted.push_back(row);
  if (m_statPosted) {
    return;
  }
  m_statPosted = true;
  // The cells bound in one frame are the viewport: ask for their stat first, in one call.
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    m_statPosted = false;
    auto rows = std::exchange(m_statWanted, {});
    if (m_model != nullptr && !rows.empty()) {
      m_model->prioritizeStat(rows);
    }
  });
}

void FileBrowser::present(Motion motion) {
  const bool loaded = m_listing.has_value() || (m_model != nullptr && m_model->status() != FileModel::Status::Loading);
  if (loaded) {
    m_waiting = false;
    m_skeletonTimer.stop();
    if (m_skeletonShown && m_skeletonHeld) {
      return; // the hold timer presents
    }
    // A shown skeleton stays on the outgoing page until its fade ends (hideOut clears it), so no frame is blank.
    const bool skeleton = m_skeletonShown;
    m_skeletonShown = false;
    m_skeletonHoldTimer.stop();
    if (skeleton && motion == Motion::None) {
      clearSkeleton(m_ui->cur());
    }
  }
  m_pendingMotion = Motion::None;
  if (m_options.view == View::Columns || m_options.view == View::Gallery) {
    motion = Motion::None; // D7: the columns view scrolls, never slides; the Gallery swaps the photo
  }
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
  const std::uint64_t gen = ++m_presentGen;
  // fresh: the page shows a new listing, so it starts at `from`; otherwise it retargets from where it is.
  const auto animateX = [this](Page& page, float from, float to, std::string_view label, bool fresh,
                               std::function<void()> done) {
    if (fresh) {
      m_animations.cancel(page.xAnim);
      page.xAnim = 0;
      page.x = from;
      if (page.root != nullptr) {
        page.root->setPosition(from, 0.0F);
      }
    }
    MotionTraceLabel trace(label);
    page.xAnim = m_animations.drive(
        page.xAnim, from, to, Style::spring(Tokens::springPage),
        [this, &page](float v) {
          page.x = v;
          if (page.root != nullptr) {
            page.root->setPosition(v, 0.0F);
          }
        },
        std::move(done), page.root, 0.0F, Unit::Px, true
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
  // Only the latest navigation hides its outgoing page: an older one's callback would hit a page in use.
  const auto hideOut = [this, gen] {
    if (gen != m_presentGen) {
      return;
    }
    Page& gone = m_ui->other();
    m_animations.cancel(gone.xAnim);
    m_animations.cancel(gone.fadeAnim);
    gone.xAnim = gone.fadeAnim = 0;
    gone.x = 0.0F;
    gone.opacity = 1.0F;
    if (gone.root == nullptr) {
      return;
    }
    gone.root->setPosition(0.0F, 0.0F);
    gone.root->setOpacity(1.0F);
    gone.root->setVisible(false);
    clearSkeleton(gone);
    gone.rows.clear();
    gone.icons->notifyDataChanged();
    gone.list->notifyDataChanged();
  };

  if (in.root != nullptr) {
    in.root->setVisible(true);
    in.root->setHitTestVisible(true);
    in.root->setZIndex(1);
  }
  // The outgoing page stays drawn until its motion ends but takes no input from now: a back slide keeps it on top.
  if (out.root != nullptr && &out != &in) {
    out.root->setHitTestVisible(false);
  }
  if (motion == Motion::None || out.root == nullptr || in.root == nullptr) {
    m_animations.cancel(in.xAnim);
    m_animations.cancel(in.fadeAnim);
    in.xAnim = in.fadeAnim = 0;
    in.x = 0.0F;
    in.opacity = 1.0F;
    if (in.root != nullptr) {
      in.root->setPosition(0.0F, 0.0F);
      in.root->setOpacity(1.0F);
    }
    if (out.root != nullptr && &out != &in && !m_waiting) {
      hideOut();
    }
  } else if (motion == Motion::Fade) {
    m_animations.cancel(in.xAnim);
    in.xAnim = 0;
    in.x = 0.0F;
    in.root->setPosition(0.0F, 0.0F);
    out.root->setZIndex(0);
    animateOpacity(out, out.opacity, 0.0F, Tokens::springFadeOut, "files.page.out", {});
    animateOpacity(in, 0.0F, 1.0F, Tokens::springFade, "files.page.in", hideOut);
  } else {
    const bool deeper = motion == Motion::Deeper;
    // Deeper: the new page enters from the trailing edge over the old one, which drifts 30 % and dims 10 %.
    out.root->setZIndex(deeper ? 0 : 2);
    in.root->setZIndex(deeper ? 2 : 0);
    // The outgoing page leaves the scene once the incoming one is home.
    if (deeper) {
      m_animations.cancel(in.fadeAnim);
      in.fadeAnim = 0;
      in.opacity = 1.0F;
      in.root->setOpacity(1.0F);
      animateX(in, w + gap, 0.0F, "files.page", true, hideOut);
      animateX(out, out.x, -w * kParallax, "files.page.under", false, {});
      animateOpacity(out, out.opacity, kUnderDim, Tokens::springPage, "files.page.dim", {});
    } else {
      animateX(in, -w * kParallax, 0.0F, "files.page.under", true, hideOut);
      animateOpacity(in, kUnderDim, 1.0F, Tokens::springPage, "files.page.dim", {});
      animateX(out, out.x, w + gap, "files.page", false, {});
    }
  }
  refreshChrome();
  notifyChanged();
}

void FileBrowser::clearSkeleton(Page& page) {
  if (page.skeleton == nullptr) {
    return;
  }
  page.skeleton->setVisible(false);
  while (!page.skeleton->children().empty()) {
    (void)page.skeleton->removeChild(page.skeleton->children().back().get());
  }
  const bool showRows = page.status.empty();
  page.empty->setVisible(!showRows);
  page.icons->setVisible(showRows && (m_options.view == View::Icons || m_ui->viewAnim != 0));
  page.listBox->setVisible(showRows && (m_options.view == View::List || m_ui->viewAnim != 0));
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
      } else {
        m_pendingMotion = Motion::Fade;
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
    const float cw = FileIconCell::cellWidth(m_iconSize, s);
    const float ch = FileIconCell::cellHeight(m_iconSize, s);
    const float g = kIconGap * s;
    const auto cols = static_cast<std::size_t>(std::max(1.0F, std::floor((w - 2.0F * pad + g) / (cw + g))));
    for (std::size_t i = 0; i < cols * 3; ++i) {
      page.skeleton->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Fill1),
          .radius = Style::scaledRadius(Tokens::radiusR8, s),
          .width = cw,
          .height = ch,
          .configure =
              [=](Box& b) {
                b.setPosition(pad + static_cast<float>(i % cols) * (cw + g), pad + static_cast<float>(i / cols) * (ch + g));
              },
      }));
    }
  } else {
    const float rh = std::ceil(Tokens::sizeRowCompact * s);
    for (std::size_t i = 0; i < 8; ++i) {
      const float y = (kListHeader + 1.0F) * s + static_cast<float>(i) * rh + 10.0F * s;
      for (const auto& [x, bw] : {std::pair{Tokens::spaceP16, 0.40F}, std::pair{0.62F, 0.14F}, std::pair{0.82F, 0.10F}}) {
        const float left = x < 1.0F ? w * x : x * s;
        page.skeleton->addChild(ui::box({
            .fill = colorSpecFromRole(ColorRole::Fill1),
            .radius = Style::scaledRadius(Tokens::radiusR4, s),
            .width = w * bw,
            .height = Tokens::spaceP12 * s,
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
    if (!m_filter.empty()) {
      const std::string needle = TypeAhead::fold(m_filter);
      std::erase_if(rows, [&](const auto& r) { return TypeAhead::fold(r->displayName).find(needle) == std::string::npos; });
    }
    m_rows = std::move(rows);
  } else {
    if (!m_waiting) { // while a listing loads, the old rows stay on screen
      m_rows = m_modelView != nullptr ? m_modelView->rows() : std::vector<std::shared_ptr<const FileInfo>>{};
      if (expandable() && !m_expanded.empty()) {
        // Expanded folders list their items after them; a row name is its path relative to the location.
        std::vector<std::shared_ptr<const FileInfo>> rows;
        const std::function<void(const std::vector<std::shared_ptr<const FileInfo>>&, const std::string&)> add =
            [&](const std::vector<std::shared_ptr<const FileInfo>>& level, const std::string& prefix) {
              for (const auto& row : level) {
                std::shared_ptr<const FileInfo> shown = row;
                if (!prefix.empty()) {
                  auto copy = std::make_shared<FileInfo>(*row);
                  copy->name = prefix + row->name;
                  shown = copy;
                }
                rows.push_back(shown);
                if (const auto it = m_expanded.find(shown->name); shown->isDir && it != m_expanded.end()) {
                  add(it->second.view->rows(), shown->name + "/");
                }
              }
            };
        add(m_rows, {});
        m_rows = std::move(rows);
        kLog.debug("expanded rows {}", m_rows.size());
      }
    }
  }
  if (searchingEverywhere()) {
    m_rows = m_searchText == m_filter ? m_searchRows : std::vector<std::shared_ptr<const FileInfo>>{};
  }
  if (m_searchKind != SearchKind::All && (!m_filter.empty() || m_ui->searchFocused)) {
    std::erase_if(m_rows, [this](const auto& r) { return !matchesKind(*r, m_searchKind); });
  }
  std::erase_if(m_selected, [this](const std::string& name) { return !indexOf(name).has_value(); });
  if (!m_filter.empty()) {
    kLog.debug("filter \"{}\" shows {}", m_filter, m_rows.size());
  }

  Page& page = m_ui->cur();
  page.rows = m_rows;
  const FileModel::Status st = status();
  page.status = st == FileModel::Status::Denied   ? "denied"
      : st == FileModel::Status::Missing          ? "missing"
      : st == FileModel::Status::Error            ? "error"
      : st == FileModel::Status::Ready && m_rows.empty() && !(m_listing.has_value() && m_listing->page) ? "empty"
                                                  : "";
  syncCustomPage(page);
  if (page.icons != nullptr && page.custom != nullptr) {
    page.empty->setVisible(false);
    page.icons->setVisible(false);
    page.listBox->setVisible(false);
  } else if (page.icons != nullptr) {
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
    page.emptyAction->setVisible(false);
    if (page.status == "empty" && !m_filter.empty()) {
      setEmpty("search", std::format("No results for “{}”", m_filter),
               searchingEverywhere() ? std::string("Searched This PC.") : std::format("Searched {}.", title()));
      page.emptyAction->setVisible(m_host.search && !searchingEverywhere());
    } else if (page.status == "empty") {
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

  if (m_rename.has_value() && !indexOf(m_rename->name).has_value() && !m_rename->busy) {
    cancelRename();
  }
  if (!m_renamePending.empty() && st != FileModel::Status::Loading) {
    if (const auto index = indexOf(m_renamePending)) {
      m_renamePending.clear();
      m_selected = {m_rows[*index]->name};
      m_anchor = m_rows[*index]->name;
      setCursor(*index, true);
      startRename(*index, false);
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

void FileBrowser::syncCustomPage(Page& page) {
  if (page.root == nullptr) {
    return;
  }
  const bool wanted = m_listing.has_value() && m_listing->page;
  if (!(wanted && page.customId == m_listing->id && page.custom != nullptr)) {
    if (page.custom != nullptr) {
      (void)page.root->removeChild(page.custom);
      page.custom = nullptr;
      page.customId.clear();
    }
    if (wanted) {
      page.custom = page.root->addChild(m_listing->page(m_ui->scale));
      page.customId = m_listing->id;
    }
  }
  const bool bannerWanted = m_listing.has_value() && m_listing->banner;
  if (bannerWanted && page.bannerId == m_listing->id && page.banner != nullptr) {
    return;
  }
  if (page.banner != nullptr) {
    (void)page.root->removeChild(page.banner);
    page.banner = nullptr;
    page.bannerId.clear();
  }
  if (bannerWanted) {
    page.banner = page.root->addChild(m_listing->banner(m_ui->scale));
    page.banner->setZIndex(3);
    page.bannerId = m_listing->id;
  }
}

void FileBrowser::rebindPages() {
  if (m_options.view == View::Columns) {
    m_columns->refreshCurrent();
  }
  if (m_options.view == View::Gallery && m_ui->gallery != nullptr) {
    m_ui->gallery->markLayoutDirty(); // a thumbnail arrived
  }
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
  syncColumns();
  syncGallery();
  Ui& ui = *m_ui;
  const float s = ui.scale;
  refreshViewButtons();
  if (ui.zoom != nullptr) {
    ui.zoom->setVisible(m_options.view == View::Icons);
    ui.zoom->setParticipatesInLayout(m_options.view == View::Icons);
  }
  Page& page = ui.cur();
  static constexpr std::array kKeys{FileSortKey::Name, FileSortKey::Modified, FileSortKey::Size, FileSortKey::Kind};
  static constexpr std::array kNames{"Name", "Date Modified", "Size", "Kind"};
  const bool where = searchingEverywhere();
  const FileListRow::Columns cols = FileListRow::columnsFor(ui.stageWidth > 0.0F ? ui.stageWidth : 1000.0F, s, where);
  const std::array<float, 4> widths{0.0F, cols.date, cols.size, cols.kind};
  if (page.whereHeader != nullptr) {
    page.whereHeader->setVisible(where);
    page.whereHeader->setParticipatesInLayout(where);
    page.whereHeader->setMinWidth(cols.where);
    page.whereHeader->setMaxWidth(cols.where);
  }
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
    ui.segments.clear();
    if (m_listing.has_value()) {
      ui.pathRow->addChild(ui::label({.text = m_listing->title, .fontSize = Style::fontSizeCaption * s,
                                      .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label1)}));
    } else {
      const auto segments = pathSegments(m_location);
      for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
          ui.pathRow->addChild(ui::glyph({.glyph = std::string("chevron-right"), .glyphSize = Tokens::sizeSymbolInline * s,
                                          .color = colorSpecFromRole(ColorRole::Label1)}));
        }
        const bool last = i + 1 == segments.size();
        Button* segment = nullptr;
        const Button::ButtonStateColors text{.bg = clearColorSpec(), .border = clearColorSpec(),
                                             .label = colorSpecFromRole(last ? ColorRole::Label1 : ColorRole::Label2)};
        Button::ButtonStateColors hover = text;
        hover.bg = colorSpecFromRole(ColorRole::Fill1);
        ui.pathRow->addChild(ui::button({
            .out = &segment,
            .text = segments[i].first,
            .fontSize = Style::fontSizeCaption * s,
            .variant = ButtonVariant::Ghost,
            .customPalette = Button::ButtonPalette{.normal = text, .hover = last ? text : hover, .pressed = text,
                                                   .disabled = text},
            .paddingH = Tokens::spaceP4 * s,
            .radius = Style::scaledRadius(Tokens::radiusR4, s),
            .height = Tokens::sizeControlS * s,
            .onClick = [this, path = segments[i].second, last] {
              if (last) {
                return;
              }
              DeferredCall::callLater([this, path, alive = std::weak_ptr<bool>(m_alive)] {
                if (!alive.expired()) {
                  go(path);
                }
              });
            },
        }));
        if (Label* label = segment->label(); label != nullptr) {
          label->setFontWeight(FontWeight::Medium);
        }
        ui.segments.emplace_back(segment, segments[i].second);
      }
    }
  }
  if (ui.crumbRow != nullptr) {
    // Mockup .crumb: 500 12 label_2, 12 px chevrons; the last part 600 label_1, accent text in the active pane.
    while (!ui.crumbRow->children().empty()) {
      (void)ui.crumbRow->removeChild(ui.crumbRow->children().back().get());
    }
    const auto parts = m_listing.has_value() ? std::vector<std::pair<std::string, fs::path>>{{m_listing->title, {}}}
                                             : pathSegments(m_location);
    for (std::size_t i = 0; i < parts.size(); ++i) {
      const bool last = i + 1 == parts.size();
      if (i > 0) {
        ui.crumbRow->addChild(ui::glyph({.glyph = std::string("chevron-right"), .glyphSize = Tokens::sizeSymbolInline * s,
                                         .color = colorSpecFromRole(ColorRole::Label2)}));
      }
      ui.crumbRow->addChild(ui::label({
          .text = parts[i].first,
          .fontSize = Style::fontSizeCaption * s,
          .fontWeight = last ? FontWeight::SemiBold : FontWeight::Medium,
          .color = colorSpecFromRole(!last ? ColorRole::Label2 : m_crumbAccent ? ColorRole::AccentText : ColorRole::Label1),
          .maxLines = 1,
      }));
    }
  }
  if (ui.status != nullptr && ui.cur().custom != nullptr) {
    ui.status->setText({});
  } else if (ui.status != nullptr) {
    std::string text = m_selected.empty() ? (m_rows.size() == 1 ? std::string("1 item") : std::format("{} items", m_rows.size()))
                                          : std::format("{} of {} selected", m_selected.size(), m_rows.size());
    if (!m_listing.has_value()) {
      if (const auto free = freeBytes(m_location)) {
        text += std::format(" · {} free", formatFileSize(*free));
      }
    }
    if (!m_statusSuffix.empty()) {
      text += "   " + m_statusSuffix;
    }
    ui.status->setText(text);
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileBrowser::setStatusSuffix(std::string suffix) {
  if (suffix == m_statusSuffix) {
    return;
  }
  m_statusSuffix = std::move(suffix);
  refreshChrome();
}

void FileBrowser::setCrumbAccent(bool accent) {
  if (accent == m_crumbAccent) {
    return;
  }
  m_crumbAccent = accent;
  refreshChrome();
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

VirtualGridView* FileBrowser::activeGrid() const {
  const Page& page = m_ui->pages[m_ui->current];
  switch (m_options.view) {
  case View::Icons:
    return page.icons;
  case View::List:
    return page.list;
  case View::Columns:
    return m_columns->currentGrid();
  case View::Gallery:
    return page.icons; // hidden; the Gallery keeps the icons' order and cursor
  }
  return page.list;
}

void FileBrowser::syncGallery() {
  if (m_options.view != View::Gallery || m_ui->gallery == nullptr) {
    return;
  }
  std::vector<fs::path> items;
  std::size_t current = 0;
  const auto cursor = cursorIndex();
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    const std::string& type = m_rows[i]->contentType;
    if (!type.starts_with("image/") && !type.starts_with("video/")) {
      continue;
    }
    if (cursor == i) {
      current = items.size();
    }
    items.push_back(pathOf(*m_rows[i]));
  }
  if (items == m_galleryItems && current == m_galleryCurrent) {
    return;
  }
  m_galleryItems = items;
  m_galleryCurrent = current;
  kLog.debug("gallery {} of {}", items.empty() ? 0 : current + 1, items.size());
  m_ui->gallery->setItems(std::move(items), current);
}

void FileBrowser::galleryStep(int delta) {
  const auto next = static_cast<std::ptrdiff_t>(m_galleryCurrent) + delta;
  if (next < 0 || next >= static_cast<std::ptrdiff_t>(m_galleryItems.size())) {
    return;
  }
  const std::string name = m_galleryItems[static_cast<std::size_t>(next)].filename().string();
  if (const auto index = indexOf(name)) {
    m_selected = {m_rows[*index]->name};
    m_anchor = m_rows[*index]->name;
    setCursor(*index, false);
    refreshChrome();
    notifyChanged();
  }
}

void FileBrowser::syncColumns() {
  if (m_options.view != View::Columns || m_listing.has_value()) {
    return;
  }
  const FileInfo* single = nullptr;
  if (m_selected.size() == 1) {
    if (const auto index = indexOf(*m_selected.begin())) {
      single = m_rows[*index].get();
    }
  }
  m_columns->update(m_location, single, m_sort, m_options.showHidden);
}

void FileBrowser::applyViewMix() {
  const bool icons = m_options.view == View::Icons;
  for (Page& page : m_ui->pages) {
    if (page.icons == nullptr) {
      continue;
    }
    page.icons->setOpacity(icons ? m_ui->viewMix : 1.0F - m_ui->viewMix);
    page.listBox->setOpacity(icons ? 1.0F - m_ui->viewMix : m_ui->viewMix);
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileBrowser::setView(View view) {
  if (view == m_options.view) {
    return;
  }
  const auto own = [](View v) { return v == View::Columns || v == View::Gallery; };
  const bool fromOwn = own(m_options.view);
  m_options.view = view;
  static constexpr std::array kNames{"icons", "list", "columns", "gallery"};
  kLog.debug("view {}", kNames[static_cast<std::size_t>(view)]);
  if (m_host.viewChanged && !m_searchViewSwitch) {
    m_host.viewChanged(view);
  }
  // The columns view and the Gallery are their own surfaces: they switch at once (no crossfade between unlike layouts).
  if (own(view) || fromOwn) {
    if (m_ui->viewAnim != 0) {
      m_animations.cancel(m_ui->viewAnim);
      m_ui->viewAnim = 0;
    }
    m_ui->viewMix = 1.0F;
    m_ui->cur().root->setVisible(!own(view));
    syncColumns();
    syncGallery();
    refreshRows();
    applyViewMix();
    if (m_host.changed) {
      m_host.changed();
    }
    return;
  }
  Ui& ui = *m_ui;
  MotionTraceLabel trace("files.view");
  ui.viewMix = 0.0F;
  ui.viewAnim = m_animations.drive(
      ui.viewAnim, 0.0F, 1.0F, Style::spring(Tokens::springFade),
      [this](float v) {
        m_ui->viewMix = v;
        applyViewMix();
      },
      [this] {
        m_ui->viewAnim = 0;
        m_ui->viewMix = 1.0F;
        applyViewMix();
        refreshRows();
      },
      m_ui->stage
  );
  refreshRows();
  applyViewMix();
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
  for (auto& [name, e] : m_expanded) {
    e.view->setSort(m_sort);
  }
  const auto start = std::chrono::steady_clock::now();
  if (m_modelView != nullptr) {
    m_modelView->setSort(m_sort);
  } else {
    refreshRows();
  }
  kLog.debug("sort took {:.2f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

void FileBrowser::setShowHidden(bool show) {
  m_options.showHidden = show;
  for (auto& [name, e] : m_expanded) {
    e.view->setShowHidden(show);
  }
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
  std::erase_if(m_selected, [this](const std::string& name) {
    const auto i = indexOf(name);
    return !i.has_value() || !rowEnabled(*m_rows[*i]);
  });
  rebindPages();
  refreshChrome();
  notifyChanged();
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
    activeGrid()->scrollToIndex(index);
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

void FileBrowser::walk(int delta) {
  if (m_rows.empty()) {
    return;
  }
  // 2+ selected: walk that set; else the folder in view order (FILES-UX 3.6).
  if (m_selected.size() > 1) {
    std::vector<std::size_t> set;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
      if (m_selected.contains(m_rows[i]->name)) {
        set.push_back(i);
      }
    }
    const auto cur = cursorIndex();
    auto it = std::ranges::find(set, cur.value_or(set.front()));
    const auto pos = static_cast<std::ptrdiff_t>(it == set.end() ? 0 : it - set.begin());
    const auto n = static_cast<std::ptrdiff_t>(set.size());
    setCursor(set[static_cast<std::size_t>(((pos + delta) % n + n) % n)], true);
    notifyChanged();
    return;
  }
  const auto cur = cursorIndex().value_or(0);
  const auto next = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(cur) + delta, 0,
                                               static_cast<std::ptrdiff_t>(m_rows.size()) - 1);
  select(static_cast<std::size_t>(next), 0);
}

std::optional<std::filesystem::path> FileBrowser::cursorPath() const {
  const auto c = cursorIndex();
  return c.has_value() ? std::optional(pathOf(*m_rows[*c])) : std::nullopt;
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
  if (m_host.pressed) {
    m_host.pressed();
  }
  if (m_waiting || index >= m_rows.size()) {
    return;
  }
  m_keyboardCursor = false;
  const auto now = std::chrono::steady_clock::now();
  const std::string& name = m_rows[index]->name;
  const bool second = name == m_lastPressName && now - m_lastPressAt < kDoubleClick;
  m_lastPressName = second ? std::string() : name;
  m_lastPressAt = now;
  const std::uint32_t mods = m_host.modifiers ? m_host.modifiers() : 0;
  // A plain press on a selected item keeps the selection, so the whole selection can be dragged; a release without a
  // drag then selects only that item (Finder).
  m_collapseOnRelease.reset();
  if (!second && mods == 0 && m_selected.contains(name) && m_selected.size() > 1) {
    m_collapseOnRelease = name;
    setCursor(index, false);
  } else if (!second || !m_selected.contains(name)) {
    select(index, mods);
  }
  if (second) {
    Page& page = m_ui->cur();
    (m_options.view == View::Icons ? page.iconsAdapter : page.listAdapter)->armActivate(index);
  }
}

void FileBrowser::pressItem(std::optional<std::size_t> index) {
  const bool down = index.has_value() && *index < m_rows.size();
  if (down) {
    m_pressName = m_rows[*index]->name;
  } else if (m_pressName.empty()) {
    return;
  }
  const float press = Tokens::motionPressScaleMedium;
  MotionTraceLabel trace("files.press");
  m_pressAnim = m_animations.drive(
      m_pressAnim, m_pressScale, down ? press : 1.0F, Style::spring(Tokens::springPress),
      [this](float v) {
        m_pressScale = v;
        if (m_ui->cur().icons != nullptr) {
          m_ui->cur().icons->notifyDataChanged();
        }
        if (m_host.invalidate) {
          m_host.invalidate();
        }
      },
      [this] {
        m_pressAnim = 0;
        if (m_pressScale >= 1.0F) {
          m_pressName.clear();
        }
      },
      m_ui->cur().icons, 0.0F, Unit::Px);
}

bool FileBrowser::expandable() const {
  return m_options.view == View::List && !m_listing.has_value() && m_filter.empty();
}

bool FileBrowser::listExpandable() const { return expandable(); }

void FileBrowser::setExpanded(std::size_t index, bool expanded) {
  if (index >= m_rows.size() || !m_rows[index]->isDir || !expandable()) {
    return;
  }
  const std::string name = m_rows[index]->name;
  if (expanded == m_expanded.contains(name)) {
    return;
  }
  kLog.debug("{} {}", expanded ? "expand" : "collapse", name);
  if (expanded) {
    Expansion e;
    e.model = FileModel::open(m_location / name, m_watcher);
    e.view = std::make_unique<FileModelView>(e.model);
    e.view->setSort(m_sort);
    e.view->setShowHidden(m_options.showHidden);
    e.view->setChangedCallback([this] { refreshRows(); });
    m_expanded.emplace(name, std::move(e));
  } else {
    // Collapsing drops the folder's open descendants too; a selection inside moves to the folder row.
    const std::string prefix = name + "/";
    std::erase_if(m_expanded, [&](const auto& kv) { return kv.first == name || kv.first.starts_with(prefix); });
    if (std::erase_if(m_selected, [&](const std::string& n) { return n.starts_with(prefix); }) > 0) {
      m_selected.insert(name);
    }
    if (m_cursor.has_value() && m_cursor->starts_with(prefix)) {
      m_cursor = name;
    }
  }
  refreshRows();
}

void FileBrowser::setIconSize(float size) {
  const float grid = Tokens::spaceP16;
  size = std::clamp(std::round(size / grid) * grid, Tokens::componentFilesIconMin, Tokens::componentFilesIconMax);
  if (size == Tokens::componentGridIcon) {
    size = 0.0F;
  }
  if (size == m_iconSize) {
    return;
  }
  m_iconSize = size;
  const float s = m_ui->scale;
  for (Page& page : m_ui->pages) {
    if (page.icons != nullptr) {
      page.icons->setMinCellWidth(FileIconCell::cellWidth(m_iconSize, s));
      page.icons->setCellHeight(FileIconCell::cellHeight(m_iconSize, s));
    }
  }
  refreshChrome();
  rebindPages();
}

float FileBrowser::iconSize() const noexcept { return m_iconSize > 0.0F ? m_iconSize : Tokens::componentGridIcon; }

void FileBrowser::setActive(bool active) {
  if (active == m_active) {
    return;
  }
  m_active = active;
  rebindPages();
}

void FileBrowser::onItemRelease(std::size_t index) {
  if (auto name = std::exchange(m_collapseOnRelease, std::nullopt); name && index < m_rows.size() && m_rows[index]->name == *name) {
    select(index, 0);
  }
}

void FileBrowser::onBandStart(bool icons, float x, float y) {
  if (m_host.pressed) {
    m_host.pressed();
  }
  if (m_waiting) {
    return;
  }
  m_band = Band{.icons = icons, .x0 = x, .y0 = y, .x1 = x, .y1 = y, .moved = false,
                .mods = m_host.modifiers ? m_host.modifiers() : 0U, .before = m_selected};
}

void FileBrowser::backgroundMenu(float sceneX, float sceneY) {
  if (m_host.pressed) {
    m_host.pressed();
  }
  // Finder: the menu is about the folder, so the selection goes.
  clearSelection();
  if (m_host.backgroundMenu) {
    m_host.backgroundMenu(sceneX, sceneY);
  }
}

void FileBrowser::onBandMove(bool icons, float x, float y) {
  if (!m_band.has_value() || m_band->icons != icons) {
    return;
  }
  Band& band = *m_band;
  band.x1 = x;
  band.y1 = y;
  band.moved = true;
  Page& page = m_ui->cur();
  VirtualGridView* grid = icons ? page.icons : page.list;
  grid->setBandRect(LayoutRect{.x = std::min(band.x0, x), .y = std::min(band.y0, y),
                               .width = std::abs(x - band.x0), .height = std::abs(y - band.y0)});
  const auto hit = grid->indicesInRect(band.x0, band.y0, x, y);
  // No modifier: the band is the selection. Shift adds to the selection the band started with; Ctrl toggles it.
  std::set<std::string> next = (band.mods & (KeyMod::Ctrl | KeyMod::Shift)) != 0 ? band.before : std::set<std::string>{};
  for (const std::size_t i : hit) {
    if (i >= m_rows.size() || !rowEnabled(*m_rows[i])) {
      continue;
    }
    const std::string& name = m_rows[i]->name;
    if ((band.mods & KeyMod::Ctrl) != 0 && band.before.contains(name)) {
      next.erase(name);
    } else {
      next.insert(name);
    }
  }
  if (next != m_selected) {
    m_selected = std::move(next);
    if (!hit.empty()) {
      m_cursor = m_rows[hit.back()]->name;
      m_anchor = m_rows[hit.front()]->name;
    }
    rebindPages();
    refreshChrome();
  }
}

void FileBrowser::onBandEnd(bool icons) {
  if (!m_band.has_value()) {
    return;
  }
  const Band band = *m_band;
  m_band.reset();
  Page& page = m_ui->cur();
  (icons ? page.icons : page.list)->setBandRect(std::nullopt);
  if (!band.moved && (band.mods & (KeyMod::Ctrl | KeyMod::Shift)) == 0) {
    clearSelection(); // a click on empty space
    return;
  }
  kLog.debug("band selected {}", m_selected.size());
  notifyChanged();
}

void FileBrowser::onItemActivate(std::size_t index) {
  if (m_waiting || index >= m_rows.size() || (m_listing.has_value() && !m_listing->activatable)) {
    return;
  }
  const FileInfo& info = *m_rows[index];
  if (!rowEnabled(info)) {
    return;
  }
  if (m_listing.has_value() && m_listing->open && m_listing->open(info)) {
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

bool FileBrowser::activateSelection() {
  if (m_selected.size() != 1) {
    return false;
  }
  const auto index = indexOf(*m_selected.begin());
  if (!index.has_value()) {
    return false;
  }
  onItemActivate(*index);
  return true;
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
  if (m_waiting) {
    return false; // the rows on screen belong to the folder we are leaving
  }
  if (event.sym == XKB_KEY_space && m_springTarget.has_value()) {
    springLoad(); // Space spring-loads the hovered folder at once
    return true;
  }
  if (event.sym == XKB_KEY_F2 && mods == 0) {
    beginRename();
    return true;
  }
  if (m_options.view == View::Gallery && mods == 0) {
    if (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right) {
      galleryStep(event.sym == XKB_KEY_Left ? -1 : 1);
      return true;
    }
    if (event.sym == XKB_KEY_Escape) {
      setView(View::Icons);
      return true;
    }
  }
  if (event.sym == XKB_KEY_t && mods == KeyMod::Ctrl) {
    newTab();
    return true;
  }
  if ((event.sym == XKB_KEY_Tab || event.sym == XKB_KEY_ISO_Left_Tab) && (mods & KeyMod::Ctrl) != 0 && m_tabs.size() > 1) {
    stepTab((mods & KeyMod::Shift) != 0 ? -1 : 1);
    return true;
  }
  m_keyboardCursor = true;
  const bool ctrl = (mods & KeyMod::Ctrl) != 0;
  const bool alt = (mods & KeyMod::Alt) != 0;
  const auto cols = static_cast<std::ptrdiff_t>(columns());
  const Page& page = m_ui->cur();
  const auto pageRows = static_cast<std::ptrdiff_t>(
      page.icons != nullptr ? activeGrid()->pageItemStride() : 10);

  switch (event.sym) {
  case XKB_KEY_Left:
    if (alt) {
      back();
    } else if (m_options.view == View::Icons) {
      moveCursor(-1, mods);
    } else if (m_options.view == View::Columns && !m_listing.has_value()) {
      up(); // FILES-UX 3.2: the parent column, with the folder we left selected
    } else if (expandable()) {
      // Finder: Left collapses an expanded folder, else goes to the row's parent folder row.
      const auto c = cursorIndex();
      if (!c.has_value()) {
        return false;
      }
      const std::string name = m_rows[*c]->name;
      if (m_expanded.contains(name)) {
        setExpanded(*c, false);
      } else if (const auto slash = name.rfind('/'); slash != std::string::npos) {
        if (const auto parent = indexOf(name.substr(0, slash))) {
          select(*parent, 0);
        }
      }
    } else {
      return false;
    }
    return true;
  case XKB_KEY_Right:
    if (alt) {
      forward();
    } else if (m_options.view == View::Icons) {
      moveCursor(1, mods);
    } else if (m_options.view == View::Columns) {
      // Into the one selected folder; Right on a file does nothing (the preview is not a focus stop).
      if (const auto rows = selectedRows(); rows.size() == 1 && rows.front()->isDir) {
        go(pathOf(*rows.front()));
      }
    } else if (expandable()) {
      if (const auto c = cursorIndex(); c.has_value() && m_rows[*c]->isDir) {
        setExpanded(*c, true);
      }
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
    // Space inside the type-ahead time is part of a name (D1); otherwise it is Quick Look.
    if (mods == 0 && m_host.quickLook && !m_typeAhead.active(std::chrono::steady_clock::now())) {
      const auto rows = selectedRows();
      const auto c = cursorIndex();
      if (rows.size() == 1 || (!rows.empty() && c.has_value())) {
        m_host.quickLook(pathOf(rows.size() == 1 ? *rows.front() : *m_rows[*c]));
        return true;
      }
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
    case XKB_KEY_3:
      setView(View::Columns);
      return true;
    case XKB_KEY_4:
      setView(View::Gallery);
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

// E2E scripts aim the pointer from this log (DESK_FILES_GEOMETRY=1): surface-local centres, after each layout change.
void FileBrowser::logGeometry() {
  static const bool enabled = std::getenv("DESK_FILES_GEOMETRY") != nullptr;
  if (!enabled) {
    return;
  }
  const auto centre = [](const Node* node) {
    float x = 0.0F;
    float y = 0.0F;
    Node::mapToScene(node, node->width() / 2.0F, node->height() / 2.0F, x, y);
    return std::pair{static_cast<int>(x), static_cast<int>(y)};
  };
  std::string out;
  VirtualGridView* grid = activeGrid();
  for (std::size_t i = 0; grid != nullptr && i < std::min<std::size_t>(m_rows.size(), 32); ++i) {
    float x = 0.0F;
    float y = 0.0F;
    if (grid->absoluteAnchorForIndex(i, x, y)) {
      out += std::format("item {} {} {}\n", m_rows[i]->displayName, static_cast<int>(x), static_cast<int>(y));
    }
  }
  if (grid != nullptr) {
    const auto [x, y] = centre(grid);
    out += std::format("view {} {}\n", x, y + static_cast<int>(grid->height() / 2.0F) - 8);
  }
  if (m_options.view == View::Columns) {
    m_columns->appendGeometry(out);
  }
  for (const auto& [node, place] : m_ui->sidebarRows) {
    const auto [x, y] = centre(node);
    out += std::format("place {} {} {}\n", place.label, x, y);
  }
  for (const auto& [node, name] : m_ui->chipNodes) {
    if (node->visible() && m_ui->chips->visible()) {
      const auto [x, y] = centre(node);
      out += std::format("chip {} {} {}\n", name, x, y);
    }
  }
  if (grid != nullptr && expandable()) {
    float gx = 0.0F;
    float gy = 0.0F;
    Node::mapToScene(grid, 0.0F, 0.0F, gx, gy);
    for (std::size_t i = 0; i < std::min<std::size_t>(m_rows.size(), 32); ++i) {
      float x = 0.0F;
      float y = 0.0F;
      if (m_rows[i]->isDir && grid->absoluteAnchorForIndex(i, x, y)) {
        const auto [from, to] = FileListRow::disclosureSpan(static_cast<int>(std::ranges::count(m_rows[i]->name, '/')),
                                                            m_ui->scale);
        out += std::format("disclosure {} {} {}\n", m_rows[i]->name, static_cast<int>(gx + (from + to) / 2.0F),
                           static_cast<int>(y));
      }
    }
  }
  if (const Flex* strip = m_ui->tabStrip; strip != nullptr && strip->visible()) {
    std::size_t t = 0;
    for (const auto& child : strip->children()) {
      if (dynamic_cast<const SidebarRow*>(child.get()) != nullptr) {
        const auto [x, y] = centre(child.get());
        out += std::format("tab {} {} {}\n", t, x, y);
        for (const auto& part : child->children()) {
          if (dynamic_cast<const Button*>(part.get()) != nullptr) {
            const auto [cx, cy] = centre(part.get());
            out += std::format("tab-close {} {} {}\n", t, cx, cy);
          }
        }
        ++t;
      } else if (dynamic_cast<const Button*>(child.get()) != nullptr) {
        const auto [x, y] = centre(child.get());
        out += std::format("tab-add {} {}\n", x, y);
      }
    }
  }
  static constexpr std::array kHeaderNames{"Name", "Date", "Size", "Kind"};
  for (std::size_t c = 0; c < kHeaderNames.size(); ++c) {
    if (const Button* b = m_ui->cur().sortButtons[c]; b != nullptr && b->visible() && m_options.view == View::List) {
      const auto [x, y] = centre(b);
      out += std::format("sort-header {} {} {}\n", kHeaderNames[c], x, y);
    }
  }
  if (const Node* banner = m_ui->cur().banner; banner != nullptr) {
    int n = 0; // the banner's buttons in tree order
    const std::function<void(const Node*)> walk = [&](const Node* node) {
      if (dynamic_cast<const Button*>(node) != nullptr) {
        const auto [x, y] = centre(node);
        out += std::format("banner-action {} {} {}\n", n++, x, y);
        return;
      }
      for (const auto& child : node->children()) {
        walk(child.get());
      }
    };
    walk(banner);
  }
  for (const auto& [node, path] : m_ui->segments) {
    const auto [x, y] = centre(node);
    out += std::format("segment {} {} {}\n", path.string(), x, y);
  }
  if (out == m_geometry) {
    return;
  }
  m_geometry = out;
  std::string_view rest = out;
  while (!rest.empty()) {
    const std::size_t nl = rest.find('\n');
    kLog.debug("geometry {}", rest.substr(0, nl));
    rest.remove_prefix(nl + 1);
  }
}

// ---- drag and drop ------------------------------------------------------------------------------------------------

void FileBrowser::onItemDrag(std::size_t index) {
  m_collapseOnRelease.reset();
  if (m_waiting || index >= m_rows.size() || !m_host.startDrag || m_rename.has_value()) {
    return;
  }
  // Dragging an unselected item drags only it (Finder, Explorer).
  if (!m_selected.contains(m_rows[index]->name)) {
    select(index, 0);
  }
  m_dragPaths = selectedPaths();
  if (m_dragPaths.empty() || !m_host.startDrag(m_dragPaths)) {
    m_dragPaths.clear();
    return;
  }
  kLog.debug("drag {} items", m_dragPaths.size());
}

namespace {
  // A drop into one of the dragged items or below one of them would loop.
  bool insideDragged(const fs::path& target, const std::vector<fs::path>& dragged) {
    for (const fs::path& p : dragged) {
      const auto [end, _] = std::ranges::mismatch(p, target);
      if (end == p.end()) {
        return true;
      }
    }
    return false;
  }

  std::string lowerAscii(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
  }

  bool writable(const fs::path& dir) { return ::access(dir.c_str(), W_OK | X_OK) == 0; }

  // The Where column of a search result: the folder, the home part as ~.
  std::string whereLabel(const fs::path& dir) {
    const char* home = std::getenv("HOME");
    const std::string d = dir.string();
    if (home != nullptr && *home != '\0') {
      const std::string h(home);
      if (d == h) {
        return "~";
      }
      if (d.starts_with(h + "/")) {
        return "~" + d.substr(h.size());
      }
    }
    return d;
  }

  bool matchesKind(const FileInfo& info, FileBrowser::SearchKind kind) {
    const std::string_view t = info.contentType;
    const auto has = [&](std::string_view part) { return t.find(part) != std::string_view::npos; };
    switch (kind) {
    case FileBrowser::SearchKind::All:
      return true;
    case FileBrowser::SearchKind::Pdf:
      return t == "application/pdf";
    case FileBrowser::SearchKind::Image:
      return t.starts_with("image/");
    case FileBrowser::SearchKind::Doc:
      return !info.isDir && ((t.starts_with("text/") && t != "text/csv") || has("msword") || has("wordprocessing") ||
                             has("opendocument.text") || t == "application/rtf");
    case FileBrowser::SearchKind::Sheet:
      return t == "text/csv" || has("spreadsheet") || has("ms-excel");
    case FileBrowser::SearchKind::Recent: {
      constexpr std::int64_t kThirtyDaysNs = 30LL * 24 * 3600 * 1000000000LL;
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
      return info.mtimeNs >= now - kThirtyDaysNs;
    }
    }
    return true;
  }

  bool contains(const Node* node, float sceneX, float sceneY) {
    float x = 0.0F;
    float y = 0.0F;
    return node != nullptr && node->visible() && Node::mapFromScene(node, sceneX, sceneY, x, y);
  }
} // namespace

std::optional<FileBrowser::DropTarget> FileBrowser::dragOver(float sceneX, float sceneY) {
  const auto accept = [this](const fs::path& folder) -> std::optional<DropTarget> {
    if (folder.empty() || insideDragged(folder, m_dragPaths) || !writable(folder)) {
      return std::nullopt;
    }
    // Dropping items back into the folder they come from does nothing.
    if (!m_dragPaths.empty() && std::ranges::all_of(m_dragPaths, [&](const fs::path& p) { return p.parent_path() == folder; })) {
      return std::nullopt;
    }
    return DropTarget{.folder = folder};
  };
  std::optional<DropTarget> target;
  std::optional<std::string> item;
  std::optional<Spring> spring;

  for (const auto& [node, place] : m_ui->sidebarRows) {
    if (contains(node, sceneX, sceneY)) {
      if (place.dropTrash) {
        target = m_dragPaths.empty() ? std::nullopt : std::optional<DropTarget>(DropTarget{.trash = true});
      } else if (!place.path.empty()) {
        target = accept(place.path);
        spring = Spring{.target = DropTarget{.folder = place.path}, .sidebar = true};
      }
      goto found;
    }
  }
  for (const auto& [node, path] : m_ui->segments) {
    if (contains(node, sceneX, sceneY)) {
      target = accept(path);
      if (path != m_location) {
        spring = Spring{.target = DropTarget{.folder = path}};
      }
      goto found;
    }
  }
  {
    Page& page = m_ui->cur();
    VirtualGridView* grid = activeGrid();
    const bool overView = contains(page.root, sceneX, sceneY);
    if (grid != nullptr && overView) {
      if (const auto index = grid->indexAtScene(sceneX, sceneY); index && *index < m_rows.size()) {
        const FileInfo& info = *m_rows[*index];
        const fs::path path = pathOf(info);
        if (info.isDir && !std::ranges::contains(m_dragPaths, path)) {
          target = accept(path);
          if (target.has_value()) {
            item = info.name;
            spring = Spring{.target = *target};
          }
          goto found;
        }
      }
      // Empty space or a file: the view's folder. Listings (Trash, Recents) take no drops.
      if (!m_listing.has_value()) {
        target = accept(m_location);
      }
    }
  }
found:
  updateEdgeScroll(sceneX, sceneY);
  setDropCue(item);
  const bool sameSpring = spring.has_value() && m_springTarget.has_value() && spring->target == m_springTarget->target;
  if (!sameSpring) {
    m_springTimer.stop();
    m_springTarget = spring;
    if (spring.has_value()) {
      m_springTimer.start(kSpringLoad, [this] { springLoad(); });
    }
  }
  if (target != m_lastDrop) {
    m_lastDrop = target;
    kLog.debug("drop target {}", target.has_value() ? (target->trash ? std::string("trash") : target->folder.string())
                                                     : std::string("none"));
  }
  return target;
}

void FileBrowser::setDropCue(std::optional<std::string> item) {
  if (item == m_dropItem) {
    return;
  }
  m_dropItem = std::move(item);
  rebindPages();
}

void FileBrowser::springLoad() {
  if (!m_springTarget.has_value()) {
    return;
  }
  const Spring spring = *std::exchange(m_springTarget, std::nullopt);
  m_springTimer.stop();
  if (!m_springFrom.has_value()) {
    m_springFrom = SpringFrom{.entry = current(), .back = m_back, .forward = m_forward};
  }
  setDropCue(std::nullopt);
  kLog.debug("spring-load {}", spring.target.folder.string());
  if (spring.sidebar) {
    jump(spring.target.folder);
  } else {
    go(spring.target.folder);
  }
}

void FileBrowser::updateEdgeScroll(float sceneX, float sceneY) {
  Page& page = m_ui->cur();
  VirtualGridView* grid = activeGrid();
  float x = 0.0F;
  float y = 0.0F;
  const bool inside = grid != nullptr && Node::mapFromScene(grid, sceneX, sceneY, x, y);
  const float band = Tokens::spaceP32 * m_ui->scale;
  constexpr float kMaxSpeed = 1600.0F; // px/s at the very edge
  float speed = 0.0F;
  if (inside && y < band) {
    speed = -kMaxSpeed * (band - y) / band;
  } else if (inside && y > grid->height() - band) {
    speed = kMaxSpeed * (y - (grid->height() - band)) / band;
  }
  m_edgeSpeed = speed * m_ui->scale;
  if (speed == 0.0F) {
    m_edgeScroll.stop();
  } else if (!m_edgeScroll.active()) {
    constexpr auto kTick = std::chrono::milliseconds(16);
    m_edgeScroll.startRepeating(kTick, [this] {
      Page& p = m_ui->cur();
      VirtualGridView* g = activeGrid();
      if (g == nullptr || m_edgeSpeed == 0.0F) {
        m_edgeScroll.stop();
        return;
      }
      g->scrollView().scrollBy(m_edgeSpeed * 0.016F);
      if (m_host.invalidate) {
        m_host.invalidate();
      }
    });
  }
}

void FileBrowser::dragLeave() {
  m_edgeScroll.stop();
  m_edgeSpeed = 0.0F;
  m_springTimer.stop();
  m_springTarget.reset();
  m_lastDrop.reset();
  setDropCue(std::nullopt);
}

void FileBrowser::dragEnded() {
  dragLeave();
  m_dragPaths.clear();
  if (auto from = std::exchange(m_springFrom, std::nullopt); from.has_value()) {
    kLog.debug("spring-load returns to {}", from->entry.path.string());
    // D15: back where the drag started, with the history it had; the spring-loaded hops leave none.
    m_back = std::move(from->back);
    m_forward = std::move(from->forward);
    navigate(std::move(from->entry), Motion::Back);
  }
}

float FileBrowser::dragImageSize() const noexcept {
  return (Tokens::componentGridIcon + Tokens::spaceP24) * m_ui->scale;
}

std::unique_ptr<Node> FileBrowser::buildDragImage() const {
  const float s = m_ui->scale;
  const float plate = (Tokens::componentGridIcon + Tokens::spaceP8) * s;
  const float step = Tokens::spaceP4 * s;
  auto root = ui::node({});
  root->setSize(dragImageSize(), dragImageSize());
  const auto paths = m_dragPaths.empty() ? selectedPaths() : m_dragPaths;
  const std::size_t shown = std::min<std::size_t>(paths.size(), 3);
  for (std::size_t i = shown; i-- > 0;) {
    std::shared_ptr<const FileInfo> info;
    for (const auto& row : m_rows) {
      if (pathOf(*row) == paths[i]) {
        info = row;
        break;
      }
    }
    auto box = ui::box({.fill = colorSpecFromRole(ColorRole::Popover), .radius = Style::scaledRadius(Tokens::radiusR8, s)});
    const float offset = static_cast<float>(i) * step;
    box->setPosition(offset, Tokens::spaceP12 * s + offset);
    box->setSize(plate, plate);
    box->setOpacity(i == 0 ? 1.0F : 0.88F);
    auto glyph = ui::glyph({.glyph = info != nullptr ? fileTypeGlyph(*info) : std::string("file"),
                            .glyphSize = Tokens::spaceP48 * s,
                            .color = colorSpecFromRole(info != nullptr && info->isDir ? ColorRole::AccentText
                                                                                       : ColorRole::Label2)});
    glyph->setPosition((plate - Tokens::spaceP48 * s) / 2.0F, (plate - Tokens::spaceP48 * s) / 2.0F);
    box->addChild(std::move(glyph));
    root->addChild(std::move(box));
  }
  if (paths.size() > 1) {
    const float badge = Tokens::spaceP24 * s;
    auto pill = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                         .paddingH = Tokens::spaceP8 * s, .fill = colorSpecFromRole(ColorRole::Accent),
                         .radius = badge / 2.0F, .minWidth = badge, .height = badge});
    pill->addChild(ui::label({.text = std::to_string(paths.size()), .fontSize = Style::fontSizeCaption * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::OnAccent)}));
    pill->setPosition(plate - badge / 2.0F, 0.0F);
    root->addChild(std::move(pill));
  }
  return root;
}

// ---- inline rename ------------------------------------------------------------------------------------------------

void FileBrowser::beginRename() {
  if (m_waiting || m_listing.has_value() || m_selected.size() != 1 || !m_host.rename) {
    return;
  }
  if (const auto index = indexOf(*m_selected.begin())) {
    startRename(*index, false);
  }
}

void FileBrowser::renameWhenListed(std::string name) {
  m_renamePending = std::move(name);
  refreshRows();
}

void FileBrowser::startRename(std::size_t index, bool all) {
  Ui& ui = *m_ui;
  if (index >= m_rows.size() || ui.renameField == nullptr) {
    return;
  }
  const FileInfo& info = *m_rows[index];
  m_rename = Rename{.name = info.name, .text = info.displayName};
  setCursor(index, true);
  Input& field = *ui.renameField;
  field.setValue(info.displayName);
  field.setInvalid(false);
  field.setFontSize((m_options.view == View::Icons ? Style::fontSizeCaption : Style::fontSizeBody) * ui.scale);
  ui.renameError->setVisible(false);
  // F2 selects the stem: "report.final.pdf" -> "report.final"; folders and dotfiles select all.
  const std::string& name = info.displayName;
  const std::size_t dot = name.rfind('.');
  if (all || info.isDir || dot == std::string::npos || dot == 0) {
    field.selectAll();
  } else {
    field.selectRange(0, dot);
  }
  ui.renameBox->setVisible(true);
  if (m_host.focus) {
    m_host.focus(field.inputArea());
  }
  kLog.debug("rename {}", info.displayName);
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileBrowser::placeRenameField() {
  Ui& ui = *m_ui;
  const auto index = m_rename.has_value() ? indexOf(m_rename->name) : std::nullopt;
  Page& page = ui.cur();
  VirtualGridView* grid = activeGrid();
  Node* cell = index.has_value() && grid != nullptr ? grid->nodeForIndex(*index) : nullptr;
  if (cell == nullptr || ui.stage == nullptr) {
    ui.renameBox->setVisible(false);
    return;
  }
  const float s = ui.scale;
  Node* name = m_options.view == View::Icons ? static_cast<FileIconCell*>(cell)->nameNode()
                                             : static_cast<FileListRow*>(cell)->nameNode();
  float sx = 0.0F;
  float sy = 0.0F;
  float x = 0.0F;
  float y = 0.0F;
  float cx = 0.0F;
  float cy = 0.0F;
  Node::mapToScene(name, 0.0F, 0.0F, sx, sy);
  (void)Node::mapFromScene(ui.stage, sx, sy, x, y);
  Node::mapToScene(cell, 0.0F, 0.0F, sx, sy);
  (void)Node::mapFromScene(ui.stage, sx, sy, cx, cy);
  const float fieldH = Tokens::sizeControlS * s;
  float width = 0.0F;
  float left = 0.0F;
  if (m_options.view == View::Icons) {
    // At least the cell, centred on it; long names widen it up to two cells.
    width = std::clamp(name->width() + Tokens::spaceP16 * s, cell->width(), cell->width() * 2.0F);
    left = cx + (cell->width() - width) / 2.0F;
  } else {
    left = x - Tokens::spaceP4 * s;
    width = std::max(name->width(), Tokens::componentFilesNameMin * s) + Tokens::spaceP8 * s;
  }
  left = std::clamp(left, 0.0F, std::max(0.0F, ui.stageWidth - width));
  const float top = m_options.view == View::Icons ? y - Tokens::spaceP2 * s : cy + (cell->height() - fieldH) / 2.0F;
  ui.renameBox->setVisible(true);
  ui.renameBox->setPosition(std::round(left), std::round(top));
  ui.renameBox->setSize(width, fieldH + (ui.renameError->visible() ? Tokens::spaceP32 * s : 0.0F));
  ui.renameField->setMinLayoutWidth(width);
}

void FileBrowser::cancelRename() {
  if (!m_rename.has_value()) {
    return;
  }
  m_rename.reset();
  if (m_ui->renameBox != nullptr) {
    m_ui->renameBox->setVisible(false);
  }
  if (m_host.focus) {
    m_host.focus(m_ui->focus);
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileBrowser::commitRename(bool next) {
  if (!m_rename.has_value() || m_rename->busy || m_ui->renameField == nullptr) {
    return;
  }
  const auto index = indexOf(m_rename->name);
  if (!index.has_value()) {
    cancelRename();
    return;
  }
  const FileInfo& info = *m_rows[*index];
  const std::string value = m_ui->renameField->value();
  const auto showError = [this](std::string error) {
    if (!m_rename.has_value()) {
      return;
    }
    m_rename->busy = false;
    m_rename->error = std::move(error);
    m_ui->renameField->setInvalid(true);
    m_ui->renameError->setText(m_rename->error);
    m_ui->renameError->setVisible(true);
    kLog.debug("rename error {}", m_rename->error);
    if (m_host.invalidate) {
      m_host.invalidate();
    }
  };
  const auto advance = [this, next, index] {
    cancelRename();
    if (next && *index + 1 < m_rows.size()) {
      m_selected = {m_rows[*index + 1]->name};
      m_anchor = m_rows[*index + 1]->name;
      startRename(*index + 1, false);
      refreshChrome();
    }
  };
  if (value == info.displayName) {
    advance();
    return;
  }
  if (value.empty() || value == "." || value == "..") {
    showError("Enter a name.");
    return;
  }
  if (value.find('/') != std::string::npos) {
    showError("A name can’t contain “/”.");
    return;
  }
  if (value.size() > 255) {
    showError("The name is too long.");
    return;
  }
  std::error_code ec;
  if (fs::exists(m_location / value, ec) && value != info.displayName) {
    // Case-only renames on the same entry are fine; another entry with that name is not.
    if (lowerAscii(value) != lowerAscii(info.displayName)) {
      showError(std::format("“{}” is already taken.", value));
      return;
    }
  }
  m_rename->busy = true;
  if (!next) {
    m_pendingSelect = {value}; // the renamed item stays selected once the listing has it
  }
  m_host.rename(pathOf(info), value, [this, alive = std::weak_ptr<bool>(m_alive), advance, showError](const std::string& error) {
    if (alive.expired()) {
      return;
    }
    if (!error.empty()) {
      m_pendingSelect.clear();
      showError(error);
      return;
    }
    advance();
  });
}

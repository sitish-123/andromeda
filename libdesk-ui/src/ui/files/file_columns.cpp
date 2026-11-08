#include "ui/files/file_columns.h"

#include "core/log.h"
#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/scene/accessible.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/controls/button.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <format>
#include <glib.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("file-columns");
  const float& kColumnWidth = Tokens::componentFilesColumnWidth;

  // The chain starts at home for paths under it (Finder starts at the volume; home is where users live), else at /.
  std::vector<fs::path> chainTo(const fs::path& location) {
    const fs::path home = g_get_home_dir();
    const std::string loc = location.string();
    fs::path root = (loc == home.string() || loc.starts_with(home.string() + "/")) ? home : fs::path("/");
    std::vector<fs::path> chain{root};
    const fs::path rel = location.lexically_relative(root);
    fs::path cur = root;
    for (const fs::path& part : rel) {
      if (part.empty() || part == ".") {
        continue;
      }
      cur /= part;
      chain.push_back(cur);
    }
    return chain;
  }
} // namespace

// One column's rows bound to its pooled list cells.
class ColumnAdapter final : public VirtualGridAdapter {
public:
  std::function<const std::vector<std::shared_ptr<const FileInfo>>&()> rows;
  std::function<FileCellState(const FileInfo&, std::size_t)> state;
  std::function<void(std::size_t)> press;
  std::function<void(std::size_t)> activate;
  Renderer* renderer = nullptr;
  float scale = 1.0F;

private:
  std::optional<std::size_t> m_last;
  std::optional<std::size_t> m_activate;
  std::chrono::steady_clock::time_point m_lastAt{};

public:

  [[nodiscard]] std::size_t itemCount() const override { return rows().size(); }
  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<FileListRow>(scale); }
  void bindTile(Node& tile, std::size_t index, bool /*selected*/, bool hovered) override {
    const auto& r = rows();
    if (renderer == nullptr || index >= r.size()) {
      return;
    }
    FileCellState s = state(*r[index], index);
    s.hovered = hovered && !s.selected;
    s.nameOnly = true;
    static_cast<FileListRow&>(tile).bind(*renderer, *r[index], tile.width(), s);
  }
  bool onPointerPress(std::size_t index, float, float, float, float) override {
    kLog.debug("column pointer press {}", index);
    press(index);
    // A second press within the double-click time activates on release (as the browser's own views do).
    const auto now = std::chrono::steady_clock::now();
    m_activate = m_last == index && now - m_lastAt < std::chrono::milliseconds(400) ? std::optional(index) : std::nullopt;
    m_last = m_activate.has_value() ? std::nullopt : std::optional(index);
    m_lastAt = now;
    return true;
  }
  PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
    PointerReleaseResult result;
    if (index.has_value() && m_activate == index) {
      result.activateIndex = index;
    }
    m_activate.reset();
    return result;
  }
  void onActivate(std::size_t index) override { activate(index); }
  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    const auto& r = rows();
    return index < r.size() ? r[index]->displayName : std::string();
  }
};

struct FileColumns::Column {
  fs::path dir;
  bool current = false;
  std::shared_ptr<FileModel> model;
  std::unique_ptr<FileModelView> view;
  std::vector<std::shared_ptr<const FileInfo>> empty;
  std::string highlight; // the child of this column that the chain continues with
  ScrollViewState scroll;
  ColumnAdapter adapter;
  VirtualGridView* grid = nullptr;
};

FileColumns::FileColumns(DirectoryWatcher& watcher, AnimationManager& animations, Host host)
    : m_watcher(watcher), m_animations(animations), m_host(std::move(host)) {}

FileColumns::~FileColumns() = default;

std::unique_ptr<FileColumns::Column> FileColumns::makeColumn(const fs::path& dir, bool current) {
  auto col = std::make_unique<Column>();
  col->dir = dir;
  col->current = current;
  Column* c = col.get();
  if (current) {
    c->adapter.rows = m_host.rows;
    c->adapter.state = m_host.state;
    c->adapter.press = m_host.press;
    c->adapter.activate = m_host.activate;
  } else {
    c->model = FileModel::open(dir, m_watcher);
    c->view = std::make_unique<FileModelView>(c->model);
    c->view->setSort(m_sort);
    c->view->setShowHidden(m_showHidden);
    c->view->setChangedCallback([this, c] {
      if (c->grid != nullptr) {
        c->grid->notifyDataChanged();
      }
      if (m_host.invalidate) {
        m_host.invalidate();
      }
    });
    c->adapter.rows = [c]() -> const std::vector<std::shared_ptr<const FileInfo>>& {
      return c->view != nullptr ? c->view->rows() : c->empty;
    };
    // mockup §3: the path through the ancestors shows in the accent tint, with the same icons as the current column.
    c->adapter.state = [this, c](const FileInfo& info, std::size_t) {
      FileCellState s;
      s.selected = info.name == c->highlight;
      s.ghost = info.hidden;
      s.path = c->dir / info.name;
      s.thumbnails = m_host.thumbnails;
      return s;
    };
    // A press in an ancestor or the trailing column moves the current folder there; a folder opens as the next column.
    c->adapter.press = [this, c](std::size_t index) {
      const auto& rows = c->view->rows();
      if (index >= rows.size()) {
        return;
      }
      const std::string name = rows[index]->name;
      const fs::path dir = c->dir;
      kLog.debug("column press {} in {}", name, dir.string());
      DeferredCall::callLater([goSelect = m_host.goSelect, dir, name] { goSelect(dir, name); });
    };
    c->adapter.activate = [](std::size_t) {};
  }
  c->adapter.scale = m_scale;
  c->adapter.renderer = m_renderer;
  return col;
}

void FileColumns::update(const fs::path& location, const FileInfo* selected, const FileSort& sort, bool showHidden) {
  const bool settings = sort.key != m_sort.key || sort.descending != m_sort.descending || showHidden != m_showHidden;
  m_sort = sort;
  m_showHidden = showHidden;
  const auto chain = chainTo(location);
  const std::optional<fs::path> trailing =
      selected != nullptr && selected->isDir ? std::optional<fs::path>(location / selected->name) : std::nullopt;

  // Keep the columns whose folder and role still match; rebuild from the first difference.
  std::vector<std::unique_ptr<Column>> next;
  const std::size_t want = chain.size() + (trailing.has_value() ? 1 : 0);
  for (std::size_t i = 0; i < want; ++i) {
    const fs::path dir = i < chain.size() ? chain[i] : *trailing;
    const bool current = i + 1 == chain.size();
    if (!settings && i < m_columns.size() && m_columns[i] != nullptr && m_columns[i]->dir == dir
        && m_columns[i]->current == current) {
      next.push_back(std::move(m_columns[i]));
    } else {
      next.push_back(makeColumn(dir, current));
    }
    if (i + 1 < chain.size()) {
      next.back()->highlight = chain[i + 1].filename().string();
    } else if (i + 1 == chain.size()) {
      next.back()->highlight.clear();
    }
  }
  const bool changed = next.size() != m_columns.size() || location != m_location || trailing != m_trailingDir;
  m_columns = std::move(next);
  m_location = location;
  m_trailingDir = trailing;
  m_previewFor = selected != nullptr && !selected->isDir ? selected->name : std::string();
  if (changed) {
    rebuildColumns();
    scrollToEnd();
  }
  showPreview(selected != nullptr && !selected->isDir ? selected : nullptr);
}

std::unique_ptr<Node> FileColumns::build(float scale) {
  m_scale = scale;
  for (auto& col : m_columns) {
    col->adapter.scale = scale;
    col->grid = nullptr;
  }
  auto scroll = ui::scrollView({.out = &m_scroll, .contentScale = scale, .orientation = ScrollOrientation::Horizontal,
                                .scrollbarAutoHide = true, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F,
                                .flexGrow = 1.0F});
  m_row = scroll->content();
  m_row->setDirection(FlexDirection::Horizontal);
  m_row->setAlign(FlexAlign::Stretch);
  m_row->setGap(0.0F);
  rebuildColumns();
  return scroll;
}

void FileColumns::rebuildColumns() {
  if (m_row == nullptr) {
    return;
  }
  const float s = m_scale;
  while (!m_row->children().empty()) {
    (void)m_row->removeChild(m_row->children().back().get());
  }
  m_preview = nullptr;
  for (auto& col : m_columns) {
    col->adapter.scale = s;
    auto grid = ui::virtualGridView({
        .out = &col->grid,
        .state = &col->scroll,
        .contentScale = s,
        .columns = 1,
        .cellHeight = std::ceil(Tokens::sizeRowCompact * s),
        .squareCells = false,
        .columnGap = 0.0F,
        .rowGap = 0.0F,
        .overscanRows = 3,
        .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        .scrollbarVisible = true,
        .adapter = &col->adapter,
        .width = kColumnWidth * s,
    });
    Accessible& a = grid->accessibleState();
    a.role = AccessibleRole::List;
    a.name = col->dir.filename().empty() ? col->dir.string() : col->dir.filename().string();
    // A grid measures 0 wide without a width limit, and the horizontal scroll measures unconstrained: the fixed
    // column gives the row its real width, so it overflows and scrolls.
    // Mockup .col: 220 wide, padding 4 (rows inset 4 in FileListRow), 0.5 separator right.
    auto column = ui::column({.align = FlexAlign::Stretch, .paddingV = Tokens::spaceP4 * s, .minWidth = kColumnWidth * s,
                              .maxWidth = kColumnWidth * s, .width = kColumnWidth * s});
    grid->setFlexGrow(1.0F);
    column->addChild(std::move(grid));
    m_row->addChild(std::move(column));
    m_row->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator), .width = 0.5F * s}));
  }
  // Mockup .prevc: the rest of the width (at least preview_width), padding 24, gap 12, centred: icon or thumbnail
  // 160, name 600 13, kind and size, Open.
  auto preview = ui::column({.out = &m_preview, .align = FlexAlign::Center, .justify = FlexJustify::Center,
                             .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP24 * s,
                             .minWidth = Tokens::componentFilesPreviewWidth * s, .flexGrow = 1.0F, .visible = false});
  preview->addChild(ui::row({.out = &m_previewArt, .align = FlexAlign::Center, .justify = FlexJustify::Center,
                             .radius = Tokens::radiusR12 * s, .clipChildren = true,
                             .width = Tokens::componentFilesPreviewArt * s, .height = Tokens::componentFilesPreviewArt * s}));
  preview->addChild(ui::label({.out = &m_previewName, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                               .color = colorSpecFromRole(ColorRole::Label1), .maxWidth = Tokens::componentFilesPreviewWidth * s,
                               .maxLines = 2, .textAlign = TextAlign::Center, .ellipsize = TextEllipsize::Middle}));
  preview->addChild(ui::label({.out = &m_previewMeta, .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2,
                               .textAlign = TextAlign::Center}));
  preview->addChild(ui::button({.text = std::string("Open"), .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Secondary,
                                .paddingH = Tokens::spaceP12 * s,
                                .radius = Style::scaledRadius(Tokens::radiusR8, s), .height = Tokens::sizeControlM * s,
                                .onClick = [this] { openPreview(); }}));
  m_row->addChild(std::move(preview));
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileColumns::showPreview(const FileInfo* file) {
  if (m_preview == nullptr) {
    return;
  }
  m_preview->setVisible(file != nullptr);
  m_preview->setParticipatesInLayout(file != nullptr);
  if (file != nullptr) {
    while (!m_previewArt->children().empty()) {
      (void)m_previewArt->removeChild(m_previewArt->children().back().get());
    }
    auto* icon = static_cast<FileIcon*>(
        m_previewArt->addChild(std::make_unique<FileIcon>(Tokens::componentFilesPreviewArt * m_scale)));
    if (m_renderer != nullptr) {
      const FileCellState state = m_host.state(*file, 0);
      icon->bind(*m_renderer, *file, state.path, state.thumbnails);
    }
    m_previewName->setText(file->displayName);
    m_previewMeta->setText(std::format("{} · {}", fileKindLabel(*file), formatFileSize(file->size)));
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileColumns::openPreview() {
  const auto& rows = m_host.rows();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i]->name == m_previewFor) {
      m_host.activate(i);
      return;
    }
  }
}

void FileColumns::setRenderer(Renderer* renderer) {
  for (auto& col : m_columns) {
    col->adapter.renderer = renderer;
  }
  m_renderer = renderer;
}

void FileColumns::refreshCurrent() {
  if (VirtualGridView* grid = currentGrid()) {
    grid->notifyDataChanged();
  }
}

void FileColumns::appendGeometry(std::string& out) const {
  for (const auto& col : m_columns) {
    if (col->grid == nullptr) {
      continue;
    }
    const auto& rows = col->adapter.rows();
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 32); ++i) {
      float x = 0.0F;
      float y = 0.0F;
      if (col->grid->absoluteAnchorForIndex(i, x, y)) {
        out += std::format("column-item {} {} {}\n", rows[i]->displayName, static_cast<int>(x), static_cast<int>(y));
      }
    }
  }
}

VirtualGridView* FileColumns::currentGrid() const noexcept {
  for (const auto& col : m_columns) {
    if (col->current) {
      return col->grid;
    }
  }
  return nullptr;
}

void FileColumns::scrollToEnd() {
  m_scrollPending = true; // the row has its new width only after the next layout
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileColumns::setViewportHeight(float height) {
  if (m_row != nullptr) {
    m_row->setMinHeight(height);
  }
}

void FileColumns::afterLayout() {
  if (!m_scrollPending || m_scroll == nullptr) {
    return;
  }
  m_scrollPending = false;
  // M37: the new column comes into view on the page spring; a fast key walk retargets.
  const float target = std::max(0.0F, m_row->width() - m_scroll->width());
  const float from = m_scroll->scrollOffset();
  kLog.debug("columns scroll row {} view {} from {} to {}", m_row->width(), m_scroll->width(), from, target);
  if (std::abs(target - from) < 1.0F) {
    return;
  }
  MotionTraceLabel trace("files.columns");
  m_scrollAnim = m_animations.drive(
      m_scrollAnim, from, target, Style::spring(Tokens::springPage),
      [this](float v) {
        if (m_scroll != nullptr) {
          m_scroll->setScrollOffset(v);
        }
      },
      [this] { m_scrollAnim = 0; }, m_scroll);
}

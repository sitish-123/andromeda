#include "ui/controls/virtual_grid_view.h"

#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>

// Internal canvas that reports a virtual size set externally and never moves
// its children during its own layout pass — VirtualGridView positions pool
// tiles and the overlay InputArea explicitly.
class VirtualGridView::Canvas : public Node {
public:
  Canvas() = default;

  void setVirtualSize(float width, float height) {
    m_virtualWidth = std::max(0.0F, width);
    m_virtualHeight = std::max(0.0F, height);
    setSize(m_virtualWidth, m_virtualHeight);
  }

protected:
  LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
    return constraints.constrain(LayoutSize{.width = m_virtualWidth, .height = m_virtualHeight});
  }

  void doLayout(Renderer& /*renderer*/) override {
    // Tiles and the input overlay are positioned by VirtualGridView::doLayout.
  }

  void doArrange(Renderer& /*renderer*/, const LayoutRect& rect) override {
    setPosition(rect.x, rect.y);
    setSize(m_virtualWidth, m_virtualHeight);
  }

private:
  float m_virtualWidth = 0.0F;
  float m_virtualHeight = 0.0F;
};

VirtualGridView::VirtualGridView() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(0.0F);
  setPadding(0.0F);
  setFillWidth(true);
  setFillHeight(true);

  auto scroll = std::make_unique<ScrollView>();
  scroll->setFlexGrow(1.0F);
  scroll->setViewportPaddingH(0.0F);
  scroll->setViewportPaddingV(0.0F);
  scroll->setOnScrollChanged([this](float offset) { onScrollChanged(offset); });
  m_scroll = static_cast<ScrollView*>(addChild(std::move(scroll)));

  auto canvas = std::make_unique<Canvas>();
  m_canvas = static_cast<Canvas*>(m_scroll->content()->addChild(std::move(canvas)));

  auto inputArea = std::make_unique<InputArea>();
  inputArea->setZIndex(50);
  inputArea->setFocusable(true);
  inputArea->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  inputArea->setOnEnter([this](const InputArea::PointerData& data) { onPointerEnter(data.localX, data.localY); });
  inputArea->setOnMotion([this](const InputArea::PointerData& data) { onPointerMotion(data.localX, data.localY); });
  inputArea->setOnLeave([this] { onPointerLeave(); });
  inputArea->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      if (data.pressed) {
        onPointerPress(data.localX, data.localY);
      } else {
        onPointerRelease(data.localX, data.localY);
      }
    } else if (data.pressed && data.button == BTN_RIGHT) {
      onSecondaryPointerPress(data.localX, data.localY);
    }
  });
  inputArea->setOnCancel([this] {
    if (m_swiping) {
      m_scroll->dragEnd();
    }
    m_swipePress = false;
    m_swiping = false;
    if (m_adapterPointerCapture && m_adapter != nullptr) {
      m_adapter->onPointerCancel();
    }
    m_adapterPointerCapture = false;
  });
  m_inputArea = static_cast<InputArea*>(m_canvas->addChild(std::move(inputArea)));
}

void VirtualGridView::bindScrollState(ScrollViewState* state) { m_scroll->bindState(state); }

void VirtualGridView::setAdapter(VirtualGridAdapter* adapter) {
  if (m_adapter == adapter) {
    return;
  }
  if (m_adapterPointerCapture && m_adapter != nullptr) {
    m_adapter->onPointerCancel();
  }
  m_adapter = adapter;
  m_adapterPointerCapture = false;
  // Drop the existing pool — tiles were built by the previous adapter's createTile().
  for (Node* tile : m_pool) {
    if (tile != nullptr) {
      m_canvas->removeChild(tile);
    }
  }
  for (InputArea* area : m_poolTooltipAreas) {
    if (area != nullptr) {
      m_inputArea->removeChild(area);
    }
  }
  m_poolTooltipAreas.clear();
  m_pool.clear();
  m_slotBoundIndex.clear();
  m_selectedIndex.reset();
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::notifyDataChanged() {
  // Force every visible slot to rebind on the next layout.
  std::ranges::fill(m_slotBoundIndex, std::nullopt);
  markLayoutDirty();
}

void VirtualGridView::notifyItemChanged(std::size_t index) {
  for (auto& boundIndex : m_slotBoundIndex) {
    if (boundIndex.has_value() && *boundIndex == index) {
      boundIndex.reset();
      markLayoutDirty();
      return;
    }
  }
}

void VirtualGridView::setColumns(std::size_t columns) {
  if (m_columns == columns) {
    return;
  }
  m_columns = columns;
  notifyDataChanged();
}

void VirtualGridView::setMinCellWidth(float width) {
  m_minCellWidth = std::max(1.0F, width);
  notifyDataChanged();
}

void VirtualGridView::setCellHeight(float height) {
  const float next = std::max(1.0F, height);
  if (std::abs(m_cellHeight - next) < 0.001F) {
    return;
  }
  m_cellHeight = next;
  notifyDataChanged();
}

void VirtualGridView::setSquareCells(bool square) {
  if (m_squareCells == square) {
    return;
  }
  m_squareCells = square;
  notifyDataChanged();
}

void VirtualGridView::setColumnGap(float gap) {
  m_columnGap = std::max(0.0F, gap);
  notifyDataChanged();
}

void VirtualGridView::setRowGap(float gap) {
  m_rowGap = std::max(0.0F, gap);
  notifyDataChanged();
}

void VirtualGridView::setOverscanRows(std::size_t rows) {
  m_overscanRows = rows;
  markLayoutDirty();
}

void VirtualGridView::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  if (m_scroll != nullptr) {
    m_scroll->setContentScale(m_scale);
  }
}

void VirtualGridView::setItemCursorShape(std::uint32_t shape) {
  if (m_itemCursorShape == shape) {
    return;
  }
  m_itemCursorShape = shape;
  for (InputArea* area : m_poolTooltipAreas) {
    if (area != nullptr) {
      area->setCursorShape(shape);
    }
  }
}

void VirtualGridView::scrollToIndex(std::size_t index) {
  m_pendingScrollToIndex = true;
  m_pendingScrollIndex = index;
  markLayoutDirty();
}

void VirtualGridView::setSelectedIndex(std::optional<std::size_t> index, bool scrollToSelection) {
  if (!scrollToSelection) {
    m_pendingScrollToIndex = false;
  }
  if (m_selectedIndex == index) {
    if (scrollToSelection && m_selectedIndex.has_value()) {
      m_pendingScrollToIndex = true;
      m_pendingScrollIndex = *m_selectedIndex;
      markLayoutDirty();
    }
    return;
  }
  m_selectedIndex = index;
  if (scrollToSelection && m_selectedIndex.has_value()) {
    m_pendingScrollToIndex = true;
    m_pendingScrollIndex = *m_selectedIndex;
  } else {
    m_pendingScrollToIndex = false;
    m_pendingScrollIndex = 0;
  }
  markLayoutDirty();
  if (m_onSelectionChanged) {
    m_onSelectionChanged(m_selectedIndex);
  }
}

void VirtualGridView::setOnSelectionChanged(std::function<void(std::optional<std::size_t>)> callback) {
  m_onSelectionChanged = std::move(callback);
}

void VirtualGridView::setPaged(std::size_t rows, float pageGap) {
  pageGap = std::max(0.0F, pageGap);
  if (m_pageRows == rows && m_pageGap == pageGap) {
    return;
  }
  m_pageRows = rows;
  m_pageGap = pageGap;
  m_reportedPage.reset();
  m_reportedPageCount = 0;
  m_scroll->setOrientation(rows > 0 ? ScrollOrientation::Horizontal : ScrollOrientation::Vertical);
  m_scroll->setScrollbarVisible(rows == 0);
  if (rows == 0) {
    m_scroll->setPaging(0.0F);
    m_pageStride = 0.0F;
    m_pageCount = 1;
  }
  std::ranges::fill(m_slotBoundIndex, std::nullopt);
  markLayoutDirty();
}

std::size_t VirtualGridView::currentPage() const noexcept {
  if (m_pageRows == 0 || m_pageStride <= 0.0F) {
    return 0;
  }
  const auto page = static_cast<std::size_t>(std::lround(std::max(m_scroll->scrollOffset(), 0.0F) / m_pageStride));
  return std::min(page, m_pageCount - 1);
}

void VirtualGridView::scrollToPage(std::size_t page) {
  m_pendingPage = page;
  markLayoutDirty();
}

void VirtualGridView::setOnPageChanged(std::function<void(std::size_t, std::size_t)> callback) {
  m_onPageChanged = std::move(callback);
}

void VirtualGridView::notifyPageChanged() {
  if (m_pageRows == 0 || !m_onPageChanged) {
    return;
  }
  const std::size_t page = currentPage();
  if (m_reportedPage == page && m_reportedPageCount == m_pageCount) {
    return;
  }
  m_reportedPage = page;
  m_reportedPageCount = m_pageCount;
  m_onPageChanged(page, m_pageCount);
}

void VirtualGridView::cellOrigin(std::size_t index, float& x, float& y) const noexcept {
  const std::size_t col = index % m_layoutColumns;
  std::size_t row = index / m_layoutColumns;
  x = static_cast<float>(visualCol(col)) * (m_cellWidth + m_columnGap);
  if (m_pageRows > 0) {
    x += static_cast<float>(row / m_pageRows) * m_pageStride;
    row %= m_pageRows;
  }
  y = static_cast<float>(row) * (m_cellHeightResolved + m_rowGap);
}

std::size_t VirtualGridView::pageItemStride() const noexcept {
  if (m_pageRows > 0) {
    return std::max<std::size_t>(1, m_layoutColumns * m_pageRows);
  }
  if (m_scroll == nullptr || m_layoutColumns == 0 || m_cellHeightResolved <= 0.0F) {
    return 1;
  }
  const float viewportH = m_scroll->contentViewportHeight();
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (rowStride <= 0.0F) {
    return m_layoutColumns;
  }
  const auto visibleRows = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(viewportH / rowStride)));
  return std::max<std::size_t>(1, visibleRows * m_layoutColumns);
}

void VirtualGridView::doLayout(Renderer& renderer) {
  if (m_adapter == nullptr || m_scroll == nullptr || m_canvas == nullptr || m_inputArea == nullptr) {
    Flex::doLayout(renderer);
    return;
  }

  // Step 1: estimate the canvas's virtual size so ScrollView can measure
  // content height correctly. Match ScrollView's configured gutter behavior.
  const float ourW = std::max(0.0F, width());
  const float ourH = std::max(0.0F, height());
  const float padH = m_scroll->viewportPaddingH();
  const float padV = m_scroll->viewportPaddingV();
  const float innerW = std::max(0.0F, ourW - 2.0F * padH);
  const float viewportH = std::max(0.0F, ourH - 2.0F * padV);
  const float scrollbarGutter = m_scroll->scrollbarGutter();

  m_itemCount = m_adapter->itemCount();

  struct GridMetrics {
    std::size_t columns = 1;
    float cellW = 0.0F;
    float cellH = 0.0F;
    std::size_t rowCount = 0;
    float virtualHeight = 0.0F;
  };

  auto resolveMetrics = [this](float availableW) {
    GridMetrics metrics;
    metrics.columns = m_columns > 0
        ? std::max<std::size_t>(1, m_columns)
        : std::max<std::size_t>(
              1,
              static_cast<std::size_t>(
                  std::floor((availableW + m_columnGap) / std::max(1.0F, m_minCellWidth + m_columnGap))
              )
          );
    const auto columnsF = static_cast<float>(metrics.columns);
    metrics.cellW = std::max(0.0F, (availableW - (columnsF - 1.0F) * m_columnGap) / std::max(1.0F, columnsF));
    metrics.cellH = m_squareCells ? metrics.cellW : m_cellHeight;
    metrics.rowCount = (m_itemCount + metrics.columns - 1) / metrics.columns;
    metrics.virtualHeight = metrics.rowCount == 0
        ? 0.0F
        : (static_cast<float>(metrics.rowCount) * metrics.cellH + static_cast<float>(metrics.rowCount - 1) * m_rowGap);
    return metrics;
  };

  const bool paged = m_pageRows > 0;
  float viewportW = innerW;
  GridMetrics metrics = resolveMetrics(viewportW);
  if (!paged && (m_scroll->reservesScrollbarGutter() || metrics.virtualHeight > viewportH + 0.5F)) {
    viewportW = std::max(0.0F, innerW - scrollbarGutter);
    metrics = resolveMetrics(viewportW);
  }

  const std::size_t previousLayoutColumns = m_layoutColumns;
  const std::size_t columns = metrics.columns;
  const float cellW = metrics.cellW;
  const float cellH = metrics.cellH;
  const std::size_t rowCount = metrics.rowCount;
  float virtualHeight = metrics.virtualHeight;
  float virtualWidth = viewportW;
  if (paged) {
    const std::size_t perPage = columns * m_pageRows;
    m_pageCount = std::max<std::size_t>(1, (m_itemCount + perPage - 1) / perPage);
    m_pageStride = viewportW + m_pageGap;
    const auto pagesF = static_cast<float>(m_pageCount);
    virtualWidth = pagesF * viewportW + (pagesF - 1.0F) * m_pageGap;
    const auto rowsF = static_cast<float>(m_pageRows);
    virtualHeight = rowsF * cellH + (rowsF - 1.0F) * m_rowGap;
    m_scroll->setPaging(m_pageStride);
  }

  m_layoutColumns = columns;
  m_cellWidth = cellW;
  m_cellHeightResolved = cellH;
  m_virtualWidth = virtualWidth;
  m_virtualHeight = virtualHeight;

  m_canvas->setVirtualSize(virtualWidth, virtualHeight);

  // Apply any pending scrollToIndex now that we know cell geometry. Keep the
  // current viewport stable when the target row is already fully visible; this
  // makes keyboard navigation scroll only at the viewport edges instead of
  // pinning each selected row to the top.
  if (m_pendingScrollToIndex) {
    m_pendingScrollToIndex = false;
    if (paged) {
      if (m_pendingScrollIndex < m_itemCount && m_pendingScrollIndex / (columns * m_pageRows) != currentPage()) {
        m_pendingPage = m_pendingScrollIndex / (columns * m_pageRows);
      }
    } else if (m_pendingScrollIndex < m_itemCount && columns > 0) {
      const std::size_t row = m_pendingScrollIndex / columns;
      const float rowTop = static_cast<float>(row) * (cellH + m_rowGap);
      const float rowBottom = rowTop + cellH;
      const float visibleTop = m_scroll->scrollOffset();
      const float visibleBottom = visibleTop + viewportH;
      if (rowTop < visibleTop) {
        m_scroll->requestScrollToOffset(rowTop);
      } else if (rowBottom > visibleBottom) {
        m_scroll->requestScrollToOffset(rowBottom - viewportH);
      }
    }
  }

  // Resolve pending jumps and clamp the offset against the new canvas before binding tiles.
  Flex::doLayout(renderer);
  if (m_pendingPage.has_value()) {
    if (paged) {
      m_scroll->scrollToPage(std::min(*m_pendingPage, m_pageCount - 1));
    }
    m_pendingPage.reset();
  }

  // Step 2: determine the visible row range from the resolved scroll offset, with overscan.
  const float rowStride = cellH + m_rowGap;
  const float scrollY = m_scroll->scrollOffset();
  std::size_t firstRow = 0;
  std::size_t lastRow = 0;
  if (paged && m_pageStride > 0.0F && rowCount > 0) {
    // Whole pages that intersect the viewport: one at rest, two while sliding.
    const float offset = std::max(0.0F, scrollY);
    const auto firstPage = static_cast<std::size_t>(std::floor(offset / m_pageStride));
    const auto lastPage = static_cast<std::size_t>(std::floor((offset + std::max(0.0F, viewportW - 1.0F)) / m_pageStride));
    firstRow = std::min(firstPage * m_pageRows, rowCount - 1);
    lastRow = std::min((lastPage + 1) * m_pageRows - 1, rowCount - 1);
  } else if (!paged && rowStride > 0.0F && rowCount > 0) {
    const long firstRaw = static_cast<long>(std::floor(scrollY / rowStride)) - static_cast<long>(m_overscanRows);
    const long lastRaw =
        static_cast<long>(std::ceil((scrollY + viewportH) / rowStride)) + static_cast<long>(m_overscanRows);
    firstRow = static_cast<std::size_t>(std::max<long>(0, firstRaw));
    lastRow = static_cast<std::size_t>(std::max<long>(0, std::min<long>(lastRaw, static_cast<long>(rowCount) - 1)));
  }
  const std::size_t visibleRowCount = (rowCount == 0 || lastRow < firstRow) ? 0 : (lastRow - firstRow + 1);
  const std::size_t desiredPoolSize = visibleRowCount * columns;

  // Step 3: grow the pool if needed. Pool is row-major (poolRows * columns)
  // so we can address slots by `row % poolRows` and keep tiles bound to the
  // same logical index while they remain in the visible window — avoids the
  // "every scroll rebinds every slot" thrash that would otherwise release
  // expensive per-tile state (e.g. wallpaper thumbnails).
  if (previousLayoutColumns != columns) {
    // Column count changed (resize): existing slot→logicalIndex mapping is stale.
    std::ranges::fill(m_slotBoundIndex, std::nullopt);
  }
  while (m_pool.size() < desiredPoolSize) {
    const std::size_t slot = m_pool.size();
    auto tile = m_adapter->createTile();
    if (tile == nullptr) {
      break;
    }
    tile->setVisible(false);
    tile->setParticipatesInLayout(false);
    Node* raw = m_canvas->addChild(std::move(tile));
    m_pool.push_back(raw);
    m_slotBoundIndex.emplace_back();
    m_slotBoundSelected.push_back(false);
    m_slotBoundHovered.push_back(false);
    m_slotBoundOverlayHovered.push_back(false);

    // Per-cell overlay: carries the item tooltip and the item cursor shape.
    auto tooltipArea = std::make_unique<InputArea>();
    tooltipArea->setVisible(false);
    tooltipArea->setAcceptedButtons(0);
    tooltipArea->setCursorShape(m_itemCursorShape);
    tooltipArea->setOnEnter([this, slot](const InputArea::PointerData& data) {
      onPoolTooltipMotion(slot, data.localX, data.localY);
    });
    tooltipArea->setOnMotion([this, slot](const InputArea::PointerData& data) {
      onPoolTooltipMotion(slot, data.localX, data.localY);
    });
    tooltipArea->setOnLeave([this, slot] { onPoolTooltipLeave(slot); });
    tooltipArea->setTooltipProvider([this, slot] -> TooltipContent {
      if (m_tooltipSuppressed
          || m_adapter == nullptr
          || slot >= m_slotBoundIndex.size()
          || !m_slotBoundIndex[slot].has_value()) {
        return {};
      }
      const std::size_t index = *m_slotBoundIndex[slot];
      std::string tooltip = m_slotBoundOverlayHovered[slot] ? m_adapter->overlayTooltip(index) : std::string{};
      if (tooltip.empty()) {
        tooltip = m_adapter->itemTooltip(index);
      }
      return tooltip.empty() ? TooltipContent{} : TooltipContent{std::move(tooltip)};
    });
    m_poolTooltipAreas.push_back(static_cast<InputArea*>(m_inputArea->addChild(std::move(tooltipArea))));
  }

  // Step 4: bind / position pool slots, addressing by `row % poolRows`.
  const std::size_t poolRows = columns == 0 ? 0 : m_pool.size() / columns;
  m_visibleStartIndex = firstRow * columns;
  std::vector<bool> slotActive(m_pool.size(), false);

  if (poolRows > 0 && visibleRowCount > 0) {
    for (std::size_t row = firstRow; row <= lastRow; ++row) {
      const std::size_t poolRow = row % poolRows;
      for (std::size_t col = 0; col < columns; ++col) {
        const std::size_t slot = poolRow * columns + col;
        if (slot >= m_pool.size()) {
          continue;
        }
        const std::size_t logicalIndex = row * columns + col;
        if (logicalIndex >= m_itemCount) {
          continue;
        }
        Node* tile = m_pool[slot];
        if (tile == nullptr) {
          continue;
        }

        slotActive[slot] = true;

        float x = 0.0F;
        float y = 0.0F;
        cellOrigin(logicalIndex, x, y);
        tile->setPosition(x, y);
        tile->setSize(cellW, cellH);

        InputArea* tooltipArea = m_poolTooltipAreas[slot];
        tooltipArea->setPosition(x, y);
        tooltipArea->setFrameSize(cellW, cellH);
        tooltipArea->setVisible(true);
        const bool selected = m_selectedIndex.has_value() && *m_selectedIndex == logicalIndex;
        const bool hovered = m_hoveredIndex.has_value() && *m_hoveredIndex == logicalIndex;
        const bool overlayHovered = m_hoveredOverlayIndex.has_value() && *m_hoveredOverlayIndex == logicalIndex;
        updateTooltipAnchor(slot, logicalIndex, overlayHovered);
        const bool dirty = !m_slotBoundIndex[slot].has_value()
            || *m_slotBoundIndex[slot] != logicalIndex
            || m_slotBoundSelected[slot] != selected
            || m_slotBoundHovered[slot] != hovered;
        if (dirty) {
          m_adapter->bindTile(*tile, logicalIndex, selected, hovered);
          m_slotBoundIndex[slot] = logicalIndex;
          m_slotBoundSelected[slot] = selected;
          m_slotBoundHovered[slot] = hovered;
        }
        if (m_slotBoundOverlayHovered[slot] != overlayHovered) {
          m_adapter->applyOverlayHover(*tile, overlayHovered);
          m_slotBoundOverlayHovered[slot] = overlayHovered;
        }
        tile->setVisible(true);
        // Canvas's doLayout is a no-op, so we lay out each pool tile explicitly
        // here — otherwise the inner glyph/label children never get measured.
        tile->layout(renderer);
      }
    }
  }

  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!slotActive[slot] && m_pool[slot] != nullptr) {
      m_pool[slot]->setVisible(false);
      m_slotBoundIndex[slot].reset();
      if (slot < m_poolTooltipAreas.size() && m_poolTooltipAreas[slot] != nullptr) {
        m_poolTooltipAreas[slot]->setVisible(false);
      }
      m_slotBoundOverlayHovered[slot] = false;
    }
  }

  // Step 5: position the input overlay across the entire virtual canvas.
  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(virtualWidth, virtualHeight);
  notifyPageChanged();
}

LayoutSize VirtualGridView::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  // Pure size report: never run doLayout from measure. The parent's measure pass
  // happens with intermediate constraints (often width=0 for a fillWidth+flexGrow
  // child); binding tiles at those phantom widths would then mark layout dirty and
  // schedule another full root re-layout, which manifests as ~6px wobble on
  // sibling labels (header titles, columns) every time the cursor moves anywhere
  // in the dialog. Tile binding belongs in the arrange pass where the final rect
  // is known.
  const float w = constraints.hasExactWidth() ? constraints.maxWidth
      : constraints.hasMaxWidth               ? constraints.maxWidth
                                              : 0.0F;
  const float h = constraints.hasExactHeight() ? constraints.maxHeight
      : constraints.hasMaxHeight               ? constraints.maxHeight
                                               : 0.0F;
  return LayoutSize{.width = w, .height = h};
}

void VirtualGridView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void VirtualGridView::onScrollChanged(float /*offset*/) {
  if (m_adapterPointerCapture && m_adapter != nullptr) {
    m_adapter->onPointerCancel();
    m_adapterPointerCapture = false;
  }
  if (m_hoveredOverlayIndex.has_value()) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
  notifyPageChanged();
}

void VirtualGridView::onPointerEnter(float localX, float localY) { onPointerMotion(localX, localY); }

void VirtualGridView::onPointerMotion(float localX, float localY) {
  if (m_swipePress) {
    // Viewport coordinates: the canvas moves under the pointer while a page spring runs.
    if (!m_swiping && std::abs(localX - m_scroll->scrollOffset() - m_swipePressViewX) >= Style::dragStartThreshold * m_scale) {
      m_swiping = true;
      m_scroll->dragBegin(m_swipePressViewX);
    }
    if (m_swiping) {
      m_scroll->dragMove(localX - m_scroll->scrollOffset());
      return;
    }
  }
  const auto idx = indexAt(localX, localY);

  if (m_adapterPointerCapture && m_adapter != nullptr) {
    // A held press only becomes a drag once the pointer has travelled the same
    // distance the rest of the shell requires. Pointers emit motion between
    // press and release (and enter is replayed as motion after a scene-root
    // swap), so without this every click would reach the adapter as a
    // zero-distance drag and never as a click.
    if (!m_dragThresholdPassed) {
      if (std::hypot(localX - m_pressLocalX, localY - m_pressLocalY) < Style::dragStartThreshold * m_scale) {
        return;
      }
      m_dragThresholdPassed = true;
    }
    if (m_adapter->onPointerDrag(idx, localX, localY, m_cellWidth, m_cellHeightResolved)) {
      notifyDataChanged();
    }
    return;
  }

  std::optional<std::size_t> overlayIdx;
  if (idx.has_value() && m_adapter != nullptr) {
    float cellLocalX = 0.0F;
    float cellLocalY = 0.0F;
    cellLocalAt(localX, localY, *idx, cellLocalX, cellLocalY);
    if (m_adapter->overlayHitTest(*idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
      overlayIdx = idx;
    }
  }

  if (idx == m_hoveredIndex && overlayIdx == m_hoveredOverlayIndex) {
    return;
  }

  m_tooltipSuppressed = false;
  if (m_hoveredOverlayIndex.has_value() && m_hoveredOverlayIndex != overlayIdx) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  if (overlayIdx.has_value() && overlayIdx != m_hoveredOverlayIndex) {
    setOverlayHoveredForIndex(*overlayIdx, true);
  }

  m_hoveredIndex = idx;
  m_hoveredOverlayIndex = overlayIdx;
  markLayoutDirty();
}

void VirtualGridView::onPointerLeave() {
  if (!m_hoveredIndex.has_value() && !m_hoveredOverlayIndex.has_value()) {
    return;
  }
  m_tooltipSuppressed = false;
  if (m_hoveredOverlayIndex.has_value()) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::onPoolTooltipMotion(std::size_t slot, float localX, float localY) {
  if (slot >= m_slotBoundIndex.size() || !m_slotBoundIndex[slot].has_value() || m_layoutColumns == 0) {
    return;
  }
  float x = 0.0F;
  float y = 0.0F;
  cellOrigin(*m_slotBoundIndex[slot], x, y);
  onPointerMotion(x + localX, y + localY);
}

void VirtualGridView::onPoolTooltipLeave(std::size_t slot) {
  if (slot >= m_slotBoundIndex.size() || (m_hoveredIndex.has_value() && m_slotBoundIndex[slot] == m_hoveredIndex)) {
    onPointerLeave();
  }
}

void VirtualGridView::onPointerPress(float localX, float localY) {
  const auto idx = indexAt(localX, localY);
  if (m_pageRows > 0) {
    m_swipePress = true;
    m_swiping = false;
    m_swipePressIndex = idx;
    m_swipePressViewX = localX - m_scroll->scrollOffset();
  }
  if (!idx.has_value()) {
    return;
  }
  setSelectedIndex(idx, false);
  if (m_adapter == nullptr) {
    return;
  }

  float cellLocalX = 0.0F;
  float cellLocalY = 0.0F;
  cellLocalAt(localX, localY, *idx, cellLocalX, cellLocalY);
  if (m_adapter->onPointerPress(*idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
    m_swipePress = false;
    m_adapterPointerCapture = true;
    m_pressLocalX = localX;
    m_pressLocalY = localY;
    m_dragThresholdPassed = false;
    m_tooltipSuppressed = true;
    for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
      if (m_slotBoundIndex[slot] == idx) {
        m_poolTooltipAreas[slot]->requestTooltipRefresh();
      }
    }
    return;
  }
  if (m_swipePress) {
    return; // activates on release unless the press becomes a swipe
  }
  m_adapter->onActivate(*idx);
}

void VirtualGridView::onPointerRelease(float localX, float localY) {
  if (m_swipePress) {
    m_swipePress = false;
    if (m_swiping) {
      m_swiping = false;
      m_scroll->dragEnd();
    } else if (m_adapter != nullptr && m_swipePressIndex.has_value() && indexAt(localX, localY) == m_swipePressIndex) {
      m_adapter->onActivate(*m_swipePressIndex);
    }
    return;
  }
  if (!m_adapterPointerCapture || m_adapter == nullptr) {
    return;
  }
  m_adapterPointerCapture = false;
  const auto result = m_adapter->onPointerRelease(indexAt(localX, localY));
  if (result.rebind) {
    notifyDataChanged();
  }
  if (result.activateIndex.has_value()) {
    m_adapter->onActivate(*result.activateIndex);
  }
}

void VirtualGridView::onSecondaryPointerPress(float localX, float localY) {
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    return;
  }
  setSelectedIndex(idx, false);
  if (m_adapter != nullptr) {
    float wx = 0.0F;
    float wy = 0.0F;
    Node::absolutePosition(m_inputArea, wx, wy);
    wx += localX;
    wy += localY;
    m_adapter->onSecondaryActivate(*idx, wx, wy);
  }
}

Node* VirtualGridView::nodeForIndex(std::size_t index) const noexcept {
  for (std::size_t slot = 0; slot < m_pool.size() && slot < m_slotBoundIndex.size(); ++slot) {
    if (m_slotBoundIndex[slot] == index) {
      return m_pool[slot];
    }
  }
  return nullptr;
}

bool VirtualGridView::absoluteAnchorForIndex(std::size_t index, float& outX, float& outY) const noexcept {
  if (m_inputArea == nullptr
      || m_layoutColumns == 0
      || m_cellWidth <= 0.0F
      || m_cellHeightResolved <= 0.0F
      || index >= m_itemCount) {
    return false;
  }

  // Prefer a live pool tile when the item is currently materialized.
  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!m_slotBoundIndex[slot].has_value() || *m_slotBoundIndex[slot] != index || m_pool[slot] == nullptr) {
      continue;
    }
    Node::absolutePosition(m_pool[slot], outX, outY);
    outX += m_pool[slot]->width() * 0.5F;
    outY += m_pool[slot]->height() * 0.5F;
    return true;
  }

  float x = 0.0F;
  float y = 0.0F;
  cellOrigin(index, x, y);
  float wx = 0.0F;
  float wy = 0.0F;
  Node::absolutePosition(m_inputArea, wx, wy);
  outX = wx + x + m_cellWidth * 0.5F;
  outY = wy + y + m_cellHeightResolved * 0.5F;
  return true;
}

std::optional<std::size_t> VirtualGridView::indexAt(float localX, float localY) const noexcept {
  if (m_layoutColumns == 0 || m_cellWidth <= 0.0F || m_cellHeightResolved <= 0.0F || m_itemCount == 0) {
    return std::nullopt;
  }
  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (localX < 0.0F || localY < 0.0F || colStride <= 0.0F || rowStride <= 0.0F) {
    return std::nullopt;
  }
  std::size_t pageRowBase = 0;
  if (m_pageRows > 0 && m_pageStride > 0.0F) {
    const auto page = static_cast<std::size_t>(std::floor(localX / m_pageStride));
    localX -= static_cast<float>(page) * m_pageStride;
    if (localY >= static_cast<float>(m_pageRows) * rowStride) {
      return std::nullopt;
    }
    pageRowBase = page * m_pageRows;
  }
  const auto colF = localX / colStride;
  const auto rowF = localY / rowStride;
  const auto col = static_cast<std::size_t>(std::floor(colF));
  const auto row = static_cast<std::size_t>(std::floor(rowF));
  if (col >= m_layoutColumns) {
    return std::nullopt;
  }
  // Reject the gutter region between cells.
  const float cellLocalX = localX - static_cast<float>(col) * colStride;
  const float cellLocalY = localY - static_cast<float>(row) * rowStride;
  if (cellLocalX > m_cellWidth || cellLocalY > m_cellHeightResolved) {
    return std::nullopt;
  }
  const std::size_t idx = (pageRowBase + row) * m_layoutColumns + visualCol(col);
  if (idx >= m_itemCount) {
    return std::nullopt;
  }
  return idx;
}

void VirtualGridView::cellLocalAt(
    float localX, float localY, std::size_t index, float& cellLocalX, float& cellLocalY
) const noexcept {
  float x = 0.0F;
  float y = 0.0F;
  cellOrigin(index, x, y);
  cellLocalX = localX - x;
  cellLocalY = localY - y;
}

void VirtualGridView::setOverlayHoveredForIndex(std::size_t index, bool hovered) {
  if (m_adapter == nullptr) {
    return;
  }
  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!m_slotBoundIndex[slot].has_value() || *m_slotBoundIndex[slot] != index) {
      continue;
    }
    if (m_slotBoundOverlayHovered[slot] == hovered) {
      return;
    }
    m_adapter->applyOverlayHover(*m_pool[slot], hovered);
    m_slotBoundOverlayHovered[slot] = hovered;
    updateTooltipAnchor(slot, index, hovered);
    m_poolTooltipAreas[slot]->requestTooltipRefresh();
    markPaintDirty();
    return;
  }
}

void VirtualGridView::updateTooltipAnchor(std::size_t slot, std::size_t index, bool overlayHovered) {
  InputArea* area = m_poolTooltipAreas[slot];
  const auto insets = m_adapter->itemTooltipAnchorInsets(index, area->width(), area->height(), overlayHovered);
  if (insets.has_value()) {
    area->setTooltipAnchorInsets(*insets);
  } else {
    area->clearTooltipAnchorInsets();
  }
}

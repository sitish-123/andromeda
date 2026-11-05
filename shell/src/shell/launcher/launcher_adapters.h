#pragma once

#include "shell/launcher/launcher_panel_detail.h"

using namespace launcher_detail;

class LauncherResultAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherResultAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherResultRow>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* row = static_cast<LauncherResultRow*>(&tile);
    row->setListStyle(m_style);
    row->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    row->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets> itemTooltipAnchorInsets(
      std::size_t index, float cellWidth, float cellHeight, bool /*overlayHovered*/
  ) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float pinnedWidth = (*m_results)[index].pinned ? glyphSize + gap : 0.0F;
    const float left = std::max(padding, cellWidth - padding - pinnedWidth - glyphSize);
    const float top = std::max(0.0F, (cellHeight - glyphSize) * 0.5F);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;

    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  PointerReleaseResult onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value();
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    }
    return {
        .rebind = sourceIndex.has_value() || targetIndex.has_value(),
        .activateIndex = activated ? sourceIndex : std::nullopt,
    };
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherAppGridAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherAppGridAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherAppGridTile>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* gridTile = static_cast<LauncherAppGridTile*>(&tile);
    gridTile->setListStyle(m_style);
    gridTile->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    gridTile->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets> itemTooltipAnchorInsets(
      std::size_t index, float cellWidth, float cellHeight, bool /*overlayHovered*/
  ) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float left = std::max(0.0F, cellWidth - padding - glyphSize);
    const float top = std::max(0.0F, cellHeight - padding - glyphSize);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;
    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  PointerReleaseResult onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value();
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    }
    return {
        .rebind = sourceIndex.has_value() || targetIndex.has_value(),
        .activateIndex = activated ? sourceIndex : std::nullopt,
    };
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherSectionsAdapter final : public VirtualListAdapter {
public:
  using SelectCallback = std::function<void(std::size_t)>;
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

  LauncherSectionsAdapter(
      LauncherListStyle style, AsyncTextureCache* cache, SelectCallback onSelect, ActivateCallback onActivate,
      SecondaryActivateCallback onSecondaryActivate
  )
      : m_style(style), m_cache(cache), m_onSelect(std::move(onSelect)), m_onActivate(std::move(onActivate)),
        m_onSecondaryActivate(std::move(onSecondaryActivate)) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setSelectedResultIndex(std::optional<std::size_t> index) { m_selectedResultIndex = index; }
  [[nodiscard]] bool setGridView(bool gridView) {
    if (m_gridView == gridView) {
      return false;
    }
    m_gridView = gridView;
    return true;
  }
  void setApplicationRowHeight(float height) {
    const float next = std::max(1.0F, height);
    if (std::abs(m_applicationRowHeight - next) < 0.5F) {
      return;
    }
    m_applicationRowHeight = next;
    ++m_revision;
  }

  struct Section {
    std::string label;
    std::string glyphName;
    std::vector<std::size_t> resultIndices;
  };

  void rebuild(const std::vector<LauncherCategory>& categories, std::string otherLabel) {
    std::vector<Section> sections;
    const std::size_t count = m_results == nullptr ? 0U : m_results->size();
    std::vector<bool> assigned(count, false);
    for (const LauncherCategory& category : categories) {
      Section section{.label = category.label, .glyphName = category.glyphName, .resultIndices = {}};
      for (std::size_t i = 0; i < count; ++i) {
        if (!assigned[i] && (*m_results)[i].category == category.label) {
          section.resultIndices.push_back(i);
          assigned[i] = true;
        }
      }
      sections.push_back(std::move(section));
    }
    Section other{.label = std::move(otherLabel), .glyphName = "apps", .resultIndices = {}};
    for (std::size_t i = 0; i < count; ++i) {
      if (!assigned[i]) {
        other.resultIndices.push_back(i);
      }
    }
    sections.push_back(std::move(other));
    rebuildSections(sections);
  }

  void rebuildSections(const std::vector<Section>& sections) {
    m_rows.clear();
    m_visualResultOrder.clear();
    m_resultPositions.assign(m_results == nullptr ? 0U : m_results->size(), std::nullopt);
    ++m_revision;
    if (m_results == nullptr) {
      return;
    }
    const std::size_t columns =
        m_cellGrid != CellGrid::None ? cellColumns(m_cellGrid) : (m_gridView ? kAppGridColumns : 1);
    for (const Section& section : sections) {
      if (section.resultIndices.empty()) {
        continue;
      }
      LauncherSectionRowData header;
      header.kind = LauncherSectionRowData::Kind::Header;
      header.label = section.label;
      header.glyphName = m_pillStyle ? std::string() : section.glyphName;
      m_rows.push_back(std::move(header));

      for (std::size_t offset = 0; offset < section.resultIndices.size(); offset += columns) {
        LauncherSectionRowData row;
        row.kind = LauncherSectionRowData::Kind::Applications;
        row.resultCount = std::min(columns, section.resultIndices.size() - offset);
        const std::size_t rowIndex = m_rows.size();
        for (std::size_t column = 0; column < row.resultCount; ++column) {
          const std::size_t resultIndex = section.resultIndices[offset + column];
          row.resultIndices[column] = resultIndex;
          m_resultPositions[resultIndex] = ResultPosition{
              .row = rowIndex,
              .column = column,
              .visualOrder = m_visualResultOrder.size(),
          };
          m_visualResultOrder.push_back(resultIndex);
        }
        m_rows.push_back(std::move(row));
      }
    }
  }

  void setCellGrid(CellGrid grid) {
    if (m_cellGrid != grid) {
      m_cellGrid = grid;
      ++m_revision;
    }
  }

  // Rulebook 9.14 sections: plain footnote headers h 28, rows h 40.
  void setPillStyle(bool pill) {
    if (m_pillStyle != pill) {
      m_pillStyle = pill;
      ++m_revision;
    }
  }

  // Unscaled height of a result row: a media cell row, a two-line row or a plain row.
  [[nodiscard]] float resultRowHeight(const LauncherSectionRowData& row) const {
    if (m_cellGrid == CellGrid::None && m_results != nullptr && row.resultCount > 0
        && row.resultIndices[0] < m_results->size() && isTwoLineRow((*m_results)[row.resultIndices[0]])) {
      return Tokens::componentLauncherRowTall;
    }
    return cellRowHeight(m_cellGrid);
  }

  // Height of the first `maxRows` result rows and the headers between them.
  [[nodiscard]] float pillContentHeight(std::size_t maxRows) const {
    float height = 0.0F;
    std::size_t rows = 0;
    // Taller media cells show fewer rows in the same space.
    maxRows = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<float>(maxRows) * Tokens::componentLauncherRow / cellRowHeight(m_cellGrid))
    );
    for (const auto& row : m_rows) {
      if (row.kind == LauncherSectionRowData::Kind::Header) {
        height += Tokens::componentLauncherSectionHeader * m_style.scale;
      } else if (rows++ < maxRows) {
        height += resultRowHeight(row) * m_style.scale;
      } else {
        break;
      }
    }
    return height;
  }

  [[nodiscard]] std::optional<std::size_t> rowForHeader(std::string_view label) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
      if (m_rows[i].kind == LauncherSectionRowData::Kind::Header && m_rows[i].label == label) {
        return i;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> firstResultIndex() const {
    return m_visualResultOrder.empty() ? std::nullopt : std::optional<std::size_t>(m_visualResultOrder.front());
  }

  [[nodiscard]] std::optional<std::size_t> rowForResult(std::size_t resultIndex) const {
    if (resultIndex >= m_resultPositions.size() || !m_resultPositions[resultIndex].has_value()) {
      return std::nullopt;
    }
    return m_resultPositions[resultIndex]->row;
  }

  [[nodiscard]] std::optional<std::size_t> moveHorizontal(std::size_t resultIndex, int direction) const {
    if (!m_gridView && m_cellGrid == CellGrid::None) {
      return resultIndex < m_resultPositions.size() && m_resultPositions[resultIndex].has_value()
          ? std::optional<std::size_t>(resultIndex)
          : firstResultIndex();
    }
    if (resultIndex >= m_resultPositions.size()
        || !m_resultPositions[resultIndex].has_value()
        || m_visualResultOrder.empty()) {
      return firstResultIndex();
    }
    const std::size_t order = m_resultPositions[resultIndex]->visualOrder;
    if (direction < 0) {
      return order == 0 ? std::optional<std::size_t>(resultIndex)
                        : std::optional<std::size_t>(m_visualResultOrder[order - 1]);
    }
    return order + 1 >= m_visualResultOrder.size() ? std::optional<std::size_t>(resultIndex)
                                                   : std::optional<std::size_t>(m_visualResultOrder[order + 1]);
  }

  [[nodiscard]] std::optional<std::size_t>
  moveVertical(std::size_t resultIndex, int direction, std::size_t applicationRows = 1) const {
    if (resultIndex >= m_resultPositions.size() || !m_resultPositions[resultIndex].has_value()) {
      return firstResultIndex();
    }
    const ResultPosition position = *m_resultPositions[resultIndex];
    std::size_t rowIndex = position.row;
    std::size_t rowsRemaining = std::max<std::size_t>(1, applicationRows);
    while (true) {
      if (direction < 0) {
        if (rowIndex == 0) {
          return resultIndex;
        }
        --rowIndex;
      } else {
        if (rowIndex + 1 >= m_rows.size()) {
          return resultIndex;
        }
        ++rowIndex;
      }
      const auto& row = m_rows[rowIndex];
      if (row.kind != LauncherSectionRowData::Kind::Applications || row.resultCount == 0) {
        continue;
      }
      --rowsRemaining;
      if (rowsRemaining == 0) {
        return row.resultIndices[std::min(position.column, row.resultCount - 1)];
      }
    }
  }

  [[nodiscard]] std::size_t estimatedVisibleApplicationRows(float viewportHeight) const {
    const float rowHeight = std::max(1.0F, m_applicationRowHeight);
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(viewportHeight / rowHeight)));
  }

  [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
  [[nodiscard]] std::uint64_t itemRevision(std::size_t /*index*/) const override { return m_revision; }
  [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return false; }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    if (index >= m_rows.size()) {
      return 1.0F;
    }
    if (m_pillStyle) {
      return (m_rows[index].kind == LauncherSectionRowData::Kind::Header ? Tokens::componentLauncherSectionHeader
                                                                         : resultRowHeight(m_rows[index]))
          * m_style.scale;
    }
    if (m_rows[index].kind == LauncherSectionRowData::Kind::Header) {
      const float textHeight =
          stableLabelHeight(renderer.measureFont(Style::fontSizeBody * m_style.scale, {FontWeight::SemiBold}));
      return std::ceil(std::max(Style::fontSizeTitle * m_style.scale, textHeight) + Style::spaceSm * m_style.scale);
    }

    (void)width;
    return m_applicationRowHeight;
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<LauncherSectionRow>(m_style, m_cache, m_onSelect, m_onActivate, m_onSecondaryActivate);
  }

  void bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) override {
    if (m_results == nullptr || index >= m_rows.size()) {
      return;
    }
    auto* row = static_cast<LauncherSectionRow*>(&item);
    row->setListStyle(m_style);
    row->setGridView(m_gridView);
    row->setCellGrid(m_cellGrid);
    row->setPillStyle(m_pillStyle);
    row->bind(m_rows[index], *m_results, m_selectedResultIndex, width, item.height());
  }

private:
  struct ResultPosition {
    std::size_t row = 0;
    std::size_t column = 0;
    std::size_t visualOrder = 0;
  };

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  SelectCallback m_onSelect;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  const std::vector<LauncherResult>* m_results = nullptr;
  std::vector<LauncherSectionRowData> m_rows;
  std::vector<std::optional<ResultPosition>> m_resultPositions;
  std::vector<std::size_t> m_visualResultOrder;
  std::optional<std::size_t> m_selectedResultIndex;
  std::uint64_t m_revision = 0;
  float m_applicationRowHeight = 1.0F;
  bool m_gridView = false;
  CellGrid m_cellGrid = CellGrid::None;
  bool m_pillStyle = false;
};

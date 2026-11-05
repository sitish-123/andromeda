#include "document_model.h"

#include <algorithm>
#include <format>

namespace viewer {

  namespace {
    const std::vector<RectF> kNoHits;
  }

  void DocumentModel::reset(std::size_t pageCount, float firstWidthPt, float firstHeightPt) {
    m_sizes.assign(pageCount, Size{});
    if (firstWidthPt > 0.0F && firstHeightPt > 0.0F) {
      m_defaultWidth = firstWidthPt;
      m_defaultHeight = firstHeightPt;
    }
    m_fitWidth = true;
    clearSearch();
  }

  void DocumentModel::setPageSize(std::size_t page, float widthPt, float heightPt) {
    if (page < m_sizes.size() && widthPt > 0.0F && heightPt > 0.0F) {
      m_sizes[page] = {widthPt, heightPt, true};
    }
  }

  void DocumentModel::setViewportWidth(float width) { m_viewportWidth = std::max(0.0F, width); }

  float DocumentModel::zoom() const noexcept {
    if (m_fitWidth && m_viewportWidth > 0.0F) {
      return std::clamp(m_viewportWidth / m_defaultWidth, kMinZoom, kMaxZoom);
    }
    return m_zoom;
  }

  void DocumentModel::zoomBy(float factor) {
    m_zoom = std::clamp(zoom() * factor, kMinZoom, kMaxZoom);
    m_fitWidth = false;
  }

  void DocumentModel::fitWidth() { m_fitWidth = true; }

  float DocumentModel::pageWidthPx(std::size_t page) const {
    return (m_sizes[page].known ? m_sizes[page].width : m_defaultWidth) * zoom();
  }

  float DocumentModel::pageHeightPx(std::size_t page) const {
    return (m_sizes[page].known ? m_sizes[page].height : m_defaultHeight) * zoom();
  }

  float DocumentModel::contentWidth() const {
    float width = m_viewportWidth;
    for (std::size_t i = 0; i < m_sizes.size(); ++i) {
      width = std::max(width, pageWidthPx(i));
    }
    return width;
  }

  RectF DocumentModel::pageBox(std::size_t page) const {
    float y = 0.0F;
    for (std::size_t i = 0; i < page && i < m_sizes.size(); ++i) {
      y += pageHeightPx(i) + kPageGap;
    }
    if (page >= m_sizes.size()) {
      return {0.0F, y, 0.0F, y};
    }
    const float width = pageWidthPx(page);
    const float x = (contentWidth() - width) / 2.0F;
    return {x, y, x + width, y + pageHeightPx(page)};
  }

  float DocumentModel::contentHeight() const {
    float y = 0.0F;
    for (std::size_t i = 0; i < m_sizes.size(); ++i) {
      y += pageHeightPx(i) + (i + 1 < m_sizes.size() ? kPageGap : 0.0F);
    }
    return y;
  }

  std::size_t DocumentModel::pageAt(float y) const {
    float bottom = 0.0F;
    for (std::size_t i = 0; i < m_sizes.size(); ++i) {
      bottom += pageHeightPx(i) + kPageGap;
      if (y < bottom) {
        return i;
      }
    }
    return m_sizes.empty() ? 0 : m_sizes.size() - 1;
  }

  std::pair<std::size_t, std::size_t> DocumentModel::visiblePages(float top, float bottom) const {
    return {pageAt(top), pageAt(bottom)};
  }

  void DocumentModel::startSearch(std::string query) {
    clearSearch();
    m_query = std::move(query);
    m_hits.assign(m_sizes.size(), {});
  }

  void DocumentModel::clearSearch() {
    m_query.clear();
    m_hits.clear();
    m_scanned = 0;
    m_hitCount = 0;
    m_current.reset();
  }

  std::optional<std::size_t> DocumentModel::nextPageToScan() const {
    if (m_query.empty() || m_scanned >= m_sizes.size()) {
      return std::nullopt;
    }
    return m_scanned;
  }

  void DocumentModel::setHits(std::size_t page, std::vector<RectF> hitsPt) {
    // Pages are scanned in order; a late reply for an older search or page does not count.
    if (m_query.empty() || page != m_scanned || page >= m_hits.size()) {
      return;
    }
    m_hitCount += hitsPt.size();
    m_hits[page] = std::move(hitsPt);
    ++m_scanned;
  }

  const std::vector<RectF>& DocumentModel::hits(std::size_t page) const {
    return page < m_hits.size() ? m_hits[page] : kNoHits;
  }

  std::size_t DocumentModel::currentOrdinal() const {
    if (!m_current) {
      return 0;
    }
    std::size_t ordinal = m_current->index + 1;
    for (std::size_t i = 0; i < m_current->page; ++i) {
      ordinal += m_hits[i].size();
    }
    return ordinal;
  }

  bool DocumentModel::nextHit(std::size_t fromPage) {
    if (m_hitCount == 0) {
      return false;
    }
    if (m_current && m_current->index + 1 < m_hits[m_current->page].size()) {
      ++m_current->index;
      return true;
    }
    const std::size_t start = m_current ? m_current->page + 1 : fromPage;
    for (std::size_t step = 0; step < m_scanned; ++step) {
      const std::size_t page = (start + step) % m_scanned;
      if (!m_hits[page].empty()) {
        m_current = HitRef{page, 0};
        return true;
      }
    }
    return false;
  }

  bool DocumentModel::previousHit(std::size_t fromPage) {
    if (m_hitCount == 0) {
      return false;
    }
    if (m_current && m_current->index > 0) {
      --m_current->index;
      return true;
    }
    const std::size_t start = m_current ? m_current->page + m_scanned - 1 : std::min(fromPage, m_scanned - 1);
    for (std::size_t step = 0; step < m_scanned; ++step) {
      const std::size_t page = (start + m_scanned - step) % m_scanned;
      if (!m_hits[page].empty()) {
        m_current = HitRef{page, m_hits[page].size() - 1};
        return true;
      }
    }
    return false;
  }

  std::string DocumentModel::countText() const {
    if (m_query.empty()) {
      return {};
    }
    if (m_hitCount == 0) {
      return searchDone() ? "No results" : "";
    }
    return std::format("{} of {}{}", currentOrdinal(), m_hitCount, searchDone() ? "" : "+");
  }

} // namespace viewer

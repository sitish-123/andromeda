#include "render/core/damage_region.h"

#include <algorithm>
#include <limits>

DamageRect uniteRects(const DamageRect& a, const DamageRect& b) noexcept {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

void DamageRegion::removeAt(std::size_t index) {
  m_rects[index] = m_rects[m_count - 1];
  --m_count;
}

void DamageRegion::add(const DamageRect& rect) {
  if (rect.empty()) {
    return;
  }
  DamageRect merged = rect;
  for (bool changed = true; changed;) {
    changed = false;
    for (std::size_t i = 0; i < m_count; ++i) {
      if (m_rects[i].contains(merged)) {
        return;
      }
      if (m_rects[i].overlaps(merged)) {
        merged = uniteRects(merged, m_rects[i]);
        removeAt(i);
        changed = true;
        break;
      }
    }
  }
  if (m_count < kMaxRects) {
    m_rects[m_count++] = merged;
    return;
  }
  std::size_t best = 0;
  float bestGrowth = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < m_count; ++i) {
    const float growth = uniteRects(m_rects[i], merged).area() - m_rects[i].area();
    if (growth < bestGrowth) {
      bestGrowth = growth;
      best = i;
    }
  }
  const DamageRect grown = uniteRects(m_rects[best], merged);
  removeAt(best);
  add(grown);
}

void DamageRegion::add(const DamageRegion& region) {
  for (const DamageRect& rect : region.rects()) {
    add(rect);
  }
}

void DamageRegion::collapseIfCompact(float maxGrowth) {
  if (m_count < 2) {
    return;
  }
  DamageRect box = m_rects[0];
  float area = 0.0F;
  for (std::size_t i = 0; i < m_count; ++i) {
    box = uniteRects(box, m_rects[i]);
    area += m_rects[i].area();
  }
  if (box.area() <= area * maxGrowth) {
    m_rects[0] = box;
    m_count = 1;
  }
}

void DamageRegion::clipTo(const DamageRect& bounds) {
  for (std::size_t i = 0; i < m_count;) {
    DamageRect& r = m_rects[i];
    r = {std::max(r.x0, bounds.x0), std::max(r.y0, bounds.y0), std::min(r.x1, bounds.x1), std::min(r.y1, bounds.y1)};
    if (r.empty()) {
      removeAt(i);
    } else {
      ++i;
    }
  }
}

std::optional<DamageRegion> DamageHistory::regionFor(int bufferAge, const DamageRegion& damage) const {
  if (bufferAge <= 0 || static_cast<std::size_t>(bufferAge - 1) > m_count) {
    return std::nullopt;
  }
  DamageRegion region = damage;
  for (std::size_t back = 1; back < static_cast<std::size_t>(bufferAge); ++back) {
    region.add(m_frames[(m_head + kFrames - back) % kFrames]);
  }
  return region;
}

void DamageHistory::push(const DamageRegion& damage) {
  m_frames[m_head] = damage;
  m_head = (m_head + 1) % kFrames;
  m_count = std::min(m_count + 1, kFrames);
}

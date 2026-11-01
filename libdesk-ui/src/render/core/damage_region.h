#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

// Axis-aligned rect in logical surface coordinates, [x0, x1) x [y0, y1).
struct DamageRect {
  float x0 = 0.0F;
  float y0 = 0.0F;
  float x1 = 0.0F;
  float y1 = 0.0F;

  [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
  [[nodiscard]] float area() const noexcept { return empty() ? 0.0F : (x1 - x0) * (y1 - y0); }
  [[nodiscard]] bool overlaps(const DamageRect& o) const noexcept {
    return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
  }
  [[nodiscard]] bool contains(const DamageRect& o) const noexcept {
    return x0 <= o.x0 && y0 <= o.y0 && o.x1 <= x1 && o.y1 <= y1;
  }
  bool operator==(const DamageRect&) const = default;
};

[[nodiscard]] DamageRect uniteRects(const DamageRect& a, const DamageRect& b) noexcept;

// The changed area of one frame: at most kMaxRects rects that do not overlap. Rects that overlap merge; when the list
// is full, a new rect merges into the rect it grows least.
class DamageRegion {
public:
  static constexpr std::size_t kMaxRects = 8;

  void add(const DamageRect& rect);
  void add(const DamageRegion& region);
  void clipTo(const DamageRect& bounds);
  void clear() noexcept { m_count = 0; }
  [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
  [[nodiscard]] std::span<const DamageRect> rects() const noexcept { return {m_rects.data(), m_count}; }

private:
  void removeAt(std::size_t index);

  std::array<DamageRect, kMaxRects> m_rects{};
  std::size_t m_count = 0;
};

// Damage of the last frames, to repaint a reused buffer that is `bufferAge` frames old (EGL_EXT_buffer_age).
class DamageHistory {
public:
  static constexpr std::size_t kFrames = 4;

  void reset() noexcept { m_count = 0; }
  // Region to repaint for this frame's `damage`; nullopt means repaint everything (unknown or too old buffer).
  [[nodiscard]] std::optional<DamageRegion> regionFor(int bufferAge, const DamageRegion& damage) const;
  void push(const DamageRegion& damage);

private:
  std::array<DamageRegion, kFrames> m_frames{};
  std::size_t m_head = 0; // next slot to write
  std::size_t m_count = 0;
};

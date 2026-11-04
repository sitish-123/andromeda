#ifndef ANDROMEDAFX_TYPES_FX_CLIPPED_REGION_H
#define ANDROMEDAFX_TYPES_FX_CLIPPED_REGION_H

#include <linux/stddef.h>
#include <wlr/util/box.h>

// 8 bytes wide, cheap with optimisations. A negative radius is a concave flare outside the box (corner_alpha.frag).
// Note: layout is on purpose to be clockwise
struct fx_corner_radii {
  int16_t top_left;
  int16_t top_right;
  int16_t bottom_right;
  int16_t bottom_left;
};

#define CORNER_RADIUS_MAX (INT16_MAX)

static __always_inline int16_t corner_radius_clamp(int radius) {
  if (radius <= 0) {
    return 0;
  }

  if (radius > CORNER_RADIUS_MAX) {
    return CORNER_RADIUS_MAX;
  }

  return radius;
}

static __always_inline struct fx_corner_radii
corner_radii_new(int top_left, int top_right, int bottom_right, int bottom_left) {
  return (struct fx_corner_radii){
      corner_radius_clamp(top_left),
      corner_radius_clamp(top_right),
      corner_radius_clamp(bottom_right),
      corner_radius_clamp(bottom_left),
  };
}

// The corners of the `edge` side (1 top, 2 right, 3 bottom, 4 left) flare concave by `flare` px along that edge,
// outside the box; the other two keep `radius`.
static __always_inline struct fx_corner_radii corner_radii_joined(int radius, int edge, int flare) {
  struct fx_corner_radii corners = corner_radii_new(radius, radius, radius, radius);
  const int16_t f = (int16_t)-corner_radius_clamp(flare);
  if (f == 0) {
    return corners;
  }
  if (edge == 1 || edge == 4) {
    corners.top_left = f;
  }
  if (edge == 1 || edge == 2) {
    corners.top_right = f;
  }
  if (edge == 2 || edge == 3) {
    corners.bottom_right = f;
  }
  if (edge == 3 || edge == 4) {
    corners.bottom_left = f;
  }
  return corners;
}

static __always_inline struct fx_corner_radii corner_radii_all(int radius) {
  return corner_radii_new(radius, radius, radius, radius);
}

static __always_inline struct fx_corner_radii corner_radii_none(void) { return corner_radii_all(0); }

#define corner_radii_func(name, tl, tr, br, bl)                                                                        \
  static __always_inline struct fx_corner_radii corner_radii_##name(int radius) {                                      \
    return corner_radii_new(tl, tr, br, bl);                                                                           \
  }

corner_radii_func(top, radius, radius, 0, 0);
corner_radii_func(bottom, 0, 0, radius, radius);
corner_radii_func(left, radius, 0, 0, radius);
corner_radii_func(right, 0, radius, radius, 0);

struct fx_corner_radii fx_corner_radii_extend(struct fx_corner_radii corners, int extend);

void fx_corner_radii_transform(enum wl_output_transform transform, struct fx_corner_radii* corners);

bool fx_corner_radii_eq(struct fx_corner_radii lhs, struct fx_corner_radii rhs);

/**
 * Retains the corner values from `input` based on if the corresponding corner value in `filter` is non-zero
 *
 * This can be compared to an `AND` operation
 */
static __always_inline struct fx_corner_radii
fx_corner_radii_filter(struct fx_corner_radii input, struct fx_corner_radii filter) {
  return corner_radii_new(
      input.top_left && filter.top_left ? input.top_left : 0, input.top_right && filter.top_right ? input.top_right : 0,
      input.bottom_right && filter.bottom_right ? input.bottom_right : 0,
      input.bottom_left && filter.bottom_left ? input.bottom_left : 0
  );
};

/**
 * Picks the value of the corner from `lhs` if non-zero, otherwise picks the corner value from `rhs`
 *
 * This can be compared to an `OR` operation
 */
static __always_inline struct fx_corner_radii
fx_corner_radii_pick(struct fx_corner_radii lhs, struct fx_corner_radii rhs) {
  return corner_radii_new(
      lhs.top_left ? lhs.top_left : rhs.top_left, lhs.top_right ? lhs.top_right : rhs.top_right,
      lhs.bottom_right ? lhs.bottom_right : rhs.bottom_right, lhs.bottom_left ? lhs.bottom_left : rhs.bottom_left
  );
};

bool fx_corner_radii_is_empty(const struct fx_corner_radii* corners);

struct clipped_region {
  struct wlr_box area;
  struct fx_corner_radii corners;
};

struct clipped_region clipped_region_get_default(void);

#endif // !ANDROMEDAFX_TYPES_FX_CLIPPED_REGION_H

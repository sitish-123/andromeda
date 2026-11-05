float rounded_rect_distance_at(
    vec2 point, vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
  vec2 relative_pos = point - position;
  vec2 centered_pos = relative_pos - size * 0.5;

  float radius;
  if (centered_pos.x < 0.0) {
    radius = centered_pos.y < 0.0 ? radius_tl : radius_bl;
  } else {
    radius = centered_pos.y < 0.0 ? radius_tr : radius_br;
  }

  // Continuous-curvature corner, the same formula as libdesk-ui smooth_corner_glsl.h (radius.smoothing token).
  vec2 half_size = size * 0.5;
  float max_r = min(half_size.x, half_size.y);
  float s = radius > 0.0 ? clamp((max_r / radius - 1.0) / 0.88, 0.0, TOKEN_RADIUS_SMOOTHING) : 0.0;
  float e = min(radius * (1.0 + 0.88 * s), max_r);
  vec2 q = abs(centered_pos) - (half_size - vec2(e));
  if (s > 0.001 && q.x > 0.0 && q.y > 0.0) {
    float n = 2.0 + 5.0 * s;
    vec2 u = q / e;
    return (pow(pow(u.x, n) + pow(u.y, n), 1.0 / n) - 1.0) * e;
  }
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - e;
}

float rounded_rect_distance(
    vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
  return rounded_rect_distance_at(gl_FragCoord.xy, size, position, radius_tl, radius_tr, radius_bl, radius_br);
}

#ifdef CORNER_FLARES
// Coverage of the concave flare of the corner at `corner` (sign = outward direction from the box centre), radius
// f > 0, along x (`along_x`) or y; -1 when rel is outside that flare's band.
float flare_coverage(vec2 rel, vec2 corner, vec2 outward, float f, bool along_x) {
  vec2 d = rel - corner;
  vec2 band = along_x ? vec2(d.x * outward.x, -d.y * outward.y) : vec2(-d.x * outward.x, d.y * outward.y);
  if (band.x <= 0.0 || band.y < 0.0 || band.x > f || band.y > f) {
    return -1.0;
  }
  return clamp(length(band - vec2(f, f)) - f + 0.5, 0.0, 1.0);
}

// A negative radius is a concave flare of that size outside the box (only in CORNER_FLARES programs: the extra code
// costs every glass pixel on Gen9 even when no radius is negative): the two negative corners of one edge flare
// along that edge (a panel joined to a bar).
float flare_alpha(vec2 rel, vec2 size, float tl, float tr, float bl, float br) {
  float c = -1.0;
  if (tl < 0.0) c = max(c, flare_coverage(rel, vec2(0.0), vec2(-1.0, -1.0), -tl, tr < 0.0));
  if (tr < 0.0) c = max(c, flare_coverage(rel, vec2(size.x, 0.0), vec2(1.0, -1.0), -tr, tl < 0.0));
  if (bl < 0.0) c = max(c, flare_coverage(rel, vec2(0.0, size.y), vec2(-1.0, 1.0), -bl, br < 0.0));
  if (br < 0.0) c = max(c, flare_coverage(rel, size, vec2(1.0, 1.0), -br, bl < 0.0));
  return c;
}

// A box with flares (negative radii) includes them: the body is the box less the flares on the two sides of the
// joined edge.
void flare_body(inout vec2 size, inout vec2 position, float tl, float tr, float bl, float br) {
  vec4 f = max(-vec4(tl, tr, bl, br), 0.0);
  if ((tl < 0.0 && tr < 0.0) || (bl < 0.0 && br < 0.0)) {
    vec2 cut = vec2(max(f.x, f.z), max(f.y, f.w));
    position.x += cut.x;
    size.x -= cut.x + cut.y;
  } else {
    vec2 cut = vec2(max(f.x, f.y), max(f.z, f.w));
    position.y += cut.x;
    size.y -= cut.x + cut.y;
  }
}

#endif

// The signed distance to the body of a box whose negative radii are flares (the rim and lens outline).
float body_distance_at(vec2 point, vec2 size, vec2 position, float tl, float tr, float bl, float br) {
#ifdef CORNER_FLARES
  if (min(min(tl, tr), min(bl, br)) < 0.0) {
    flare_body(size, position, tl, tr, bl, br);
  }
  return rounded_rect_distance_at(point, size, position, max(tl, 0.0), max(tr, 0.0), max(bl, 0.0), max(br, 0.0));
#else
  return rounded_rect_distance_at(point, size, position, tl, tr, bl, br);
#endif
}

// Returns 0.0 outside and 1.0 inside the rounded rectangle. A cutout reverses
// the result so the rectangle interior becomes transparent.
float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
#ifdef CORNER_FLARES
  if (min(min(radius_tl, radius_tr), min(radius_bl, radius_br)) < 0.0) {
    flare_body(size, position, radius_tl, radius_tr, radius_bl, radius_br);
    float flare = flare_alpha(gl_FragCoord.xy - position, size, radius_tl, radius_tr, radius_bl, radius_br);
    if (flare >= 0.0) {
      return is_cutout ? 1.0 - flare : flare;
    }
    radius_tl = max(radius_tl, 0.0);
    radius_tr = max(radius_tr, 0.0);
    radius_bl = max(radius_bl, 0.0);
    radius_br = max(radius_br, 0.0);
  }
#endif
  // Exact shortcuts: 1 px inside the part no corner curve reaches, or 1 px outside the box, the smoothstep below is
  // 0 or 1. Most fragments of a large surface take the first one.
  vec2 rel = gl_FragCoord.xy - position;
  float radius = max(max(radius_tl, radius_tr), max(radius_bl, radius_br));
  float inset = min(radius * (1.0 + 0.88 * TOKEN_RADIUS_SMOOTHING), min(size.x, size.y) * 0.5) + 1.0;
  if (rel.x >= inset && rel.y >= inset && rel.x <= size.x - inset && rel.y <= size.y - inset) {
    return is_cutout ? 0.0 : 1.0;
  }
  if (rel.x <= -1.0 || rel.y <= -1.0 || rel.x >= size.x + 1.0 || rel.y >= size.y + 1.0) {
    return is_cutout ? 1.0 : 0.0;
  }
  float dist = rounded_rect_distance(size, position, radius_tl, radius_tr, radius_bl, radius_br);
  float result = smoothstep(0.0, 1.0, dist);
  return is_cutout ? result : 1.0 - result;
}

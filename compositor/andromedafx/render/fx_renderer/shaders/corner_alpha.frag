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

// Returns 0.0 outside and 1.0 inside the rounded rectangle. A cutout reverses
// the result so the rectangle interior becomes transparent.
float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
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

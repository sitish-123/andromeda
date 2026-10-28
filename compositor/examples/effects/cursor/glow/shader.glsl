// A soft accent-colored halo around the pointer, breathing with time.
vec4 cursor(vec2 uv) {
  vec4 c = andromeda_sample(uv);
  float d = distance(uv, andromeda_pointer) * 2.0;
  float breath = 0.75 + 0.25 * sin(andromeda_time * 3.0);
  vec4 tint = andromeda_palette_count > 0 ? andromeda_palette_at(0.0) : vec4(0.48, 0.64, 1.0, 1.0);
  float halo = exp(-d * d * 6.0) * 0.5 * breath;
  return c * (1.0 - halo) + tint * halo;
}

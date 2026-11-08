precision highp float;

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec2 size;
uniform vec2 position;
uniform float radius_top_left;
uniform float radius_top_right;
uniform float radius_bottom_left;
uniform float radius_bottom_right;

uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

uniform float fade;
uniform vec2 fade_from;
uniform vec2 fade_to;

float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
);

void main() {
  float quad_corner_alpha = corner_alpha(
      size - 1.0, position + 0.5, false, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right
  );

  // Clipping
  float clip_corner_alpha = corner_alpha(
      clip_size - 1.0, clip_position + 0.5, true, clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left,
      clip_radius_bottom_right
  );

  float fade_alpha = 1.0;
  if (fade > 0.0) {
    vec2 axis = fade_to - fade_from;
    float t = clamp(dot(gl_FragCoord.xy - fade_from, axis) / max(dot(axis, axis), 1.0), 0.0, 1.0);
    // Light from above: full on the top edge, mostly gone a third of the way down the sides.
    float s = 1.0 - t;
    fade_alpha = 1.0 - fade * (1.0 - s * s * s * s);
  }

  gl_FragColor = v_color * quad_corner_alpha * clip_corner_alpha * fade_alpha;
}

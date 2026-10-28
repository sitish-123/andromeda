uniform vec2 andromeda_workspace_axis;

vec4 animation(vec2 uv) {
  float coordinate = dot(uv, abs(andromeda_workspace_axis));
  if (andromeda_workspace_axis.x + andromeda_workspace_axis.y < 0.0)
    coordinate = 1.0 - coordinate;
  float visible = step(coordinate, andromeda_clamped_progress);
  return mix(andromeda_sample(uv), andromeda_sample_incoming(uv), visible);
}

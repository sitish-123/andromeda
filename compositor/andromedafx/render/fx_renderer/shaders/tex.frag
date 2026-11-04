#define SOURCE % d
#define EFFECTS % d
#define SAMPLE_CLAMP % d

#define SOURCE_TEXTURE_RGBA 1
#define SOURCE_TEXTURE_RGBX 2
#define SOURCE_TEXTURE_EXTERNAL 3

#if !defined(SOURCE) || !defined(EFFECTS) || !defined(SAMPLE_CLAMP)
#error "Missing shader preamble"
#endif

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

precision highp float;

varying vec2 v_texcoord;

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
uniform samplerExternalOES tex;
#elif SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_RGBX
uniform sampler2D tex;
#endif

uniform float alpha;
uniform int source_tf;
uniform mat3 primaries_matrix;
uniform float lum_multiplier;
uniform vec2 source_luminances;
uniform int target_tf;
uniform bool map_hdr_to_sdr;

#define TRANSFER_FUNCTION_PASSTHROUGH 0
#define TRANSFER_FUNCTION_SRGB 1
#define TRANSFER_FUNCTION_ST2084_PQ 2
#define TRANSFER_FUNCTION_EXT_LINEAR 4
#define TRANSFER_FUNCTION_GAMMA22 8
#define TRANSFER_FUNCTION_BT1886 16

#if EFFECTS
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

// Liquid glass (0 = off): the backdrop bends toward the centre near the rim and a soft specular rim lights the edge.
uniform float liquid;
// Specular rim alone (0 = off) for frosted shapes: the rim draws at max(liquid, rim), refraction only with liquid.
uniform float rim;
// Lens (0 = off): the sharp backdrop, displaced by the glass thickness, shows through the rim band over the blur.
uniform float lens;
uniform sampler2D lens_tex;
// Density: the backdrop mixed toward the tint colour (rgb) by a, so the client's tint reads denser.
uniform vec4 tint_mix;
// UV step of one framebuffer pixel; the y sign follows the texture orientation.
uniform vec2 liquid_texel;
// The shape whose edge gets the rim; liquid_radius < 0 = the quad's own corners.
uniform vec2 liquid_size;
uniform vec2 liquid_position;
uniform float liquid_radius;
#endif

uniform float discard_transparent;

#if SAMPLE_CLAMP
// Normalized texel range the fragment may sample: xy is the first sampled texel
// center, zw the last. A cropped surface whose crop edge falls between texels
// snaps its source box to whole texels for sharpness, which can leave the box
// reaching one texel past the crop. Clamping here duplicates the last texel
// inside the crop instead of pulling in whatever the client left outside it,
// which for a client-side-decorated window is its transparent shadow margin.
// The clamp is compiled only into the variants selected for such crops, so
// every other draw keeps the exact fetch drivers already handle well.
uniform vec4 sample_bounds;
#define SAMPLE_UV(uv) clamp(uv, sample_bounds.xy, sample_bounds.zw)
#else
#define SAMPLE_UV(uv) (uv)
#endif

vec4 sample_texture(vec2 uv) {
#if SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_EXTERNAL
  return texture2D(tex, SAMPLE_UV(uv));
#elif SOURCE == SOURCE_TEXTURE_RGBX
  return vec4(texture2D(tex, SAMPLE_UV(uv)).rgb, 1.0);
#endif
}

float srgb_channel_to_linear(float x) { return x > 0.04045 ? pow((x + 0.055) / 1.055, 2.4) : x / 12.92; }

vec3 srgb_color_to_linear(vec3 color) {
  return vec3(srgb_channel_to_linear(color.r), srgb_channel_to_linear(color.g), srgb_channel_to_linear(color.b));
}

vec3 pq_color_to_linear(vec3 color) {
  float inv_m1 = 1.0 / 0.1593017578125;
  float inv_m2 = 1.0 / 78.84375;
  float c1 = 0.8359375;
  float c2 = 18.8515625;
  float c3 = 18.6875;
  vec3 power = pow(color, vec3(inv_m2));
  vec3 num = max(power - c1, 0.0);
  vec3 denom = c2 - c3 * power;
  return pow(num / denom, vec3(inv_m1));
}

vec3 bt1886_color_to_linear(vec3 color) {
  float l_min = source_luminances.x;
  float l_max = source_luminances.y;
  float lb = pow(l_min, 1.0 / 2.4);
  float lw = pow(l_max, 1.0 / 2.4);
  float a = pow(lw - lb, 2.4);
  float b = lb / (lw - lb);
  vec3 luminance = a * pow(color + vec3(b), vec3(2.4));
  return (luminance - l_min) / (l_max - l_min);
}

float linear_channel_to_srgb(float x) { return x > 0.0031308 ? 1.055 * pow(x, 1.0 / 2.4) - 0.055 : 12.92 * x; }

vec3 linear_color_to_srgb(vec3 color) {
  return vec3(linear_channel_to_srgb(color.r), linear_channel_to_srgb(color.g), linear_channel_to_srgb(color.b));
}

vec3 linear_color_to_gamma22(vec3 color) { return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2)); }

vec3 hdr_to_sdr(vec3 color) {
  // Retain shadows and midtones, then roll highlights toward SDR peak with a
  // continuous Reinhard shoulder. Scaling the triplet retains channel ratios
  // instead of turning distinct bright colors into independently clipped ones.
  const float knee = 0.8;
  float peak = max(max(color.r, color.g), color.b);
  if (peak <= knee) {
    return color;
  }
  float excess = peak - knee;
  float mapped_peak = knee + excess / (1.0 + excess / (1.0 - knee));
  return color * (mapped_peak / peak);
}

vec4 convert_color(vec4 color) {
  if (source_tf == TRANSFER_FUNCTION_PASSTHROUGH) {
    return color;
  }

  float color_alpha = color.a;
  vec3 rgb = color_alpha == 0.0 ? vec3(0.0) : color.rgb / color_alpha;
  // Extended-linear negative channels carry colors outside the source gamut.
  // Preserve them through the primary conversion and linear blend buffer.
  if (source_tf != TRANSFER_FUNCTION_EXT_LINEAR) {
    rgb = max(rgb, vec3(0.0));
  }

  if (source_tf == TRANSFER_FUNCTION_SRGB) {
    rgb = srgb_color_to_linear(rgb);
  } else if (source_tf == TRANSFER_FUNCTION_ST2084_PQ) {
    rgb = pq_color_to_linear(rgb);
  } else if (source_tf == TRANSFER_FUNCTION_GAMMA22) {
    rgb = pow(rgb, vec3(2.2));
  } else if (source_tf == TRANSFER_FUNCTION_BT1886) {
    rgb = bt1886_color_to_linear(rgb);
  }

  rgb *= lum_multiplier;
  rgb = primaries_matrix * rgb;
  if (map_hdr_to_sdr) {
    rgb = hdr_to_sdr(rgb);
  }
  if (target_tf == TRANSFER_FUNCTION_SRGB) {
    rgb = linear_color_to_srgb(max(rgb, vec3(0.0)));
  } else if (target_tf == TRANSFER_FUNCTION_GAMMA22) {
    rgb = linear_color_to_gamma22(rgb);
  }

  return vec4(rgb * color_alpha, color_alpha);
}

#if EFFECTS
float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
);
float rounded_rect_distance_at(
    vec2 point, vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
);

float liquid_distance(vec2 point) {
  if (liquid_radius >= 0.0) {
    return rounded_rect_distance_at(
        point, liquid_size, liquid_position, liquid_radius, liquid_radius, liquid_radius, liquid_radius
    );
  }
  return rounded_rect_distance_at(
      point, size, position, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right
  );
}
#endif

void main() {
  vec2 uv = v_texcoord;
#if EFFECTS
  float specular = 0.0;
  float lens_weight = 0.0;
  vec2 lens_uv = uv;
  float shine = max(liquid, rim);
  if (shine > 0.0) {
    // Rim band: a quarter of the short side, at most 16 px. t = 1 at the edge, 0 at the band's inner end.
    float band = min(min(liquid_size.x, liquid_size.y) * 0.25, 16.0);
    vec2 p = gl_FragCoord.xy;
    float d = liquid_distance(p);
    // Past the rim band there is no refraction and no rim: skip the four extra distance evaluations.
    if (d > -band) {
      vec2 normal = vec2(liquid_distance(p + vec2(1.0, 0.0)) - liquid_distance(p - vec2(1.0, 0.0)),
                         liquid_distance(p + vec2(0.0, 1.0)) - liquid_distance(p - vec2(0.0, 1.0)));
      normal = length(normal) > 0.0 ? normalize(normal) : vec2(0.0);
      float t = clamp(1.0 + d / band, 0.0, 1.0);
      // Refraction: sample from further inside, strongest at the rim (up to 0.8 band at Clear, 0.4 at Tinted).
      uv -= normal * (t * t * band * 0.8 * liquid) * liquid_texel;
      // Lens: a convex edge magnifies the sharp backdrop toward the centre; it fades into the blur over the band.
      lens_uv = uv - normal * (t * t * band * 0.6 * lens) * liquid_texel;
      lens_weight = lens * t * t * 0.85;
      // Specular: a reflection, not an outline. Brightest on the outermost pixel (d = -0.5), fading over 2.5 px
      // inward; the coverage mask cuts it outside. pow 8 keeps the key glint on the curves that face the top-left
      // light (a straight top edge gets 6 %), a back glint opposite, a faint even rim between. y-down: light (-1, -1).
      float inner = clamp(1.0 + (d + 0.5) / 2.5, 0.0, 1.0);
      float edge = inner * inner;
      float l = dot(normal, normalize(vec2(-1.0, -1.0)));
      float key = pow(max(l, 0.0), 8.0);
      float back = 0.4 * pow(max(-l, 0.0), 8.0);
      float facing = 0.1 + key + back;
      float glow = clamp(1.0 + 2.0 * d / band, 0.0, 1.0);
      specular = shine * (0.08 * glow * glow + 0.45 * edge) * facing;
    }
  }
  vec4 base = sample_texture(uv);
  if (lens_weight > 0.0) {
    base = mix(base, texture2D(lens_tex, clamp(lens_uv, 0.0, 1.0)), lens_weight);
  }
  vec4 color = convert_color(base);
  color.rgb = mix(color.rgb, tint_mix.rgb * color.a, tint_mix.a);
  color.rgb += specular * color.a;
#else
  vec4 color = convert_color(sample_texture(uv));
#endif
#if EFFECTS
  float quad_corner_alpha = corner_alpha(
      size - 0.5, position + 0.25, false, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right
  );

  // Clipping
  float clip_corner_alpha = corner_alpha(
      clip_size - 1.0, clip_position + 0.5, true, clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left,
      clip_radius_bottom_right
  );

  gl_FragColor = color * alpha * quad_corner_alpha * clip_corner_alpha;
#else
  gl_FragColor = color * alpha;
#endif

  if (gl_FragColor.a < discard_transparent) {
    discard;
  }
}

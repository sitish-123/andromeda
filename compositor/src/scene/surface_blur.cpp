#include "scene/surface_blur.h"

#include "config/config.h"
// clang-format off
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on
#include <pixman.h>

namespace andromeda {

  namespace {
    // Liquid glass needs a light blur, or the refraction bends a smear (.agent/mockups/launcher-material.html: liquid
    // blur 10 px against the frosted sheet's strong blur).
    float liquidBlurStrength(float liquid) { return 1.0F - 0.65F * liquid; }

    wlr_scene_buffer* findSurfaceBuffer(wlr_scene_node& root, wlr_surface* surface) {
      struct Lookup {
        wlr_surface* surface;
        wlr_scene_buffer* buffer = nullptr;
      } lookup{surface};

      wlr_scene_node_for_each_buffer(
          &root,
          [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            auto* lookup = static_cast<Lookup*>(data);
            if (lookup->buffer != nullptr) {
              return;
            }
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface != nullptr && sceneSurface->surface == lookup->surface) {
              lookup->buffer = buffer;
            }
          },
          &lookup
      );
      return lookup.buffer;
    }

  } // namespace

  bool surfaceTransparent(wlr_surface* surface, const wlr_box& surfaceBox) {
    const pixman_box32_t box = {
        surfaceBox.x, surfaceBox.y, surfaceBox.x + surfaceBox.width, surfaceBox.y + surfaceBox.height
    };
    return pixman_region32_contains_rectangle(&surface->opaque_region, &box) != PIXMAN_REGION_IN;
  }

  void SurfaceBlur::update(
      wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
      fx_corner_radii corners, const wlr_box* clipBox, const SurfaceBlurOptions& options, float surfaceOpacity,
      wlr_scene_buffer* maskSource, const pixman_region32_t* clipRegion
  ) {
    const Config::Appearance::Blur& cfg = config().appearance.blur;
    wlr_box drawBox = nodeBox;
    if (clipBox != nullptr) {
      if (!wlr_box_intersection(&drawBox, &nodeBox, clipBox) || drawBox.width <= 0 || drawBox.height <= 0) {
        if (m_node != nullptr) {
          wlr_scene_node_set_enabled(&m_node->node, false);
        }
        return;
      }
    }
    // A compositor opacity below 1 makes even a client-declared opaque surface
    // reveal the backdrop, so it also requires a blur node. Its opacity does
    // not attenuate the blur itself: client-provided alpha does not either.
    const bool want = cfg.enabled
        && options.enabled.value_or(false)
        && drawBox.width > 0
        && drawBox.height > 0
        && (surfaceOpacity < 1.0F || surfaceTransparent(surface, surfaceBox));
    if (!want) {
      if (m_node != nullptr) {
        wlr_scene_node_set_enabled(&m_node->node, false);
      }
      return;
    }

    if (m_node != nullptr && m_masked != (options.ignoreAlpha > 0.0F)) {
      wlr_scene_node_destroy(&m_node->node);
      m_node = nullptr;
    }

    if (m_node == nullptr) {
      m_node = wlr_scene_blur_create(parent, drawBox.width, drawBox.height);
      if (m_node == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&m_node->node);
      m_masked = options.ignoreAlpha > 0.0F;
    }

    wlr_scene_buffer* currentMask = wlr_scene_blur_get_transparency_mask_source(m_node);
    if (maskSource != nullptr) {
      if (currentMask != maskSource) {
        wlr_scene_blur_set_transparency_mask_source(m_node, maskSource);
      }
    } else if (currentMask == nullptr) {
      if (wlr_scene_buffer* mask = findSurfaceBuffer(parent->node, surface)) {
        wlr_scene_blur_set_transparency_mask_source(m_node, mask);
      }
    }

    if (m_node->ignore_alpha != options.ignoreAlpha) {
      wlr_scene_blur_set_ignore_alpha(m_node, options.ignoreAlpha);
    }

    m_optimized = options.optimized.value_or(cfg.optimized);
    wlr_scene_blur_set_should_only_blur_bottom_layer(m_node, m_optimized && !m_live);

    wlr_scene_node_set_enabled(&m_node->node, true);
    wlr_scene_node_set_position(&m_node->node, drawBox.x, drawBox.y);
    if (m_node->width != drawBox.width || m_node->height != drawBox.height) {
      wlr_scene_blur_set_size(m_node, drawBox.width, drawBox.height);
    }
    if (!fx_corner_radii_eq(m_node->corners, corners)) {
      wlr_scene_blur_set_corner_radii(m_node, corners);
    }
    if (m_node->alpha != m_alpha) {
      wlr_scene_blur_set_alpha(m_node, m_alpha);
    }
    wlr_scene_blur_set_liquid(m_node, m_liquid);
    wlr_scene_blur_set_strength(m_node, liquidBlurStrength(m_liquid));
    // The rim follows the client's blur region (the drawn shape), not the node box: a panel surface is larger than
    // its sheet. Liquid surfaces are capsules (the launcher pill), so the radius is half the short side.
    if (clipRegion != nullptr && pixman_region32_not_empty(clipRegion)) {
      const pixman_box32_t* e = pixman_region32_extents(clipRegion);
      const wlr_box shape{e->x1 + nodeBox.x - drawBox.x, e->y1 + nodeBox.y - drawBox.y, e->x2 - e->x1, e->y2 - e->y1};
      wlr_scene_blur_set_liquid_shape(m_node, &shape, std::min(shape.width, shape.height) / 2);
    } else {
      wlr_scene_blur_set_liquid_shape(m_node, nullptr, 0);
    }
    wlr_scene_buffer* shapeSource = wlr_scene_blur_get_transparency_mask_source(m_node);
    const pixman_region32_t* shape =
        shapeSource != nullptr ? wlr_scene_node_get_clip_region(&shapeSource->node) : nullptr;
    pixman_region32_t clip;
    pixman_region32_init(&clip);
    if (shape != nullptr) {
      pixman_region32_copy(&clip, shape);
      int sourceX = 0;
      int sourceY = 0;
      int blurX = 0;
      int blurY = 0;
      wlr_scene_node_coords(&shapeSource->node, &sourceX, &sourceY);
      wlr_scene_node_coords(&m_node->node, &blurX, &blurY);
      pixman_region32_translate(&clip, sourceX - blurX, sourceY - blurY);
    }
    if (clipRegion != nullptr) {
      pixman_region32_t region;
      pixman_region32_init(&region);
      pixman_region32_copy(&region, clipRegion);
      pixman_region32_translate(&region, nodeBox.x - drawBox.x, nodeBox.y - drawBox.y);
      if (shape != nullptr) {
        pixman_region32_intersect(&clip, &clip, &region);
      } else {
        pixman_region32_copy(&clip, &region);
      }
      pixman_region32_fini(&region);
    }
    wlr_scene_node_set_clip_region(&m_node->node, shape != nullptr || clipRegion != nullptr ? &clip : nullptr);
    pixman_region32_fini(&clip);
  }

  void SurfaceBlur::setAlpha(float alpha) {
    m_alpha = alpha;
    if (m_node != nullptr && m_node->alpha != alpha) {
      wlr_scene_blur_set_alpha(m_node, alpha);
    }
  }

  void SurfaceBlur::setLiquid(float liquid) {
    m_liquid = liquid;
    if (m_node != nullptr) {
      wlr_scene_blur_set_liquid(m_node, liquid);
      wlr_scene_blur_set_strength(m_node, liquidBlurStrength(liquid));
    }
  }

  void SurfaceBlur::setLive(bool live) {
    m_live = live;
    if (m_node != nullptr) {
      wlr_scene_blur_set_should_only_blur_bottom_layer(m_node, m_optimized && !m_live);
    }
  }

  void SurfaceBlur::hide() {
    if (m_node != nullptr) {
      wlr_scene_node_set_enabled(&m_node->node, false);
    }
  }

  bool SurfaceBlur::active() const { return m_node != nullptr && m_node->node.enabled; }

} // namespace andromeda

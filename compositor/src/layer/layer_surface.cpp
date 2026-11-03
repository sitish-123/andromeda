#include "layer/layer_surface.h"

#include "server/background_effect.h"

#include "scene/effect_registry.h"
extern "C" {
#include <andromedafx/render/effect.h>
}

#include "config/resolve.h"
#include "design_tokens.h"
#include "core/dirty.h"
#include "core/log.h"
#include "input/seat.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/border_rect.h"
#include "server/dsk_surface.h"
#include "server/server.h"
#include "view/popup.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <optional>
#include <cstdlib>

namespace andromeda {

  namespace {

    // A spring overshoot above 1 would upscale the buffer and flip its whole-pixel position and size for the frames it
    // takes to settle, so the shown scale stops at the buffer's own size.
    // A spring tail that shrinks the surface by less than half a pixel shows as 1: resampling at 0.999 would smear
    // 1 px lines (the sheet hairline) until the spring settles.
    double shownScale(const SurfaceMotion& motion, const wlr_surface* surface) {
      const double scale = std::clamp(motion.value(SurfaceMotion::Property::Scale)[0], 0.01, 1.0);
      const double extent = std::max(surface->current.width, surface->current.height);
      return (1.0 - scale) * extent < 0.5 ? 1.0 : scale;
    }

    // Scale and Translate together scale about an anchor (PanelManager::detachedScaleShift). A shown scale clamped
    // from the spring's value keeps that anchor: the translate shrinks with it, or its overshoot moves the surface.
    std::array<double, 2> shownTranslate(const SurfaceMotion& motion, double shownScale) {
      const double scale = motion.value(SurfaceMotion::Property::Scale)[0];
      const auto& translate = motion.value(SurfaceMotion::Property::Translate);
      const double keep = scale == shownScale ? 1.0 : (1.0 - shownScale) / (1.0 - scale);
      return {translate[0] * keep, translate[1] * keep};
    }

    // The blur's corner is this much rounder than the shape: an integer or staircase radius can come out smaller than
    // the client's anti-aliased curve, and the blurred backdrop then shows past it as bright dots on the corners.
    constexpr int kBlurCornerSlack = 1;

    struct RoundedShape {
      wlr_box box;
      int radius;
    };

    // One rounded rectangle sent as a pixel staircase (one band per row inside the curve, e.g.
    // Surface::tessellateRoundedRect; rects in band order) -> its box and corner radius. Holes or an asymmetric band
    // -> nullopt.
    std::optional<RoundedShape> roundedShapeOf(const std::vector<pixman_box32_t>& rects) {
      pixman_box32_t e = rects.front();
      for (const pixman_box32_t& r : rects) {
        e = {std::min(e.x1, r.x1), std::min(e.y1, r.y1), std::max(e.x2, r.x2), std::max(e.y2, r.y2)};
      }
      const int width = e.x2 - e.x1;
      const int height = e.y2 - e.y1;
      std::optional<int> radius;
      for (std::size_t i = 0; i < rects.size(); ++i) {
        const pixman_box32_t& r = rects[i];
        const bool sameBand = i > 0 && rects[i - 1].y1 == r.y1;
        const bool gap = i > 0 && rects[i - 1].y2 != r.y1;
        const int left = r.x1 - e.x1;
        const int right = e.x2 - r.x2;
        if (sameBand || gap || std::abs(left - right) > 1) {
          return std::nullopt;
        }
        // The last row of a corner band reaches the full width; pixman merges it into the straight band below.
        if (!radius && left == 0 && right == 0) {
          const int offset = r.y1 - e.y1;
          radius = offset > 0 ? offset + 1 : 0;
        }
      }
      // No full-width band: a capsule, the curve spans the whole short side.
      return RoundedShape{{e.x1, e.y1, width, height}, radius.value_or(std::min(width, height) / 2)};
    }

    // A client blur region made of rounded rectangles (a bar sends all its islands in one region) -> one shape each,
    // so every blur gets the shader's anti-aliased edge instead of the staircase. Any other shape -> empty.
    std::vector<RoundedShape> roundedShapesOf(const pixman_region32_t* region) {
      int count = 0;
      const pixman_box32_t* rects = pixman_region32_rectangles(region, &count);
      // Rects that touch across adjacent bands belong to one shape.
      std::vector<int> parent(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        parent[static_cast<std::size_t>(i)] = i;
      }
      const auto root = [&parent](int i) {
        while (parent[static_cast<std::size_t>(i)] != i) {
          i = parent[static_cast<std::size_t>(i)];
        }
        return i;
      };
      for (int i = 0; i < count; ++i) {
        for (int j = i - 1; j >= 0 && rects[j].y2 >= rects[i].y1; --j) {
          if (rects[j].y2 == rects[i].y1 && rects[j].x1 < rects[i].x2 && rects[i].x1 < rects[j].x2) {
            parent[static_cast<std::size_t>(root(i))] = root(j);
          }
        }
      }
      std::vector<int> roots;
      std::vector<std::vector<pixman_box32_t>> groups;
      for (int i = 0; i < count; ++i) {
        const int r = root(i);
        auto it = std::ranges::find(roots, r);
        if (it == roots.end()) {
          roots.push_back(r);
          groups.emplace_back();
          it = roots.end() - 1;
        }
        groups[static_cast<std::size_t>(it - roots.begin())].push_back(rects[i]);
      }
      std::vector<RoundedShape> shapes;
      for (const auto& group : groups) {
        const auto shape = roundedShapeOf(group);
        if (!shape) {
          return {};
        }
        shapes.push_back(*shape);
      }
      return shapes;
    }
    constexpr Logger kLog("layer");

    // Unknown values fall back to the top layer, matching Output::layerTree.
    LayerShellLayer layerOf(uint32_t layer) {
      switch (layer) {
      case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return LayerShellLayer::Background;
      case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return LayerShellLayer::Bottom;
      case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return LayerShellLayer::Top;
      case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return LayerShellLayer::Overlay;
      default:
        return LayerShellLayer::Top;
      }
    }
  } // namespace

  LayerSurface* LayerSurface::fromSurface(wlr_surface* surface) {
    wlr_surface* walk = surface;
    while (walk != nullptr) {
      walk = wlr_surface_get_root_surface(walk);
      if (wlr_layer_surface_v1* layer = wlr_layer_surface_v1_try_from_wlr_surface(walk)) {
        return static_cast<LayerSurface*>(layer->data);
      }
      if (wlr_xdg_popup* popup = wlr_xdg_popup_try_from_wlr_surface(walk)) {
        walk = popup->parent;
        continue;
      }
      break;
    }
    return nullptr;
  }

  LayerSurface::LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface)
      : SceneNode(SceneNodeKind::LayerSurface), m_server(&server), m_layerSurface(layerSurface) {
    if (m_layerSurface->output == nullptr) {
      m_layerSurface->output = m_server->preferredOutput();
    }
    if (m_layerSurface->output == nullptr) {
      kLog.error("no output for layer surface, destroying");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_layerSurface = nullptr;
      return;
    }

    Output* out = output();
    if (out == nullptr) {
      kLog.error("layer surface output has no Output wrapper, destroying");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_layerSurface = nullptr;
      return;
    }

    const auto layer = m_layerSurface->pending.layer;
    kLog.debug(
        "new layer surface ns={} layer={} output={} exclusive={}",
        m_layerSurface->namespace_ != nullptr ? m_layerSurface->namespace_ : "", static_cast<unsigned>(layer),
        m_layerSurface->output->name, m_layerSurface->pending.exclusive_zone
    );

    m_scene = wlr_scene_layer_surface_v1_create(out->layerTree(layer), m_layerSurface);
    if (m_scene == nullptr) {
      kLog.error("failed to create scene layer surface");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_layerSurface = nullptr;
      return;
    }
    m_popupTree = wlr_scene_tree_create(out->layerPopupTree(layer));
    if (m_popupTree == nullptr) {
      kLog.error("failed to create layer popup tree");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_scene = nullptr;
      m_layerSurface = nullptr;
      return;
    }
    m_popupTree->node.data = sceneNodeData(this);
    syncPopupTree();
    m_sceneUpdate.notify = onSceneUpdate;
    if (!wlr_scene_node_add_update_listener(&m_scene->tree->node, &m_sceneUpdate)) {
      kLog.error("failed to watch layer popup geometry");
      std::abort();
    }
    m_sceneDestroy.notify = onSceneDestroy;
    wl_signal_add(&m_scene->tree->node.events.destroy, &m_sceneDestroy);

    m_rule = resolveLayerRules(config(), ruleText(m_layerSurface->namespace_), layerOf(layer), m_server->uptimeMs());
    m_scene->tree->node.data = sceneNodeData(this);
    m_layerSurface->data = this;

    m_map.notify = onMap;
    wl_signal_add(&m_layerSurface->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    // Snapshot before the scene helper disables its subsurface tree. The normal
    // buffer iterator intentionally skips disabled trees.
    wl_list_insert(&m_layerSurface->surface->events.unmap.listener_list, &m_unmap.link);
    m_commit.notify = onCommit;
    wl_signal_add(&m_layerSurface->surface->events.commit, &m_commit);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_layerSurface->events.destroy, &m_destroy);
    m_newPopup.notify = onNewPopup;
    wl_signal_add(&m_layerSurface->events.new_popup, &m_newPopup);

    m_server->registerAnimatable(this);
    notifyOutputScale();
  }

  LayerSurface::~LayerSurface() {
    m_server->unregisterAnimatable(this);
    if (m_sceneDestroy.link.next != nullptr) {
      wl_list_remove(&m_sceneUpdate.link);
      wl_list_remove(&m_sceneDestroy.link);
      wlr_scene_node_destroy(&m_popupTree->node);
    }
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_newPopup.link);
    }
  }

  bool LayerSurface::hasActiveAnimations() const {
    if (m_fade.animating()) {
      return true;
    }
    const DskSurfaceManager* dsk = m_server->dskSurfaces();
    if (dsk == nullptr) {
      return false;
    }
    const DskSurfaceManager::State* state = dsk->state(m_layerSurface->surface);
    return (state != nullptr && state->motion.animating()) || dsk->driven(m_layerSurface->surface);
  }

  bool LayerSurface::tickAnimations(double nowMsec) {
    // A tracker in its interacting phase moves values from input, between ticks.
    if (const DskSurfaceManager* dsk = m_server->dskSurfaces(); dsk != nullptr
        && (dsk->tick(m_layerSurface->surface, static_cast<uint32_t>(nowMsec)) || dsk->driven(m_layerSurface->surface))) {
      applySurfaceMotion();
      applyFadeAlpha();
    }
    const bool ticked = m_fade.tick(nowMsec);
    if (m_scene != nullptr) {
      bindAnimationEffect(&m_scene->tree->node, AnimationEvent::Layers, m_fade);
      bindAnimationEffect(&m_popupTree->node, AnimationEvent::Layers, m_fade);
    }
    if (!ticked) {
      return false;
    }
    applyFadeAlpha();
    if (m_layerSurface != nullptr && m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
      if (Output* out = output()) {
        out->markBlurBackgroundDirty(&m_scene->tree->node);
      }
    }
    return true;
  }

  bool LayerSurface::animatesOn(const Output* out) const { return output() == out; }

  void LayerSurface::applyFadeAlpha() {
    if (m_scene == nullptr) {
      return;
    }
    // Overshooting curves can push this out of range; wlr_scene_buffer_set_opacity asserts opacity is in [0, 1].
    float alpha = m_fade.animating() && effectRegistry().animationEffect(AnimationEvent::Layers) != nullptr
        ? 1.0F
        : std::clamp(static_cast<float>(m_fade.current()), 0.0F, 1.0F);
    const DskSurfaceManager* dsk = m_server->dskSurfaces();
    if (const DskSurfaceManager::State* state = dsk != nullptr ? dsk->state(m_layerSurface->surface) : nullptr) {
      const float opacity =
          std::clamp(static_cast<float>(state->motion.value(SurfaceMotion::Property::Opacity)[0]), 0.0F, 1.0F);
      alpha *= opacity;
      // A fade-in leads with the blur (ease-out cubic), so the backdrop is frosted before the fill is opaque; a
      // fade-out drops the blur ahead of the fill (cubic), or the frost outlasts the content. The fade scales the blur
      // size, which the eye reads on a log scale: a linear fade looks fully frosted until its last frames.
      const bool fadingOut = state->motion.target(SurfaceMotion::Property::Opacity)[0] < opacity;
      const float lead = 1.0F - opacity;
      m_blurFade = fadingOut ? opacity * opacity * opacity : 1.0F - lead * lead * lead;
      m_blur.setFade(m_blurFade);
      for (SurfaceBlur& blur : m_shapeBlurs) {
        blur.setFade(m_blurFade);
      }
    }
    for (auto* tree : {m_scene->tree, m_popupTree}) {
      wlr_scene_node_for_each_buffer(
          &tree->node,
          [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
          },
          &alpha
      );
    }
    if (alpha != m_surfaceAlpha) {
      m_surfaceAlpha = alpha;
      applySurfaceMotion();
    }
  }

  void LayerSurface::applyStroke(wlr_scene_tree* tree, const wlr_box* clip, int radius) {
    const DskSurfaceManager* dsk = m_server->dskSurfaces();
    const DskSurfaceManager::State* state = dsk != nullptr ? dsk->state(m_layerSurface->surface) : nullptr;
    const auto rgba = state != nullptr ? state->motion.value(SurfaceMotion::Property::Stroke) : SurfaceMotion::Value{};
    const float alpha = std::clamp(static_cast<float>(rgba[3]), 0.0F, 1.0F) * m_surfaceAlpha;
    constexpr int kWidth = 1;
    if (tree == nullptr || clip == nullptr || alpha <= 0.0F || clip->width <= 2 * kWidth
        || clip->height <= 2 * kWidth) {
      if (m_stroke != nullptr) {
        wlr_scene_node_set_enabled(&m_stroke->node, false);
      }
      return;
    }
    const float color[4]{
        std::clamp(static_cast<float>(rgba[0]), 0.0F, 1.0F) * alpha,
        std::clamp(static_cast<float>(rgba[1]), 0.0F, 1.0F) * alpha,
        std::clamp(static_cast<float>(rgba[2]), 0.0F, 1.0F) * alpha, alpha
    };
    if (m_stroke == nullptr) {
      m_stroke = wlr_scene_border_create(m_scene->tree, color, color);
      if (m_stroke == nullptr) {
        return;
      }
    }
    wlr_scene_border_set_colors(m_stroke, color, color);
    // The ring lies inside the clip, as the client's border lies inside its sheet.
    const BorderRing ring = makeBorderRing(clip->width - 2 * kWidth, clip->height - 2 * kWidth, radius, kWidth, 0);
    applyBorderGeometry(m_stroke, ring, kWidth, 0);
    wlr_scene_node_set_position(
        &m_stroke->node, tree->node.x + clip->x + kWidth + ring.box.x, tree->node.y + clip->y + kWidth + ring.box.y
    );
    wlr_scene_node_place_above(&m_stroke->node, &tree->node);
    wlr_scene_node_set_enabled(&m_stroke->node, true);
  }

  wlr_scene_tree* LayerSurface::surfaceTree() const {
    if (m_scene == nullptr) {
      return nullptr;
    }
    // wlr_scene_layer_surface_v1_create makes the surface tree the only tree child; blur and shadow are not trees.
    wlr_scene_node* child = nullptr;
    wl_list_for_each(child, &m_scene->tree->children, link) {
      if (child->type == WLR_SCENE_NODE_TREE) {
        return wlr_scene_tree_from_node(child);
      }
    }
    return nullptr;
  }

  void LayerSurface::applySurfaceMotion() {
    const DskSurfaceManager* dsk = m_server->dskSurfaces();
    const DskSurfaceManager::State* state = dsk != nullptr ? dsk->state(m_layerSurface->surface) : nullptr;
    wlr_scene_tree* tree = state != nullptr ? surfaceTree() : nullptr;
    if (tree == nullptr) {
      applyStroke(nullptr, nullptr, 0);
      return;
    }
    using Property = SurfaceMotion::Property;
    const SurfaceMotion& motion = state->motion;
    const double scale = shownScale(motion, m_layerSurface->surface);
    const auto translate = shownTranslate(motion, scale);
    const double width = m_layerSurface->surface->current.width;
    const double height = m_layerSurface->surface->current.height;
    // Scale about the surface center. The scene takes surface units per scene unit, the inverse of the shown size.
    // Input follows: wlr_scene_node_at reports surface-local coordinates through the same scale.
    wlr_scene_subsurface_tree_set_scale(&tree->node, 1.0 / scale);
    wlr_scene_node_set_position(
        &tree->node, static_cast<int>(std::lround(translate[0] + width * (1.0 - scale) / 2.0)),
        static_cast<int>(std::lround(translate[1] + height * (1.0 - scale) / 2.0))
    );

    // The protocol's clip is in surface-local px; the tree clip and corner box are in scene units.
    const auto& clip = motion.value(Property::Clip);
    const wlr_box clipBox{
        static_cast<int>(std::lround(clip[0] * scale)), static_cast<int>(std::lround(clip[1] * scale)),
        static_cast<int>(std::lround(clip[2] * scale)), static_cast<int>(std::lround(clip[3] * scale))
    };
    const bool clipped = clipBox.width > 0 && clipBox.height > 0;
    const int radius = static_cast<int>(std::lround(motion.value(Property::Radius)[0] * scale));
    // With a stroke the surface stops 1 px inside the clip, so the ring shows the blur through it as the client's own
    // hairline does, instead of lying on the opaque fill. The tree clip cuts the corner mask's 1 px outer fringe.
    const bool stroked = clipped && motion.value(Property::Stroke)[3] > 0.0 && clipBox.width > 2 && clipBox.height > 2;
    const wlr_box fillBox =
        stroked ? wlr_box{clipBox.x + 1, clipBox.y + 1, clipBox.width - 2, clipBox.height - 2} : clipBox;
    wlr_scene_tree_set_clip(tree, clipped ? &fillBox : nullptr);
    struct Corners {
      int radius;
      const wlr_box* box;
    } corners{stroked ? std::max(radius - 1, 0) : radius, clipped ? &fillBox : nullptr};
    wlr_scene_node_for_each_buffer(
        &tree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          const auto* c = static_cast<const Corners*>(data);
          wlr_scene_buffer_set_corner_radius(buffer, c->radius);
          wlr_scene_buffer_set_corner_box(buffer, c->box);
        },
        &corners
    );
    applyStroke(tree, clipped ? &clipBox : nullptr, radius);

    updateBlur();
    const double level = std::clamp(motion.value(Property::Elevation)[0], 0.0, 2.0);
    if (level < 0.005) {
      m_shadow.hide();
      return;
    }
    // Level 0..1 grows the float plane (e2) from nothing at constant blur; 1..2 blends float into over (e3).
    const Tokens::Elevation& from = Tokens::elevationE2;
    const Tokens::Elevation& to = Tokens::elevationE3;
    const auto mix = [](double a, double b, double t) { return a + (b - a) * t; };
    const auto round = [](double v) { return static_cast<int>(std::lround(v)); };
    const double up = std::clamp(level - 1.0, 0.0, 1.0);
    const double base = std::min(level, 1.0);
    const auto opacity = [&](double a, double b) { return static_cast<float>(mix(a * base, b, up)); };
    m_shadow.setStyle(ShadowStyle{
        .softness = round(mix(from.keyBlur, to.keyBlur, up)),
        .offsetX = 0,
        .offsetY = round(mix(from.keyY * base, to.keyY, up)),
        .opacity = {.light = opacity(from.keyLight, to.keyLight), .dark = opacity(from.keyDark, to.keyDark)},
        .contactSoftness = round(mix(from.contactBlur, to.contactBlur, up)),
        .contactOffsetY = round(mix(from.contactY * base, to.contactY, up)),
        .contactOpacity = {.light = opacity(from.contactLight, to.contactLight),
                           .dark = opacity(from.contactDark, to.contactDark)},
    });
    // The shadow follows the visible rect: the clip when set, else the whole surface, after translate and scale.
    const wlr_box shape = clipped ? clipBox
                                  : wlr_box{0, 0, static_cast<int>(std::lround(width * scale)),
                                            static_cast<int>(std::lround(height * scale))};
    m_shadow.update(
        m_scene->tree, shape.width, shape.height, 0, corners.radius, 0, tree->node.x + shape.x, tree->node.y + shape.y
    );
  }

  void LayerSurface::beginCloseAnimation(wlr_scene_tree* tree) {
    Output* out = output();
    const auto& animation = config().animation;
    const auto& layers = animation.layers;
    if (out == nullptr || m_scene == nullptr || !animation.enabled || !layers.enabled) {
      return;
    }

    const uint32_t layer = m_layerSurface->current.layer;
    wlr_scene_tree* parentTree = tree == m_popupTree ? out->layerPopupTree(layer) : out->layerTree(layer);
    wlr_scene_tree* snap = wlr_scene_tree_create(parentTree);
    if (snap == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&snap->node, tree->node.x, tree->node.y);

    struct CopyCtx {
      wlr_scene_tree* snap;
      int rootX;
      int rootY;
      int buffersCopied;
    };
    CopyCtx ctx{snap, tree->node.x, tree->node.y, 0};
    wlr_scene_node_for_each_buffer(
        &tree->node,
        [](wlr_scene_buffer* src, int sx, int sy, void* data) {
          auto* c = static_cast<CopyCtx*>(data);
          if (src->buffer == nullptr || !src->node.enabled) {
            return;
          }
          wlr_scene_buffer* copy = wlr_scene_buffer_create(c->snap, src->buffer);
          if (copy == nullptr) {
            return;
          }
          wlr_scene_node_set_position(&copy->node, sx - c->rootX, sy - c->rootY);
          if (src->dst_width > 0 && src->dst_height > 0) {
            wlr_scene_buffer_set_dest_size(copy, src->dst_width, src->dst_height);
          }
          if (src->src_box.width > 0 && src->src_box.height > 0) {
            wlr_scene_buffer_set_source_box(copy, &src->src_box);
          }
          wlr_scene_buffer_set_transform(copy, src->transform);
          wlr_scene_buffer_set_corner_radii(copy, src->corners);
          wlr_scene_buffer_set_corner_box(copy, &src->corner_box);
          wlr_scene_buffer_set_opacity(copy, src->opacity);
          wlr_scene_buffer_copy_color(copy, src);
          ++c->buffersCopied;
        },
        &ctx
    );

    if (ctx.buffersCopied == 0) {
      wlr_scene_node_destroy(&snap->node);
      return;
    }

    wlr_scene_node_copy_animations_for_snapshot(&snap->node, &tree->node);
    (void)m_server->animateCloseSnapshot(
        out, snap, snap, {}, {},
        Server::CloseSnapshotOverrides{
            .durationMs = layers.durationMs, .curve = layers.curve, .style = "fade", .event = AnimationEvent::Layers
        }
    );
    wlr_output_schedule_frame(out->wlr());
  }

  Output* LayerSurface::output() const {
    if (m_layerSurface == nullptr || m_layerSurface->output == nullptr) {
      return nullptr;
    }
    return static_cast<Output*>(m_layerSurface->output->data);
  }

  void LayerSurface::notifyOutputScale() {
    Output* out = output();
    if (out == nullptr || m_layerSurface == nullptr) {
      return;
    }
    wlr_layer_surface_v1_for_each_surface(m_layerSurface, &Output::notifySurfaceScaleIter, out);
    wlr_layer_surface_v1_for_each_popup_surface(m_layerSurface, &Output::notifySurfaceScaleIter, out);
  }

  bool LayerSurface::exclusiveKeyboard() const {
    return acceptsKeyboard()
        && m_layerSurface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
  }

  bool LayerSurface::acceptsKeyboard() const {
    if (!m_mapped || m_layerSurface == nullptr) {
      return false;
    }
    if (m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
      if (Output* out = output(); out != nullptr && out->hasFullscreenView()) {
        return false;
      }
    }
    const auto interactivity = m_layerSurface->current.keyboard_interactive;
    return interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
        || interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
  }

  bool LayerSurface::hasKeyboardFocus() const {
    return m_layerSurface != nullptr && fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface) == this;
  }

  void LayerSurface::reparentToLayer(uint32_t layer) {
    Output* out = output();
    if (out == nullptr || m_scene == nullptr) {
      return;
    }
    // Leaving or entering the watched background tree sends no update to its listener.
    out->markBlurBackgroundDirty();
    wlr_scene_node_reparent(&m_popupTree->node, out->layerPopupTree(layer));
    wlr_scene_node_reparent(&m_scene->tree->node, out->layerTree(layer));
  }

  void LayerSurface::syncPopupTree() {
    wlr_scene_node_set_position(&m_popupTree->node, m_scene->tree->node.x, m_scene->tree->node.y);
    wlr_scene_node_set_enabled(&m_popupTree->node, m_mapped && m_scene->tree->node.enabled);
  }

  void LayerSurface::onSceneUpdate(wl_listener* listener, void* data) {
    LayerSurface* self = wl_container_of(listener, self, m_sceneUpdate); // NOLINT(modernize-use-auto)
    if (data == &self->m_scene->tree->node) {
      self->syncPopupTree();
    }
  }

  void LayerSurface::onSceneDestroy(wl_listener* listener, void* /*data*/) {
    LayerSurface* self = wl_container_of(listener, self, m_sceneDestroy); // NOLINT(modernize-use-auto)
    wl_list_remove(&self->m_sceneUpdate.link);
    wl_list_remove(&self->m_sceneDestroy.link);
    self->m_sceneUpdate.link.next = nullptr;
    self->m_sceneDestroy.link.next = nullptr;
    self->m_shadow.reset();
    self->m_stroke = nullptr; // a child of the destroyed scene tree
    wlr_scene_node_destroy(&self->m_popupTree->node);
    self->m_popupTree = nullptr;
  }

  void LayerSurface::focus() {
    if (m_server->sessionLocked() || !acceptsKeyboard()) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* surface = m_layerSurface->surface;
    // Layers take the seat: drop every window's activated/border so none stay "focused".
    m_server->deactivateViews(nullptr);
    if (seat->keyboard_state.focused_surface == surface) {
      return;
    }

    // End any keyboard grab (e.g. xdg_popup) that would intercept the enter and block focus from reaching this layer
    // surface. Pointer/touch grabs remain intact so the popup stays visually mapped.
    wlr_seat_keyboard_end_grab(seat);

    // Give the layer seat keyboard focus so clients (e.g. Noctalia) receive Escape.
    m_server->notifyKeyboardEnter(surface);
    m_server->refreshOutputPolicies();
  }

  void LayerSurface::focusForPopupGrab() {
    if (m_server->sessionLocked() || !acceptsKeyboard() || hasKeyboardFocus()) {
      return;
    }
    if (const LayerSurface* exclusive = m_server->exclusiveKeyboardLayer(); exclusive != nullptr && exclusive != this) {
      return;
    }

    m_server->deactivateViews(nullptr);
    // focus() would end the popup's keyboard grab, and wlroots answers that by dismissing the popup. Enter past it.
    m_server->notifyKeyboardEnter(m_layerSurface->surface, Server::KeyboardGrab::Bypass);
    m_server->refreshOutputPolicies();
  }

  SurfaceBlurOptions LayerSurface::blurOptions() const {
    return SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(m_rule.ignoreAlpha.value_or(0.0)),
        .enabled = m_rule.blur.value_or(false),
        .optimized = m_rule.optimized,
    };
  }

  SurfaceBlurOptions LayerSurface::popupBlurOptions() const {
    return SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(m_rule.ignoreAlpha.value_or(0.0)),
        .enabled = m_rule.blurPopups.value_or(false),
        .optimized = m_rule.optimized,
    };
  }

  void LayerSurface::unconstrainPopup(wlr_xdg_popup* popup) {
    Output* out = output();
    if (out == nullptr || m_scene == nullptr) {
      return;
    }

    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(m_layerSurface->output, &width, &height);

    // wlroots expects the box in the root layer surface coordinate system and
    // translates it through every ancestor popup itself.
    const wlr_box box = {
        .x = -m_scene->tree->node.x,
        .y = -m_scene->tree->node.y,
        .width = width,
        .height = height,
    };
    wlr_xdg_popup_unconstrain_from_box(popup, &box);
  }

  bool LayerSurface::pollBackdrop() {
    float luminance = 0.0F;
    if (m_blur.pollBackdrop(&luminance)) {
      // Hysteresis: a backdrop near the threshold keeps its tone instead of flickering.
      m_backdropDark = m_backdropDark ? luminance < 0.45F : luminance < 0.35F;
      if (const DskSurfaceManager* dsk = m_server->dskSurfaces()) {
        dsk->sendBackdrop(m_layerSurface->surface, m_backdropDark);
      }
    }
    return m_blur.backdropPending();
  }

  void LayerSurface::updateBlur() {
    const wlr_box box{0, 0, m_layerSurface->surface->current.width, m_layerSurface->surface->current.height};
    SurfaceBlurOptions options = blurOptions();
    // A client blur region (ext-background-effect-v1) turns blur on unless a layer rule turns it off; the blur draws
    // only inside the region.
    std::optional<wlr_box> clip;
    const BackgroundEffectManager* effects = m_server->backgroundEffects();
    const pixman_region32_t* region = effects != nullptr ? effects->blurRegion(m_layerSurface->surface) : nullptr;
    // The blur is not under the surface tree, so a dsk_surface_v1 clip and its corners are applied here too.
    std::optional<wlr_box> dskClip;
    // The surface tree moves with the dsk translate; the blur node is its sibling, so it moves by the same offset.
    wlr_box nodeBox = box;
    fx_corner_radii corners = corner_radii_none();
    double motionScale = 1.0;
    const DskSurfaceManager* dsk = m_server->dskSurfaces();
    const DskSurfaceManager::State* state = dsk != nullptr ? dsk->state(m_layerSurface->surface) : nullptr;
    m_liquid = state != nullptr
        ? static_cast<float>(std::clamp(state->motion.value(SurfaceMotion::Property::Liquid)[0], 0.0, 1.0))
        : 0.0F;
    // Liquid value [1]: the specular rim alone, for frosted sheets of the same glass family.
    m_rim = state != nullptr
        ? static_cast<float>(std::clamp(state->motion.value(SurfaceMotion::Property::Liquid)[1], 0.0, 1.0))
        : 0.0F;
    m_blur.setLiquid(m_liquid);
    m_blur.setRim(m_rim);
    m_blur.setBackdropSampling(m_liquid > 0.0F);
    if (state != nullptr) {
      const auto& c = state->motion.value(SurfaceMotion::Property::Clip);
      const double scale = shownScale(state->motion, m_layerSurface->surface);
      const wlr_box scaled{
          static_cast<int>(std::lround(c[0] * scale)), static_cast<int>(std::lround(c[1] * scale)),
          static_cast<int>(std::lround(c[2] * scale)), static_cast<int>(std::lround(c[3] * scale))
      };
      const auto translate = shownTranslate(state->motion, scale);
      nodeBox.x = static_cast<int>(std::lround(translate[0] + box.width * (1.0 - scale) / 2.0));
      nodeBox.y = static_cast<int>(std::lround(translate[1] + box.height * (1.0 - scale) / 2.0));
      // The surface tree shows the buffer at this scale, so the blur node and the client region shrink with it;
      // a full-size blur would draw a second, offset shape during a scale open or close.
      nodeBox.width = static_cast<int>(std::lround(box.width * scale));
      nodeBox.height = static_cast<int>(std::lround(box.height * scale));
      motionScale = scale;
      if (scaled.width > 0 && scaled.height > 0) {
        dskClip = scaled;
        dskClip->x += nodeBox.x;
        dskClip->y += nodeBox.y;
        const double radius = state->motion.value(SurfaceMotion::Property::Radius)[0] * scale;
        corners = corner_radii_all(radius > 0.0 ? static_cast<int>(std::ceil(radius)) + kBlurCornerSlack : 0);
      }
    }
    // A client blur region is the exact shape, so the alpha test of blur_ignore_alpha must not cut it: a clear liquid
    // tint (alpha 0.34) would lose its whole blur.
    if (region != nullptr && m_rule.blur.value_or(true)) {
      options.ignoreAlpha = 0.0F;
    }
    std::vector<RoundedShape> shapes;
    std::vector<wlr_box> shapeClips;
    if (region != nullptr && m_rule.blur.value_or(true) && dskClip && pixman_region32_not_empty(region)) {
      // The clip and its corner radius are the exact shape; a client region is a pixel staircase that shows past the
      // anti-aliased corner.
      region = nullptr;
      clip = dskClip;
      options.enabled = true;
    } else if (region != nullptr && m_rule.blur.value_or(true)) {
      shapes = roundedShapesOf(region);
      if (!shapes.empty()) {
        const auto scaled = [motionScale](int v) { return static_cast<int>(std::lround(v * motionScale)); };
        for (const RoundedShape& shape : shapes) {
          shapeClips.push_back(wlr_box{
              nodeBox.x + scaled(shape.box.x), nodeBox.y + scaled(shape.box.y), scaled(shape.box.width),
              scaled(shape.box.height)
          });
        }
        clip = shapeClips.front();
        corners = corner_radii_all(scaled(shapes.front().radius) + kBlurCornerSlack);
        region = nullptr;
      } else if (!pixman_region32_not_empty(region)) {
        // The blur keeps the full surface box: the alpha mask is stretched over the blur node, so a node shrunk to
        // the region extents would misalign it. The region clips the blur instead.
        clip = wlr_box{0, 0, 0, 0};
      }
      options.enabled = true;
    } else {
      region = nullptr;
      clip = dskClip;
    }
    pixman_region32_t scaledRegion;
    pixman_region32_init(&scaledRegion);
    if (region != nullptr && motionScale != 1.0) {
      int count = 0;
      const pixman_box32_t* rects = pixman_region32_rectangles(region, &count);
      std::vector<pixman_box32_t> scaled(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        scaled[static_cast<std::size_t>(i)] = {
            static_cast<int32_t>(std::lround(rects[i].x1 * motionScale)),
            static_cast<int32_t>(std::lround(rects[i].y1 * motionScale)),
            static_cast<int32_t>(std::lround(rects[i].x2 * motionScale)),
            static_cast<int32_t>(std::lround(rects[i].y2 * motionScale)),
        };
      }
      pixman_region32_fini(&scaledRegion);
      pixman_region32_init_rects(&scaledRegion, scaled.data(), count);
      region = &scaledRegion;
    }
    m_blur.update(
        m_scene->tree, m_layerSurface->surface, nodeBox, box, corners, clip ? &*clip : nullptr, options, 1.0F, nullptr,
        region
    );
    pixman_region32_fini(&scaledRegion);
    if (m_shapeBlurs.size() + 1 < shapes.size()) {
      m_shapeBlurs.resize(shapes.size() - 1);
    }
    for (std::size_t i = 0; i < m_shapeBlurs.size(); ++i) {
      SurfaceBlur& blur = m_shapeBlurs[i];
      if (i + 1 >= shapes.size()) {
        blur.hide();
        continue;
      }
      blur.setLiquid(m_liquid);
      blur.setRim(m_rim);
      blur.setFade(m_blurFade);
      blur.update(
          m_scene->tree, m_layerSurface->surface, nodeBox, box,
          corner_radii_all(static_cast<int>(std::lround(shapes[i + 1].radius * motionScale)) + kBlurCornerSlack), &shapeClips[i + 1],
          options
      );
    }
  }

  void LayerSurface::applyConfig() {
    m_rule = resolveLayerRules(
        config(), ruleText(m_layerSurface->namespace_), layerOf(m_layerSurface->current.layer), m_server->uptimeMs()
    );
    updateBlur();
  }

  void LayerSurface::refreshStartupRuleEffects() {
    m_rule = resolveLayerRules(
        config(), ruleText(m_layerSurface->namespace_), layerOf(m_layerSurface->current.layer), m_server->uptimeMs()
    );
    if (m_mapped) {
      updateBlur();
    }
  }

  void LayerSurface::onMap(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void LayerSurface::onUnmap(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
  }

  void LayerSurface::onCommit(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_commit);
    self->handleCommit();
  }

  void LayerSurface::onDestroy(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void LayerSurface::onNewPopup(wl_listener* listener, void* data) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_newPopup);
    self->handleNewPopup(data);
  }

  void LayerSurface::handleMap() {
    m_mapped = true;
    syncPopupTree();
    m_server->scheduleIpcLayersEvent();
    m_server->updateIdleInhibit();
    if (Output* out = output()) {
      out->markDirty(Dirty::LayerArrange);
      if (m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        out->markBlurBackgroundDirty(&m_scene->tree->node);
      }
    }
    // Protocol requires focus for exclusive. On-demand layers also take it as they map: launchers,
    // quick terminals and panels expect the keyboard right away.
    // A click or a focus action moves focus off them again, unlike an exclusive layer.
    if (acceptsKeyboard()) {
      focus();
    }
    updateBlur();
    notifyDesktopStack();

    const auto& animation = config().animation;
    const auto& layers = animation.layers;
    if (animation.enabled && layers.enabled) {
      m_fade.snap(0.0);
      applyFadeAlpha();
      m_fade.retarget(1.0, layers.durationMs, layers.curve);
      if (Output* out = output()) {
        wlr_output_schedule_frame(out->wlr());
      }
    } else {
      m_fade.snap(1.0);
      applyFadeAlpha();
    }
  }

  void LayerSurface::handleUnmap() {
    // Snapshot before any other unmap bookkeeping runs: the live buffer is still valid here.
    beginCloseAnimation(m_scene->tree);
    beginCloseAnimation(m_popupTree);
    wlr_scene_node_clear_animations(&m_scene->tree->node);
    wlr_scene_node_clear_animations(&m_popupTree->node);
    const bool hadFocus = hasKeyboardFocus();
    m_mapped = false;
    syncPopupTree();
    m_server->scheduleIpcLayersEvent();
    m_server->updateIdleInhibit();
    m_blur.hide();
    for (SurfaceBlur& blur : m_shapeBlurs) {
      blur.hide();
    }
    m_fade.snap(1.0);
    // Avoid sending configures while unmapping (wrong serial / client abort).
    m_arrangingOut = true;
    if (Output* out = output()) {
      out->markDirty(Dirty::LayerArrange);
      if (m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        out->markBlurBackgroundDirty(&m_scene->tree->node);
      }
    }
    m_arrangingOut = false;
    notifyDesktopStack();
    if (hadFocus) {
      m_server->refocus();
    }
  }

  void LayerSurface::notifyDesktopStack() {
    const uint32_t layer = m_layerSurface->current.layer;
    if (layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND && layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
      return;
    }
    if (Overview* overview = m_server->overview()) {
      overview->onDesktopLayerChanged(output());
    }
  }

  void LayerSurface::handleCommit() {
    updateBlur();
    // dsk_surface_v1 state applied on this commit: show set values now, and run the frame loop for animations.
    if (const DskSurfaceManager* dsk = m_server->dskSurfaces();
        dsk != nullptr && dsk->state(m_layerSurface->surface) != nullptr) {
      applySurfaceMotion();
      applyFadeAlpha();
      if (Output* out = hasActiveAnimations() ? output() : nullptr) {
        wlr_output_schedule_frame(out->wlr());
      }
    }
    if (m_layerSurface->initial_commit) {
      if (Output* out = output()) {
        out->arrangeLayers();
        out->markDirty(Dirty::LayerArrange);
      }
      return;
    }

    if ((m_layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) != 0) {
      m_rule = resolveLayerRules(
          config(), ruleText(m_layerSurface->namespace_), layerOf(m_layerSurface->current.layer), m_server->uptimeMs()
      );
      updateBlur();
      reparentToLayer(m_layerSurface->current.layer);
      notifyDesktopStack();
    }

    if ((m_layerSurface->current.committed
         & (WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY | WLR_LAYER_SURFACE_V1_STATE_LAYER))
        != 0) {
      if (exclusiveKeyboard()) {
        focus();
      } else if (hasKeyboardFocus() && !acceptsKeyboard()) {
        m_server->refocus();
      }
    }

    if (m_layerSurface->current.committed != 0) {
      if (Output* out = output()) {
        out->markDirty(Dirty::LayerArrange);
      }
    }
  }

  void LayerSurface::handleDestroy() {
    Server* server = m_server;
    wlr_output* wlrOutput = m_layerSurface != nullptr ? m_layerSurface->output : nullptr;
    kLog.debug("destroy layer surface output={}", wlrOutput != nullptr ? wlrOutput->name : "(none)");

    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_newPopup.link);
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_commit.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_newPopup.link.next = nullptr;

    // m_scene is already freed: wlr_scene_layer_surface_v1_create registers its own destroy listener on
    // m_layerSurface->events.destroy before ours, and that handler destroys the tree and frees the helper.
    m_scene = nullptr;
    if (m_layerSurface != nullptr) {
      m_layerSurface->data = nullptr;
    }
    // removeLayerSurface deletes this. Arrange after erase, never via this->m_server.
    server->removeLayerSurface(this, wlrOutput);
  }

  void LayerSurface::handleNewPopup(void* data) {
    auto* popup = static_cast<wlr_xdg_popup*>(data);
    // The popup tree shares the surface origin and stacks above its shell layer's regular surfaces.
    wlr_scene_node_raise_to_top(&m_popupTree->node);
    new Popup(popup, m_popupTree);
  }

} // namespace andromeda

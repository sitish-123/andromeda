#include "render/render_context.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "core/ui_phase.h"
#include "render/backend/render_backend.h"
#include "render/core/lockscreen_transition_types.h"
#include "render/core/texture_handle.h"
#include "render/core/texture_manager.h"
#include "render/core/wallpaper_types.h"
#include "render/gl_shared_context.h"
#include "render/render_target.h"
#include "render/scene/audio_spectrum_node.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/effect_node.h"
#include "render/scene/fancy_audio_visualizer_node.h"
#include "render/scene/glyph_node.h"
#include "render/scene/graph_node.h"
#include "render/scene/image_node.h"
#include "render/scene/lockscreen_transition_node.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/screen_corner_node.h"
#include "render/scene/spinner_node.h"
#include "render/scene/text_node.h"
#include "render/scene/wallpaper_node.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("render");
  constexpr float kSlowRenderOperationDebugMs = 50.0F;
  constexpr float kSlowRenderOperationWarnMs = 1000.0F;
  constexpr float kPaintCullSlack = 16.0F;
  // Damage rects merge into one repaint box when the box is at most this many times their area.
  constexpr float kSingleWalkMaxGrowth = 1.5F;

} // namespace

namespace {

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename... Args> void logSlowRenderOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowRenderOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowRenderOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

  RenderScissor
  scissorForClip(float sw, float sh, float bw, float bh, float left, float top, float right, float bottom) {
    const float scaleX = sw > 0.0F ? bw / sw : 1.0F;
    const float scaleY = sh > 0.0F ? bh / sh : 1.0F;
    const auto clampX = [bw](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bw)));
    };
    const auto clampY = [bh](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bh)));
    };

    // Round clip edges independently in buffer space. Rounding the size after
    // flooring the origin can drop the final column/row for fractional origins.
    const std::int32_t x0 = clampX(static_cast<std::int32_t>(std::floor(left * scaleX)));
    const std::int32_t x1 = clampX(static_cast<std::int32_t>(std::ceil(right * scaleX)));
    const std::int32_t y0 = clampY(static_cast<std::int32_t>(std::floor((sh - bottom) * scaleY)));
    const std::int32_t y1 = clampY(static_cast<std::int32_t>(std::ceil((sh - top) * scaleY)));
    return RenderScissor{
        .x = x0,
        .y = y0,
        .width = std::max(std::int32_t{0}, x1 - x0),
        .height = std::max(std::int32_t{0}, y1 - y0),
    };
  }

  Mat3 nodeLocalTransform(const Node* node) {
    const float cx = node->transformOriginX();
    const float cy = node->transformOriginY();
    return Mat3::translation(node->x(), node->y())
        * Mat3::translation(cx, cy)
        * Mat3::rotation(node->rotation())
        * Mat3::scale(node->scaleX(), node->scaleY())
        * Mat3::translation(-cx, -cy);
  }

  std::optional<DamageRect> paintBounds(const Node* node, const Mat3& world) {
    if (node->type() == NodeType::Base) {
      return std::nullopt;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(node, world, left, top, right, bottom);
    float pad = kPaintCullSlack;
    if (node->type() == NodeType::Text) {
      const auto* text = static_cast<const TextNode*>(node);
      // A text node has no size: it paints its measured extent around the baseline origin.
      const DamageRect& e = text->paintExtent();
      for (const Vec2 corner :
           {world.transformPoint(e.x0, e.y0), world.transformPoint(e.x1, e.y0), world.transformPoint(e.x1, e.y1),
            world.transformPoint(e.x0, e.y1)}) {
        left = std::min(left, corner.x);
        top = std::min(top, corner.y);
        right = std::max(right, corner.x);
        bottom = std::max(bottom, corner.y);
      }
      pad += std::max(std::abs(text->shadowOffsetX()), std::abs(text->shadowOffsetY()));
    } else if (node->type() == NodeType::Rect) {
      // Same overhang as the rect quad (RectProgram::draw: border + softness + 2 px).
      const auto& style = static_cast<const RectNode*>(node)->style();
      pad = std::max(pad, style.borderWidth + style.softness + 2.0F);
    } else if (node->type() == NodeType::Glyph) {
      const auto* glyph = static_cast<const GlyphNode*>(node);
      pad += std::max(std::abs(glyph->shadowOffsetX()), std::abs(glyph->shadowOffsetY()));
    }
    // The pads above are local units; a scaled or rotated node moves its overhang up to its largest axis scale.
    const Vec2 o = world.transformPoint(0.0F, 0.0F);
    const Vec2 ax = world.transformPoint(1.0F, 0.0F);
    const Vec2 ay = world.transformPoint(0.0F, 1.0F);
    pad *= std::max(1.0F, std::max(std::hypot(ax.x - o.x, ax.y - o.y), std::hypot(ay.x - o.x, ay.y - o.y)));
    return DamageRect{left - pad, top - pad, right + pad, bottom + pad};
  }

  // Adds what changed under `node` since the last frame and records the new painted bounds. A node whose own paint
  // changed (position, opacity, style) repaints its whole subtree. False when the frame needs a full repaint.
  bool collectDamage(Node* node, const Mat3& parentTransform, bool subtreeChanged, DamageRegion& out) {
    if (!subtreeChanged && !node->paintDirty() && !node->layoutDirty()) {
      return true;
    }
    if (!node->visible()) {
      Node::takePaintedBounds(node, out);
      return true;
    }
    if (node->type() == NodeType::RenderProxy) {
      return false;
    }
    const Mat3 world = parentTransform * nodeLocalTransform(node);
    const std::optional<DamageRect> bounds = paintBounds(node, world);
    const bool whole = subtreeChanged || node->selfPaintDirty();
    if (whole || node->paintedBounds() != bounds) {
      if (node->paintedBounds()) {
        out.add(*node->paintedBounds());
      }
      if (bounds) {
        out.add(*bounds);
      }
      node->setPaintedBounds(bounds);
    }
    for (const auto& child : node->children()) {
      if (!collectDamage(child.get(), world, whole, out)) {
        return false;
      }
    }
    return true;
  }

} // namespace

RenderContext::RenderContext() = default;

RenderContext::~RenderContext() { cleanup(); }

void RenderContext::initialize(GlSharedContext& shared) {
  cleanup();
  m_shared = &shared;
  m_backend = createDefaultRenderBackend();
  m_backend->initialize(shared);

  // Pango handles font fallback via Fontconfig automatically — no explicit chain.
  m_backend->textureManager().probeExtensions();
  m_textRenderer.initialize(m_backend.get(), &m_backend->textureManager());
  m_glyphRenderer.initialize(
      paths::assetPath("fonts/noctalia-tabler.ttf").string(), m_backend.get(), &m_backend->textureManager()
  );
  m_textRenderer.setFontFamily(m_textFontFamily);
  m_textRenderer.setBaseDirection(m_textBaseDirRtl);
  ++m_textMetricsGeneration;
  m_graphicsResetPending = false;
}

void RenderContext::restoreAfterGraphicsReset(GlSharedContext& shared) {
  if (m_backend == nullptr) {
    throw std::runtime_error("cannot restore an uninitialized render context");
  }
  m_shared = &shared;
  m_backend->initialize(shared);
  m_backend->textureManager().probeExtensions();
  invalidateGpuResourcesNextFrame();
}

void RenderContext::prepareForGraphicsReset() {
  if (m_backend == nullptr) {
    return;
  }

  m_textRenderer.abandonGlyphTextures();
  m_glyphRenderer.abandonGlyphTextures();
  m_backend->abandonAfterGraphicsReset();
}

bool RenderContext::makeCurrentNoSurface() {
  if (m_backend == nullptr || m_graphicsResetPending) {
    return false;
  }
  return m_backend->makeCurrentNoSurface();
}

bool RenderContext::makeCurrent(RenderTarget& target) {
  return m_backend != nullptr && !m_graphicsResetPending && m_backend->makeCurrent(target);
}

bool RenderContext::readPixels(RenderTarget& target, std::span<std::uint8_t> rgba) {
  return m_backend != nullptr && !m_graphicsResetPending && m_backend->readPixels(target, rgba);
}

void RenderContext::setTextFontFamily(std::string family) {
  if (family.empty()) {
    family = "sans-serif";
  }
  if (m_textFontFamily == family) {
    return;
  }
  makeCurrentNoSurface();
  m_textFontFamily = std::move(family);
  m_textRenderer.setFontFamily(m_textFontFamily);
  ++m_textMetricsGeneration;
}

void RenderContext::setTextBaseDirection(bool rtl) {
  if (m_textBaseDirRtl == rtl) {
    return;
  }
  makeCurrentNoSurface();
  m_textBaseDirRtl = rtl;
  m_textRenderer.setBaseDirection(rtl);
  ++m_textMetricsGeneration;
}

void RenderContext::notifyFontConfigChanged() {
  m_textRenderer.notifyFontConfigChanged();
  ++m_textMetricsGeneration;
}

bool RenderContext::renderScene(RenderTarget& target, Node* sceneRoot, const WallpaperMaskDrawParams* wallpaperMask) {
  if (m_backend == nullptr || m_graphicsResetPending) {
    return false;
  }
  const auto totalStart = std::chrono::steady_clock::now();
  const auto sw = static_cast<float>(target.logicalWidth());
  const auto sh = static_cast<float>(target.logicalHeight());
  const auto bw = static_cast<float>(target.bufferWidth());
  const auto bh = static_cast<float>(target.bufferHeight());
  const DamageRect surfaceRect{0.0F, 0.0F, sw, sh};

  // A full repaint is needed when the buffer changed, GPU resources or glyphs were rebuilt, or the wallpaper mask
  // changed; an unchanged mask is redrawn inside each repainted rect.
  // NOCTALIA_FULL_REDRAW turns damage tracking off, to rule it out when a surface shows stale pixels.
  static const bool forceFull = std::getenv("NOCTALIA_FULL_REDRAW") != nullptr;
  const bool damageReset = target.takeDamageReset(sceneRoot);
  const bool maskChanged = target.takeWallpaperMaskChange(wallpaperMask);
  // Why this frame repaints everything (NOCTALIA_PROFILE frame stats); the first reason found wins.
  const char* fullReason = forceFull ? "env"
      : damageReset                  ? "reset"
      : sceneRoot == nullptr         ? "no-root"
      : maskChanged                  ? "wallpaper-mask"
      : m_glyphTexturesDirty         ? "glyphs"
      : (m_gpuResourceGeneration != 0 && sceneRoot->gpuResourceGeneration() != m_gpuResourceGeneration) ? "gpu-reset"
                                     : nullptr;
  bool full = fullReason != nullptr;
  DamageRegion damage;
  if (sceneRoot != nullptr) {
    damage = sceneRoot->takePendingDamage();
    if (!collectDamage(sceneRoot, Mat3::identity(), false, damage) && !full) {
      full = true;
      fullReason = "render-proxy";
    }
  }
  damage.clipTo(surfaceRect);
  if (!full && damage.empty()) {
    return false;
  }

  if (!m_backend->beginFrame(target)) {
    // Collected damage is lost with this frame: make the next one a full repaint.
    (void)target.takeDamageReset(nullptr);
    return false;
  }
  const float renderScale = target.contentScale();
  std::optional<DamageRegion> repaint;
  const int bufferAge = full ? -1 : m_backend->bufferAge(target);
  if (!full) {
    repaint = target.damageHistory().regionFor(bufferAge, damage);
    if (!repaint) {
      fullReason = "buffer-age";
    } else {
      // Each rect costs a full scene walk; a few extra pixels cost less.
      repaint->collapseIfCompact(kSingleWalkMaxGrowth);
    }
  }
  if (full) {
    damage.clear();
    damage.add(surfaceRect);
  }
  target.damageHistory().push(damage);

  if (sceneRoot != nullptr
      && m_gpuResourceGeneration != 0
      && sceneRoot->gpuResourceGeneration() != m_gpuResourceGeneration) {
    sceneRoot->invalidateGpuResources(target.renderer(), m_gpuResourceGeneration);
  }

  if (m_glyphTexturesDirty) {
    m_textRenderer.invalidateGlyphTextures();
    m_glyphRenderer.invalidateGlyphTextures();
    m_glyphTexturesDirty = false;
  }

  const auto drawStart = std::chrono::steady_clock::now();
  m_statDrawn = 0;
  m_statCulled = 0;
  std::vector<DamageRect> bufferDamage;
  {
    UiPhaseScope renderPhase(UiPhase::Render);
    if (!repaint) {
      m_backend->clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
      if (sceneRoot != nullptr) {
        renderNode(
            renderScale, sceneRoot, Mat3::identity(), 1.0F, sw, sh, bw, bh, 0.0F, 0.0F, sw, sh, false, false, false
        );
      }
    } else {
      const float sx = sw > 0.0F ? bw / sw : 1.0F;
      const float sy = sh > 0.0F ? bh / sh : 1.0F;
      for (const DamageRect& rect : repaint->rects()) {
        const DamageRect buffer{
            std::floor(rect.x0 * sx), std::floor(rect.y0 * sy), std::ceil(rect.x1 * sx), std::ceil(rect.y1 * sy)
        };
        bufferDamage.push_back(buffer);
        m_backend->setScissor(scissorForClip(sw, sh, bw, bh, rect.x0, rect.y0, rect.x1, rect.y1));
        m_backend->setBlendMode(RenderBlendMode::PremultipliedAlpha);
        m_backend->clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
        renderNode(
            renderScale, sceneRoot, Mat3::identity(), 1.0F, sw, sh, bw, bh, rect.x0, rect.y0, rect.x1, rect.y1, true,
            false, false
        );
        // The mask cuts the repainted pixels only; pixels outside the rect keep their earlier cut.
        if (wallpaperMask != nullptr && wallpaperMask->texture != 0) {
          m_backend->setScissor(scissorForClip(sw, sh, bw, bh, rect.x0, rect.y0, rect.x1, rect.y1));
          m_backend->setBlendMode(RenderBlendMode::DestinationOut);
          m_backend->drawWallpaperMask(*wallpaperMask);
        }
      }
    }
    if (!repaint && wallpaperMask != nullptr && wallpaperMask->texture != 0) {
      m_backend->disableScissor();
      m_backend->setBlendMode(RenderBlendMode::DestinationOut);
      m_backend->drawWallpaperMask(*wallpaperMask);
    }
  }
  float ms = elapsedSince(drawStart);
  logSlowRenderOperation(
      ms, "scene draw took {:.1F}ms ({}x{} logical, {}x{} buffer)", ms, target.logicalWidth(), target.logicalHeight(),
      target.bufferWidth(), target.bufferHeight()
  );

  if (noctalia::profiling::enabled()) {
    float area = 0.0F;
    for (const DamageRect& r : repaint ? repaint->rects() : damage.rects()) {
      area += r.area();
    }
    kLog.info(
        "frame-stats {}x{} age={} rects={} full={} area={:.0F}% drawn={} culled={} draw_ms={:.3F}", target.logicalWidth(),
        target.logicalHeight(), bufferAge, repaint ? repaint->rects().size() : std::size_t{0}, fullReason != nullptr ? fullReason : "-",
        sw * sh > 0.0F ? area * 100.0F / (sw * sh) : 0.0F, m_statDrawn, m_statCulled, ms
    );
  }
  m_backend->endFrame(target, bufferDamage);
  const RenderGraphicsResetStatus resetStatus = m_backend->graphicsResetStatus();
  ms = elapsedSince(totalStart);
  logSlowRenderOperation(ms, "renderScene took {:.1F}ms total", ms);
  if (resetStatus != RenderGraphicsResetStatus::NoError) {
    handleGraphicsReset(resetStatus);
  }
  return true;
}

TextMetrics RenderContext::measureTextScaled(
    float scale, std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines,
    TextAlign align, TextEllipsize ellipsize, bool useMarkup
) {
  auto m = m_textRenderer.measure(scale, text, fontSize, font, maxWidth, maxLines, align, ellipsize, useMarkup);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight,
      .lineCount = m.lineCount,
      .alignOffsetX = m.alignOffsetX
  };
}

TextMetrics RenderContext::measureFontScaled(float scale, float fontSize, const TextFont& font) {
  auto m = m_textRenderer.measureFont(scale, fontSize, font);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight,
      .capHeight = m.capHeight
  };
}

void RenderContext::measureTextCursorStopsScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
    std::vector<float>& outStops, const TextFont& font
) {
  m_textRenderer.measureCursorStops(scale, text, fontSize, byteOffsets, outStops, font);
}

void RenderContext::measureTextCursorStopsWrappedScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
    std::vector<TextCursorStop>& outStops, const TextFont& font
) {
  m_textRenderer.measureCursorStopsWrapped(scale, text, fontSize, byteOffsets, maxWidth, outStops, font);
}

void RenderContext::measureTextCharSpansScaled(
    float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
    std::vector<TextCharSpan>& outSpans, const TextFont& font
) {
  m_textRenderer.measureCharSpans(scale, text, fontSize, byteOffsets, outSpans, font);
}

TextMetrics RenderContext::measureGlyphScaled(float scale, char32_t codepoint, float fontSize) {
  auto m = m_glyphRenderer.measureGlyph(scale, codepoint, fontSize);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.top,
      .inkBottom = m.bottom,
      .inkLeft = m.left,
      .inkRight = m.right
  };
}

TextureManager& RenderContext::textureManager() {
  makeCurrentNoSurface();
  return m_backend->textureManager();
}

void RenderContext::invalidateGpuResourcesNextFrame() noexcept {
  ++m_gpuResourceGeneration;
  m_glyphTexturesDirty = true;
}

void RenderContext::handleGraphicsReset(RenderGraphicsResetStatus status) {
  if (m_graphicsResetPending) {
    return;
  }
  m_graphicsResetPending = true;
  m_shared->reportGraphicsReset(status, "render");
}

void RenderContext::renderNode(
    float renderScale, const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh, float bw,
    float bh, float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip, bool ignoreNodeOpacity,
    bool parentPaintContained
) {
  if (!node->visible()) {
    return;
  }

  const Mat3 worldTransform = parentTransform * nodeLocalTransform(node);
  const float effectiveOpacity = ignoreNodeOpacity ? parentOpacity : parentOpacity * node->opacity();
  float boundsLeft = 0.0F;
  float boundsTop = 0.0F;
  float boundsRight = 0.0F;
  float boundsBottom = 0.0F;
  Node::transformedBounds(node, worldTransform, boundsLeft, boundsTop, boundsRight, boundsBottom);

  const bool paintContained = parentPaintContained || node->paintContained();
  if (paintContained
      && hasClip
      && node->type() != NodeType::RenderProxy
      && node->width() > 0.0F
      && node->height() > 0.0F
      && (boundsRight + kPaintCullSlack <= clipLeft
          || boundsLeft - kPaintCullSlack >= clipRight
          || boundsBottom + kPaintCullSlack <= clipTop
          || boundsTop - kPaintCullSlack >= clipBottom)) {
    ++m_statCulled;
    return;
  }

  // The node's own draw is skipped when it misses the clip; its children may still reach it. Same bounds as damage.
  // Glyph nodes have no size (ink sits above and right of the baseline origin), so they are never culled.
  bool drawSelf = true;
  if (hasClip && node->type() != NodeType::RenderProxy && node->type() != NodeType::Glyph) {
    const std::optional<DamageRect> paint = paintBounds(node, worldTransform);
    drawSelf = paint
        && paint->x1 > clipLeft
        && paint->x0 < clipRight
        && paint->y1 > clipTop
        && paint->y0 < clipBottom;
  }
  if (node->type() != NodeType::Base) {
    ++(drawSelf ? m_statDrawn : m_statCulled);
  }

  if (hasClip) {
    m_backend->setScissor(scissorForClip(sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom));
  } else {
    m_backend->disableScissor();
  }

  switch (drawSelf ? node->type() : NodeType::Base) {
  case NodeType::Rect: {
    const auto* rect = static_cast<const RectNode*>(node);
    auto style = rect->style();
    style.fill.a *= effectiveOpacity;
    style.border.a *= effectiveOpacity;
    for (auto& stop : style.gradientStops) {
      stop.color.a *= effectiveOpacity;
    }
    m_backend->drawRect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Text: {
    const auto* text = static_cast<const TextNode*>(node);
    if (!text->text().empty()) {
      const TextFont& font = text->font();
      if (text->hasShadow()) {
        auto shadowColor = text->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(text->shadowOffsetX(), text->shadowOffsetY());
        m_textRenderer.draw(
            renderScale, sw, sh, 0.0F, 0.0F, text->text(), text->fontSize(), shadowColor, shadowTransform, font,
            text->maxWidth(), text->maxLines(), text->textAlign(), text->ellipsize(), text->useMarkup()
        );
      }
      auto color = text->color();
      color.a *= effectiveOpacity;
      m_textRenderer.draw(
          renderScale, sw, sh, 0.0F, 0.0F, text->text(), text->fontSize(), color, worldTransform, font,
          text->maxWidth(), text->maxLines(), text->textAlign(), text->ellipsize(), text->useMarkup()
      );
    }
    break;
  }
  case NodeType::Image: {
    const auto* img = static_cast<const ImageNode*>(node);
    if (img->textureId() != 0) {
      m_backend->drawImage(
          RenderImageDraw{
              .texture = img->textureId(),
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .width = node->width(),
              .height = node->height(),
              .tint = img->tint(),
              .monochromeTint = img->monochromeTint(),
              .alphaMaskTint = img->alphaMaskTint(),
              .opacity = effectiveOpacity,
              .radius = img->radius(),
              .borderColor = img->borderColor(),
              .borderWidth = img->borderWidth(),
              .fitMode = static_cast<RenderImageFitMode>(img->fitMode()),
              .textureWidth = static_cast<float>(img->textureWidth()),
              .textureHeight = static_cast<float>(img->textureHeight()),
              .transform = worldTransform,
              .scrim = img->scrim(),
          }
      );
    }
    break;
  }
  case NodeType::Glyph: {
    const auto* icon = static_cast<const GlyphNode*>(node);
    if (icon->codepoint() != 0) {
      if (icon->hasShadow()) {
        auto shadowColor = icon->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(icon->shadowOffsetX(), icon->shadowOffsetY());
        m_glyphRenderer.drawGlyph(
            renderScale, sw, sh, 0.0F, 0.0F, icon->codepoint(), icon->fontSize(), shadowColor, shadowTransform
        );
      }
      auto color = icon->color();
      color.a *= effectiveOpacity;
      m_glyphRenderer.drawGlyph(
          renderScale, sw, sh, 0.0F, 0.0F, icon->codepoint(), icon->fontSize(), color, worldTransform
      );
    }
    break;
  }
  case NodeType::Spinner: {
    const auto* spinner = static_cast<const SpinnerNode*>(node);
    auto style = spinner->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawSpinner(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::CountdownRing: {
    const auto* ring = static_cast<const CountdownRingNode*>(node);
    auto style = ring->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawCountdownRing(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::ScreenCorner: {
    const auto* corner = static_cast<const ScreenCornerNode*>(node);
    auto style = corner->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawScreenCorner(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::AudioSpectrum: {
    const auto* spectrum = static_cast<const AudioSpectrumNode*>(node);
    auto style = spectrum->style();
    style.color1.a *= effectiveOpacity;
    style.color2.a *= effectiveOpacity;
    const float pixelScaleX = sw > 0.0F ? bw / sw : 1.0F;
    const float pixelScaleY = sh > 0.0F ? bh / sh : 1.0F;
    m_backend->drawAudioSpectrum(
        sw, sh, pixelScaleX, pixelScaleY, node->width(), node->height(), style, spectrum->values(), worldTransform
    );
    break;
  }
  case NodeType::FancyAudioVisualizer: {
    const auto* visualizer = static_cast<const FancyAudioVisualizerNode*>(node);
    if (visualizer->textureId() != 0) {
      auto style = visualizer->style();
      style.primaryColor.a *= effectiveOpacity;
      style.secondaryColor.a *= effectiveOpacity;
      m_backend->drawFancyAudioVisualizer(
          visualizer->textureId(), visualizer->textureWidth(), sw, sh, node->width(), node->height(), style,
          worldTransform
      );
    }
    break;
  }
  case NodeType::Effect: {
    const auto* effect = static_cast<const EffectNode*>(node);
    auto style = effect->style();
    style.bgColor.a *= effectiveOpacity;
    m_backend->drawEffect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Graph: {
    const auto* graph = static_cast<const GraphNode*>(node);
    if (graph->textureId() != 0) {
      auto style = graph->style();
      style.lineColor1.a *= effectiveOpacity;
      style.lineColor2.a *= effectiveOpacity;
      style.lineColor3.a *= effectiveOpacity;
      style.graphFillOpacity *= effectiveOpacity;
      m_backend->drawGraph(
          graph->textureId(), graph->textureWidth(), sw, sh, node->width(), node->height(), style, worldTransform
      );
    }
    break;
  }
  case NodeType::Wallpaper: {
    const auto* wallpaper = static_cast<const WallpaperNode*>(node);
    const bool hasSource1 = wallpaper->sourceKind1() == WallpaperSourceKind::Color || wallpaper->texture1() != 0;
    if (hasSource1) {
      const bool hasSource2 = wallpaper->sourceKind2() == WallpaperSourceKind::Color || wallpaper->texture2() != 0;
      const WallpaperSourceKind sourceKind2 = hasSource2 ? wallpaper->sourceKind2() : wallpaper->sourceKind1();
      const TextureId texture2 = hasSource2 ? wallpaper->texture2() : wallpaper->texture1();
      const Color& sourceColor2 = hasSource2 ? wallpaper->sourceColor2() : wallpaper->sourceColor1();
      const float imageWidth2 = hasSource2 ? wallpaper->imageWidth2() : wallpaper->imageWidth1();
      const float imageHeight2 = hasSource2 ? wallpaper->imageHeight2() : wallpaper->imageHeight1();
      const float progress = hasSource2 ? wallpaper->progress() : 0.0F;
      m_backend->drawWallpaper(
          WallpaperDrawParams{
              .transition = wallpaper->transition(),
              .from =
                  {.kind = wallpaper->sourceKind1(),
                   .texture = wallpaper->texture1(),
                   .color = wallpaper->sourceColor1(),
                   .imageWidth = wallpaper->imageWidth1(),
                   .imageHeight = wallpaper->imageHeight1()},
              .to =
                  {.kind = sourceKind2,
                   .texture = texture2,
                   .color = sourceColor2,
                   .imageWidth = imageWidth2,
                   .imageHeight = imageHeight2},
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .quadWidth = node->width(),
              .quadHeight = node->height(),
              .progress = progress,
              .fillMode = static_cast<float>(wallpaper->fillMode()),
              .params = wallpaper->transitionParams(),
              .fillColor = wallpaper->fillColor(),
              .transform = worldTransform,
              .span = wallpaper->spanParams(),
          }
      );
    }
    break;
  }
  case NodeType::LockscreenTransition: {
    const auto* transition = static_cast<const LockscreenTransitionNode*>(node);
    if (transition->texture() != 0) {
      m_backend->drawLockscreenTransition(
          LockscreenTransitionDrawParams{
              .transition = transition->transition(),
              .texture = transition->texture(),
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .quadWidth = node->width(),
              .quadHeight = node->height(),
              .progress = transition->progress(),
              .opacity = effectiveOpacity,
              .params = transition->transitionParams(),
              .transform = worldTransform,
          }
      );
    }
    break;
  }
  case NodeType::RenderProxy: {
    const auto* proxy = static_cast<const RenderProxyNode*>(node);
    const Node* source = proxy->source();
    bool sourceContainsProxy = false;
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == source) {
        sourceContainsProxy = true;
        break;
      }
    }
    if (source != nullptr && !sourceContainsProxy) {
      const Mat3 sourceParent = worldTransform * Mat3::translation(-source->x(), -source->y());
      renderNode(
          renderScale, source, sourceParent, effectiveOpacity, sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom,
          hasClip, true, false
      );
    }
    return;
  }
  case NodeType::Base:
    break;
  }

  // Fast path: children are already in zIndex order (the common case — most
  // callers never touch zIndex, or set it identically across siblings). Skip
  // allocating/sorting a side vector and iterate the child list directly.
  // Only fall back to the sorted copy when there's an actual out-of-order
  // pair, which removes a per-node heap allocation from every rendered frame.
  const auto& children = node->children();
  bool childrenSorted = true;
  for (std::size_t i = 1; i < children.size(); ++i) {
    if (children[i]->zIndex() < children[i - 1]->zIndex()) {
      childrenSorted = false;
      break;
    }
  }

  std::vector<const Node*> orderedChildren;
  if (!childrenSorted) {
    orderedChildren.reserve(children.size());
    for (const auto& child : children) {
      orderedChildren.push_back(child.get());
    }
    std::ranges::stable_sort(orderedChildren, [](const Node* a, const Node* b) { return a->zIndex() < b->zIndex(); });
  }

  float childClipLeft = clipLeft;
  float childClipTop = clipTop;
  float childClipRight = clipRight;
  float childClipBottom = clipBottom;
  bool childHasClip = hasClip;

  if (node->clipChildren()) {
    childClipLeft = hasClip ? std::max(childClipLeft, boundsLeft) : boundsLeft;
    childClipTop = hasClip ? std::max(childClipTop, boundsTop) : boundsTop;
    childClipRight = hasClip ? std::min(childClipRight, boundsRight) : boundsRight;
    childClipBottom = hasClip ? std::min(childClipBottom, boundsBottom) : boundsBottom;
    childHasClip = true;
  }

  if (childHasClip && (childClipRight <= childClipLeft || childClipBottom <= childClipTop)) {
    return;
  }

  if (childrenSorted) {
    for (const auto& child : children) {
      renderNode(
          renderScale, child.get(), worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop,
          childClipRight, childClipBottom, childHasClip, false, paintContained
      );
    }
  } else {
    for (const auto* child : orderedChildren) {
      renderNode(
          renderScale, child, worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop,
          childClipRight, childClipBottom, childHasClip, false, paintContained
      );
    }
  }
}

void RenderContext::cleanup() {
  if (m_backend != nullptr) {
    // Need a current context to destroy GL resources, but we may not have a surface.
    m_backend->makeCurrentNoSurface();
  }

  // Text renderers first — they destroy GL textures and need a current context.
  m_textRenderer.cleanup();
  m_glyphRenderer.cleanup();

  if (m_backend != nullptr) {
    m_backend->cleanup();
    m_backend.reset();
  }
}

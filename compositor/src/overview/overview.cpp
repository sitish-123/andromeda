#include "overview/overview.h"

extern "C" {
#include <andromedafx/render/effect.h>
}

#include "config/config.h"
#include "core/log.h"
#include "core/tracy.h"
#include "input/cursor.h"
#include "input/event_time.h"
#include "input/gesture_physics.h"
#include "input/gestures.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/drop_target.h"
#include "layout/layout.h"
#include "layout/master.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/interact.h"
#include "overview/shortcut_labels.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/effect_registry.h"
#include "scene/hint_rect.h"
#include "scene/ssd_frame.h"
#include "scene/surface_shape.h"
#include "scene/text_buffer.h"
#include "server/color_manager.h"
#include "server/desktop_shell.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace andromeda {

  namespace {
    constexpr Logger kLog("overview");
    constexpr double kMiddleScrollStepPx = 105.0;

    // Overview progress over which the desktop's shadows and pinned windows fade, starting from the desktop.
    constexpr double kDesktopChromeFade = 0.1;
    // Below this a card is neither drawn nor hit.
    constexpr float kHiddenOpacity = 0.004F;
    // A card the filter misses keeps this share of its opacity.
    constexpr float kFilterMissOpacity = 0.25F;

    static_assert(static_cast<int>(OverviewStyle::Grid) == static_cast<int>(ov::Style::Grid));
    ov::Style configuredStyle() { return static_cast<ov::Style>(config().overview.style); }

    std::string_view arrangementLabel(LayoutMode mode) {
      switch (mode) {
      case LayoutMode::Floating:
        return "Free";
      case LayoutMode::Scrolling:
        return "Scrolling";
      case LayoutMode::Dwindle:
        return "Split";
      case LayoutMode::Master:
        return "Main + Stack";
      }
      return "";
    }

    ov::Arrangement arrangementOf(LayoutMode mode) {
      switch (mode) {
      case LayoutMode::Floating:
        return ov::Arrangement::Free;
      case LayoutMode::Scrolling:
        return ov::Arrangement::Scrolling;
      case LayoutMode::Dwindle:
      case LayoutMode::Master:
        return ov::Arrangement::Tiled;
      }
      return ov::Arrangement::Scrolling;
    }

    // Whole layout pixels: a settled card sits on the pixel grid.
    wlr_box layoutBox(const ov::Rect& rect, const wlr_box& output) {
      return {
          .x = static_cast<int>(std::lround(output.x + rect.x)),
          .y = static_cast<int>(std::lround(output.y + rect.y)),
          .width = std::max(1, static_cast<int>(std::lround(rect.w))),
          .height = std::max(1, static_cast<int>(std::lround(rect.h))),
      };
    }

    ov::Rect localRect(const wlr_box& box, const wlr_box& output) {
      return {
          static_cast<double>(box.x - output.x), static_cast<double>(box.y - output.y), static_cast<double>(box.width),
          static_cast<double>(box.height)
      };
    }

    // A view with a box to draw: a tiled opener waiting for the arrange that places it, and a hidden tab sharing its
    // box with the tab on show, have none.
    bool laidOut(const View* view) {
      const wlr_box& geometry = view->geometryBox();
      const wlr_box& world = view->presentedBox();
      return geometry.width > 0
          && geometry.height > 0
          && world.width > 0
          && world.height > 0
          && !view->presentationSuppressed();
    }
    // Pointer travel that promotes a press on a card into a relocate drag.
    constexpr double kDragThreshold = 10.0;
    // Edge auto-scroll while dragging: the band along an output edge, the hold before the first row step, the step
    // period after it, and the pan speed of a scrolling workspace (no hold; it runs on the frame clock).
    constexpr double kEdgeZone = 72.0;
    constexpr int kEdgeDelayMs = 380;
    constexpr int kEdgeRepeatMs = 650;
    constexpr double kEdgePanPxPerMs = 0.55; // 22 px every 40 ms in the prototype, spread over every frame
    // Corner radius of the ring over the new-workspace target.
    constexpr int kPlusHintRadius = 14;
    // A drop on the middle of a tiled window swaps the two; the outer 28 % on each side inserts beside it (prototype).
    constexpr double kSwapZoneStart = 0.28;
    // How much of the focused border color mixes into the unfocused one for a landing target that is not the live one.
    constexpr float kLandingTargetBlend = 0.4F;
    // Inset of a shortcut badge from the card edge it hugs.
    constexpr int kBadgeMargin = 6;

    std::array<float, 4> mixColor(const std::array<float, 4>& from, const std::array<float, 4>& to, float amount) {
      std::array<float, 4> out{};
      for (size_t index = 0; index < out.size(); ++index) {
        out[index] = std::lerp(from[index], to[index], amount);
      }
      return out;
    }

    bool boxContains(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    // wlr_scene_rect colors are premultiplied; straight alpha renders as an
    // over-bright wash (scene/color.h).
    std::array<float, 4> tint(const std::array<float, 4>& base, double opacity) {
      std::array<float, 4> out{};
      // Overshooting curves push the overview progress past [0, 1].
      premultiplied(out.data(), base, static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
      return out;
    }
    // A scene buffer's opaque region is in its destination coordinates, so a scaled copy of a surface needs the
    // surface's region scaled with it. Edges round inward: a pixel the opaque area only partly covers stays
    // see-through.
    void setScaledOpaqueRegion(
        wlr_scene_buffer* buffer, const pixman_region32_t& source, int sourceWidth, int sourceHeight, int destWidth,
        int destHeight
    ) {
      pixman_region32_t scaled;
      pixman_region32_init(&scaled);
      if (sourceWidth > 0 && sourceHeight > 0) {
        const double sx = static_cast<double>(destWidth) / sourceWidth;
        const double sy = static_cast<double>(destHeight) / sourceHeight;
        int count = 0;
        const pixman_box32_t* rects = pixman_region32_rectangles(&source, &count);
        for (int index = 0; index < count; ++index) {
          const auto x1 = static_cast<int>(std::ceil(rects[index].x1 * sx));
          const auto y1 = static_cast<int>(std::ceil(rects[index].y1 * sy));
          const auto x2 = static_cast<int>(std::floor(rects[index].x2 * sx));
          const auto y2 = static_cast<int>(std::floor(rects[index].y2 * sy));
          if (x2 > x1 && y2 > y1) {
            pixman_region32_union_rect(&scaled, &scaled, x1, y1, x2 - x1, y2 - y1);
          }
        }
      }
      wlr_scene_buffer_set_opaque_region(buffer, &scaled);
      pixman_region32_fini(&scaled);
    }

    void layoutWorkspaceBackground(
        wlr_scene_rect* background, const wlr_box& box, int radius, const std::array<float, 4>& color
    ) {
      if (background == nullptr) {
        return;
      }
      if (color[3] <= 0.001F) {
        wlr_scene_node_set_enabled(&background->node, false);
        return;
      }

      wlr_scene_node_set_enabled(&background->node, true);
      wlr_scene_node_set_position(&background->node, box.x, box.y);
      wlr_scene_rect_set_size(background, box.width, box.height);
      wlr_scene_rect_set_color(background, color.data());
      wlr_scene_rect_set_clipped_region(background, clipped_region_get_default());
      wlr_scene_rect_set_corner_radii(background, corner_radii_all(radius));
    }

    // Cards are pure output: hit testing runs off Overview's own boxes, so scene
    // input must never land on them (Server::viewAt then sees layer surfaces only).
    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }

    char asciiLower(char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    char shortcutCharacter(uint32_t keysym) {
      if (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_9) {
        return static_cast<char>('0' + (keysym - XKB_KEY_KP_0));
      }
      const xkb_keysym_t lowered = xkb_keysym_to_lower(keysym);
      return lowered >= 0x21 && lowered <= 0x7E ? static_cast<char>(lowered) : '\0';
    }

    bool shortcutStartsWith(std::string_view label, std::string_view prefix) {
      if (label.size() < prefix.size()) {
        return false;
      }
      for (size_t index = 0; index < prefix.size(); ++index) {
        if (asciiLower(label[index]) != prefix[index]) {
          return false;
        }
      }
      return true;
    }

    wlr_scene_buffer* sourceBufferForSurface(wlr_scene_node* node, wlr_surface* surface) {
      if (node->type == WLR_SCENE_NODE_BUFFER) {
        wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
        return sceneSurface != nullptr && sceneSurface->surface == surface ? buffer : nullptr;
      }
      if (node->type != WLR_SCENE_NODE_TREE) {
        return nullptr;
      }
      wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &tree->children, link) {
        if (wlr_scene_buffer* buffer = sourceBufferForSurface(child, surface)) {
          return buffer;
        }
      }
      return nullptr;
    }

    // The size `surface` is shown at in its view, through the view's scene buffer `source`. It differs from the surface
    // size when the view draws the surface scaled.
    void viewSurfaceSize(wlr_scene_buffer* source, const wlr_surface* surface, int* width, int* height) {
      if (wlr_scene_surface* sceneSurface = source != nullptr ? wlr_scene_surface_try_from_buffer(source) : nullptr) {
        wlr_scene_surface_get_size(sceneSurface, width, height);
        return;
      }
      *width = surface->current.width;
      *height = surface->current.height;
    }
  } // namespace

  Overview::Overview(Server& server) : m_server(&server) { m_server->registerAnimatable(this); }

  Overview::~Overview() {
    m_server->unregisterAnimatable(this);
    m_zoomAnim.snap(0.0);
    teardown();
    if (m_edgeTimer != nullptr) {
      wl_event_source_remove(m_edgeTimer);
    }
  }

  float Overview::desktopChromeAlpha() const {
    return static_cast<float>(std::clamp(1.0 - m_progress / kDesktopChromeFade, 0.0, 1.0));
  }

  double Overview::motionPixelsPerUnit(bool rows) const {
    // A window travels at most from its desktop rect, one output extent away per workspace, to its place; a row step
    // moves anything by at most one output extent. Whole pixels per unit keep the target on the pixel grid
    // finishSpringTail rounds against.
    double bound = 0.0;
    for (const auto& state : m_outputs) {
      const auto extent = static_cast<double>(std::max(state->outputBox.width, state->outputBox.height));
      const double steps = rows ? 1.0 : static_cast<double>(state->workspaceViews.size() + 1);
      bound = std::max(bound, extent * steps);
    }
    if (!rows) {
      bound *= std::abs(m_targetProgress - m_progressFrom);
    }
    return std::ceil(bound);
  }

  // -: geometry

  bool Overview::refreshOutputBoxes(OutputState& state) const {
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), state.output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }
    state.outputBox = outputBox;
    state.usableBox = state.output->usableArea();
    if (state.usableBox.width <= 0 || state.usableBox.height <= 0) {
      state.usableBox = outputBox;
    }
    return true;
  }

  ov::Input Overview::placementInput(const OutputState& state, bool targets) const {
    ov::Input input;
    input.width = state.outputBox.width;
    input.height = state.outputBox.height;
    input.usable = localRect(state.usableBox, state.outputBox);
    input.stripScale = std::clamp(config().overview.zoom, 0.1, 0.75);
    const WorkspaceGroup* group = state.output->workspaceGroup();
    input.horizontal = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
    input.workspaces.resize(state.workspaceBackgrounds.size());
    for (size_t index = 0; group != nullptr && index < input.workspaces.size(); ++index) {
      if (const Workspace* workspace = group->workspaceAt(index)) {
        input.workspaces[index].arrangement = arrangementOf(workspace->layoutMode());
      }
    }
    for (size_t k = 0; k < state.cards.size(); ++k) {
      const Card& card = *state.cards[k];
      if (card.workspaceIndex >= input.workspaces.size() || !laidOut(card.view)) {
        continue;
      }
      wlr_box world = card.view->presentedBox();
      if (targets) {
        world.x = card.view->layoutTargetX();
        world.y = card.view->layoutTargetY();
      }
      input.workspaces[card.workspaceIndex].members.push_back(
          {static_cast<uint32_t>(k), localRect(world, state.outputBox), card.view->minimized()}
      );
    }
    return input;
  }

  ov::Fit Overview::workspaceFit(const OutputState& state, size_t index) {
    if (state.mainBox.has_value() && index == state.mainWorkspace) {
      return state.mainFit;
    }
    if (index < state.workspaceViews.size() && state.workspaceViews[index].shown) {
      return state.workspaceViews[index].fit;
    }
    return {1.0, static_cast<double>(state.outputBox.x), static_cast<double>(state.outputBox.y)};
  }

  bool Overview::workspaceViewContains(const OutputState& state, size_t index, double lx, double ly) {
    if (state.mainBox.has_value() && index == state.mainWorkspace && boxContains(*state.mainBox, lx, ly)) {
      return true;
    }
    return index < state.workspaceViews.size()
        && state.workspaceViews[index].shown
        && boxContains(state.workspaceViews[index].box, lx, ly);
  }

  double Overview::workspaceScale(const Workspace* workspace) const {
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output->workspaceGroup();
      if (workspace == nullptr || group == nullptr || workspace->group() != group || !state->placed) {
        continue;
      }
      // The settled scale, not the one mid-zoom: travel means the same before the opening has landed as after.
      const ov::Placement& placement = state->placement;
      const size_t index = workspace->index();
      if (placement.mainFit.has_value() && index == state->mainWorkspace) {
        return std::max(0.01, placement.mainFit->s);
      }
      for (const ov::Card& card : placement.cards) {
        if (card.workspace == index && state->placedInput.width > 0) {
          return std::max(0.01, card.rect.w / state->placedInput.width);
        }
      }
    }
    return 1.0;
  }

  void Overview::layoutCard(Card& card, const View* liveTarget) {
    View* view = card.view;
    if (card.shadowTree != nullptr) {
      wlr_scene_node_set_enabled(&card.shadowTree->node, false);
    }
    const wlr_box& geometry = view->geometryBox();
    const wlr_box& world = view->presentedBox();
    // Not laid out yet (a tiled opener waiting for its arrange, a hidden tab sharing the shown tab's box), or faded
    // out by the placement: nothing to draw, nothing to hit.
    if (!laidOut(view) || (card.opacity < kHiddenOpacity && &card != m_dragCard)) {
      card.blur.hide();
      wlr_scene_node_set_enabled(&card.tree->node, false);
      syncCardEffects(card);
      return;
    }
    wlr_scene_node_set_enabled(&card.tree->node, true);

    const double z = card.scale;
    const int contentW = std::max(1, static_cast<int>(std::lround(world.width * z)));
    const int contentH = std::max(1, static_cast<int>(std::lround(world.height * z)));
    // The drag owns the card origin; the size always follows the presented box, which may have changed since the
    // output was laid out.
    card.box.width = contentW;
    card.box.height = contentH;
    wlr_scene_node_set_position(&card.tree->node, card.box.x, card.box.y);
    wlr_scene_tree_set_clip(card.tree, card.clip.width > 0 && card.clip.height > 0 ? &card.clip : nullptr);
    layoutCardShadow(card, desktopChromeAlpha() * card.opacity);
    const float cardOpacity = (&card == m_dragCard ? config().appearance.dragOpacity : 1.0F) * card.opacity;
    const float presentedOpacity = view->presentedOpacity() * cardOpacity;

    const int borderWidth = view->decorationBorderWidth();
    const int outerBorderWidth = view->decorationOuterBorderWidth();
    const int total = borderWidth + outerBorderWidth;
    const bool decorated = total > 0 && !view->currentFullscreen() && !view->maximizedToEdges();
    const int scaledRadius = static_cast<int>(std::lround(view->decorationCornerRadius() * z));
    const int outerRadius = decorated ? scaledRadius : 0;
    const auto scaledWidth = [z](int width) {
      return width > 0 ? std::max(1, static_cast<int>(std::lround(width * z))) : 0;
    };
    const int innerWidth = scaledWidth(borderWidth);
    const int outerWidth = scaledWidth(outerBorderWidth);
    const int surfaceRadius = nestedRadius(outerRadius, innerWidth + outerWidth);
    const bool borderVisible = decorated && innerWidth + outerWidth > 0;
    wlr_scene_node_set_enabled(&card.border->node, borderVisible);
    if (borderVisible) {
      card.borderPadding = scaledWidth(view->borderEffectPadding());
      applyBorderGeometry(
          card.border,
          makeBorderRing(
              contentW, contentH + static_cast<int>(std::lround(view->titleBarHeight() * z)), outerRadius, innerWidth,
              outerWidth, card.borderPadding
          ),
          innerWidth, outerWidth
      );
      wlr_scene_node_set_position(
          &card.border->node, card.border->node.x,
          card.border->node.y - static_cast<int>(std::lround(view->titleBarHeight() * z))
      );
      const std::array<float, 4> innerColor = tint(cardBorderColor(card, liveTarget), presentedOpacity);
      const std::array<float, 4> outerColor = tint(view->borderColors().outer, presentedOpacity);
      wlr_scene_border_set_colors(card.border, innerColor.data(), outerColor.data());
    }
    layoutCardChrome(card, world, decorated ? total : 0, z, presentedOpacity);
    syncCardEffects(card);

    if (card.badge != nullptr) {
      // Overshooting curves can push m_progress past [0, 1] and wlr_scene_buffer_set_opacity asserts.
      const auto badgeAlpha = static_cast<float>(std::clamp(m_progress, 0.0, 1.0));
      const bool matched = card.shortcutMatched != SIZE_MAX;
      const bool fits =
          contentW >= card.badgeWidth + 2 * kBadgeMargin && contentH >= card.badgeHeight + 2 * kBadgeMargin;
      const bool badgeOn =
          !card.shortcut.empty() && matched && fits && !m_closing && &card != m_dragCard && badgeAlpha > 0.01F;
      wlr_scene_node_set_enabled(&card.badge->node, badgeOn);
      if (badgeOn) {
        // The badge hugs the card's top-left corner, the only corner the output clip or the top/overlay exclusive
        // zones can hide. On each axis it slides just far enough to clear the start of the usable area, never past
        // the card's opposite inset, where a card scrolled out of view keeps it. `fits` keeps the card-local bounds
        // ordered.
        const auto inset = [](int origin, int extent, int badgeExtent, int clipStart) {
          return std::clamp(
              std::max(clipStart, origin) + kBadgeMargin - origin, kBadgeMargin, extent - badgeExtent - kBadgeMargin
          );
        };
        wlr_scene_node_set_position(
            &card.badge->node, inset(card.box.x, contentW, card.badgeWidth, card.owner->usableBox.x),
            inset(card.box.y, contentH, card.badgeHeight, card.owner->usableBox.y)
        );
        wlr_scene_buffer_set_opacity(card.badgeText, badgeAlpha);
        const std::array<float, 4> background = tint(card.badgeBackground, badgeAlpha);
        wlr_scene_rect_set_color(card.badgeRect, background.data());
        // The badge renders unscaled, so it takes the zoomed radius the cards
        // around it use instead of the full-size one.
        wlr_scene_rect_set_corner_radius(
            card.badgeRect, std::min(scaledRadius, std::min(card.badgeWidth, card.badgeHeight) / 2)
        );
      }
    }

    // Every surface of the card rounds against the card's content box, the same rule the live window uses, so a
    // client that draws its corners from a subsurface keeps them rounded in the thumbnail.
    const fx_corner_radii contentCorners = view->titleBarHeight() > 0
        ? corner_radii_new(0, 0, surfaceRadius, surfaceRadius)
        : corner_radii_all(surfaceRadius);
    const auto roundToCardBox = [surfaceRadius, contentW, contentH, contentCorners](wlr_scene_buffer* buffer) {
      const wlr_box cornerBox{-buffer->node.x, -buffer->node.y, contentW, contentH};
      wlr_scene_buffer_set_corner_radii(buffer, contentCorners);
      wlr_scene_buffer_set_corner_box(buffer, surfaceRadius > 0 ? &cornerBox : nullptr);
    };
    const double fx = static_cast<double>(contentW) / geometry.width;
    const double fy = static_cast<double>(contentH) / geometry.height;
    bool blurUpdated = false;
    for (const auto& entry : card.surfaces) {
      wlr_surface* surface = entry->surface;
      if (entry->buffer == nullptr) {
        continue;
      }
      // A client retires a subsurface with a null buffer, or by destroying its subsurface role. Either drops the
      // surface out of the commit walk; skipping its mirror would leave the last frame at the zoom it was laid out at.
      // A destroyed role emits no commit, so `mapped` catches it before the next walk does.
      if (!entry->inTree || !surface->mapped || surface->current.width <= 0 || surface->current.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      const float surfaceOpacity =
          entry->sourceBuffer != nullptr ? entry->sourceBuffer->opacity * cardOpacity : cardOpacity;
      wlr_scene_buffer_set_opacity(entry->buffer, surfaceOpacity);
      const pixman_region32_t* shape = entry->view->xwayland() ? m_server->xwayland()->surfaceShape(surface) : nullptr;
      if (!entry->isRoot) {
        wlr_box sub{
            card.box.x + static_cast<int>(std::lround((entry->sx - geometry.x) * fx)),
            card.box.y + static_cast<int>(std::lround((entry->sy - geometry.y) * fy)),
            std::max(1, static_cast<int>(std::lround(surface->current.width * fx))),
            std::max(1, static_cast<int>(std::lround(surface->current.height * fy))),
        };
        wlr_scene_node_set_enabled(&entry->buffer->node, true);
        wlr_scene_node_set_position(&entry->buffer->node, sub.x - card.box.x, sub.y - card.box.y);
        wlr_scene_buffer_set_dest_size(entry->buffer, sub.width, sub.height);
        applySurfaceShape(entry->buffer, surface, shape);
        roundToCardBox(entry->buffer);
        continue;
      }
      // Root surface: crop to the committed window geometry so CSD shadow padding never leaks into the thumbnail, then
      // scale the visible part of that region onto the card.
      wlr_fbox base{};
      wlr_surface_get_buffer_source_box(surface, &base);
      int width = 0;
      int height = 0;
      viewSurfaceSize(entry->sourceBuffer, surface, &width, &height);
      const double bx = base.width / width;
      const double by = base.height / height;
      wlr_fbox src{base.x + geometry.x * bx, base.y + geometry.y * by, geometry.width * bx, geometry.height * by};
      if (src.x < base.x) {
        src.width -= base.x - src.x;
        src.x = base.x;
      }
      if (src.y < base.y) {
        src.height -= base.y - src.y;
        src.y = base.y;
      }
      src.width = std::min(src.width, base.x + base.width - src.x);
      src.height = std::min(src.height, base.y + base.height - src.y);
      if (src.width <= 0 || src.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      wlr_scene_node_set_enabled(&entry->buffer->node, true);
      wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
      wlr_scene_buffer_set_source_box(entry->buffer, &src);
      wlr_scene_buffer_set_dest_size(entry->buffer, contentW, contentH);
      applySurfaceShape(entry->buffer, surface, shape);
      roundToCardBox(entry->buffer);
      const wlr_box blurBox{0, 0, contentW, contentH};
      card.blur.setAlpha(1.0F);
      card.blur.setMaterial(configGlass(glass::Kind::Sheet));
      card.blur.update(
          card.tree, surface, blurBox, geometry, contentCorners, nullptr, view->blurOptions(), entry->buffer->opacity,
          entry->buffer
      );
      blurUpdated = true;
    }
    if (!blurUpdated) {
      card.blur.hide();
    }
  }

  void Overview::layoutCardChrome(Card& card, const wlr_box& world, int borderInset, double zoom, float alpha) const {
    const Output* output = card.owner != nullptr ? card.owner->output : nullptr;
    ViewChromeGeometry geometry{
        .contentX = 0,
        .contentY = 0,
        .contentWidth = world.width,
        .contentHeight = world.height,
        .borderInset = borderInset,
        .cornerRadius = card.view->maximizedToEdges() ? 0 : card.view->decorationCornerRadius(),
        .titleBarHeight = card.view->titleBarHeight(),
        .scale = output != nullptr ? output->wlr()->scale : 1.0F,
        .suppressed = card.view->currentFullscreen(),
        .zoom = zoom,
    };
    const auto update = [&](const ViewChromeAttachment* live, std::unique_ptr<ViewChromeAttachment>& preview) {
      if (live == nullptr) {
        preview.reset();
        return;
      }
      if (preview == nullptr)
        preview = live->makePreview(card.tree);
      if (preview == nullptr)
        return;
      live->syncPreview(*preview);
      preview->setAlpha(alpha);
      preview->layout(geometry);
    };
    update(card.view->ssdFrame(), card.frame);
    geometry.suppressed = geometry.suppressed || card.view->maximizedToEdges();
    update(card.view->chromeAttachment(), card.chrome);
  }

  void Overview::layoutCardShadow(Card& card, float alpha) const {
    const double zoom = card.scale;
    const wlr_scene_shadow* source = card.view->shadowNode();
    if (alpha <= 0.0F || source == nullptr || !source->node.enabled || &card == m_dragCard) {
      return;
    }
    wlr_scene_tree* parent = card.view->shadowPooled() ? card.owner->tileShadows : card.tree;
    if (parent == nullptr) {
      return;
    }
    if (card.shadowTree == nullptr) {
      card.shadowTree = wlr_scene_tree_create(parent);
      if (card.shadowTree == nullptr) {
        return;
      }
      card.shadow = wlr_scene_shadow_create(card.shadowTree, 0, 0, 0, 0.0F, source->color);
      if (card.shadow == nullptr) {
        destroyCardShadow(card);
        return;
      }
    } else if (card.shadowTree->node.parent != parent) {
      wlr_scene_node_reparent(&card.shadowTree->node, parent);
    }
    // Under the card the tree follows the card origin and sits below its content, the way the real container sits
    // under the frame; in the pool it carries the card origin itself.
    if (parent == card.tree) {
      wlr_scene_node_lower_to_bottom(&card.shadowTree->node);
      wlr_scene_node_set_position(&card.shadowTree->node, 0, 0);
    } else {
      wlr_scene_node_set_position(&card.shadowTree->node, card.box.x, card.box.y);
    }
    wlr_scene_node_set_enabled(&card.shadowTree->node, true);

    const auto scaled = [zoom](int value) { return static_cast<int>(std::lround(value * zoom)); };
    // Signed: a negative radius is a concave flare (fx_corner_radii).
    const auto scaledCorner = [zoom](int16_t value) { return static_cast<int16_t>(std::lround(value * zoom)); };
    wlr_scene_node_set_position(&card.shadow->node, scaled(source->node.x), scaled(source->node.y));
    wlr_scene_shadow_set_size(card.shadow, std::max(0, scaled(source->width)), std::max(0, scaled(source->height)));
    wlr_scene_shadow_set_blur_sigma(card.shadow, static_cast<float>(source->blur_sigma * zoom));
    wlr_scene_shadow_set_corner_radius(card.shadow, scaled(source->corner_radius));
    const float color[4] = {source->color[0], source->color[1], source->color[2], source->color[3] * alpha};
    wlr_scene_shadow_set_color(card.shadow, color);
    const clipped_region& hole = source->clipped_region;
    wlr_scene_shadow_set_clipped_region(
        card.shadow,
        clipped_region{
            .area =
                {
                    .x = scaled(hole.area.x),
                    .y = scaled(hole.area.y),
                    .width = scaled(hole.area.width),
                    .height = scaled(hole.area.height),
                },
            .corners = {
                .top_left = scaledCorner(hole.corners.top_left),
                .top_right = scaledCorner(hole.corners.top_right),
                .bottom_right = scaledCorner(hole.corners.bottom_right),
                .bottom_left = scaledCorner(hole.corners.bottom_left),
            },
        }
    );
  }

  void Overview::destroyCardShadow(Card& card) {
    if (card.shadowTree != nullptr) {
      wlr_scene_node_destroy(&card.shadowTree->node);
    }
    card.shadowTree = nullptr;
    card.shadow = nullptr;
  }

  void Overview::layoutOutput(OutputState& state) {
    ANDROMEDA_ZONE("Overview::layoutOutput");
    if (!refreshOutputBoxes(state)) {
      return;
    }
    const wlr_box& outputBox = state.outputBox;

    // Cards moving in from other workspaces start past the output. One clip on this output's overview tree contains
    // every card, ring, and workspace background, so none needs to trim its own geometry. The dragged card is
    // reparented out to the unclipped overview root, which is what lets it span outputs.
    wlr_scene_tree_set_clip(state.tree, &outputBox);
    if (state.backgroundBlur != nullptr) {
      wlr_scene_blur* blur = state.backgroundBlur;
      wlr_scene_node_set_enabled(&blur->node, m_progress > 0.001);
      wlr_scene_node_set_position(&blur->node, outputBox.x, outputBox.y);
      if (blur->width != outputBox.width || blur->height != outputBox.height) {
        wlr_scene_blur_set_size(blur, outputBox.width, outputBox.height);
      }
      // Blur alpha is normalized to [0, 1]; clamp an overshooting progress. Strength stays 1: only a full-strength
      // bottom-layer blur reads the cached wallpaper blur, any other strength re-blurs the whole output every frame.
      const auto level = static_cast<float>(std::clamp(m_progress, 0.0, 1.0));
      if (blur->alpha != level) {
        wlr_scene_blur_set_alpha(blur, level);
      }
    }

    wlr_scene_node_set_position(&state.backgroundTint->node, outputBox.x, outputBox.y);
    wlr_scene_rect_set_size(state.backgroundTint, outputBox.width, outputBox.height);
    const std::array<float, 4> backgroundTint = tint(config().colors.overview.backgroundTint, m_progress);
    // A fully transparent tint leaves the wallpaper untouched.
    wlr_scene_node_set_enabled(&state.backgroundTint->node, backgroundTint[3] > 0.001F);
    wlr_scene_rect_set_color(state.backgroundTint, backgroundTint.data());

    refreshDesktop(state);

    // Placement is pure and settled; only its input changing re-runs it. An open or close in flight interpolates.
    const ov::Style style = configuredStyle();
    const double row = state.rowScroll.current();
    ov::Input input = placementInput(state, false);
    std::vector<uintptr_t> structure{static_cast<uintptr_t>(style), input.workspaces.size()};
    for (const ov::Workspace& workspace : input.workspaces) {
      structure.push_back(static_cast<uintptr_t>(workspace.arrangement));
      structure.push_back(workspace.members.size());
      for (const ov::Member& member : workspace.members) {
        structure.push_back(reinterpret_cast<uintptr_t>(state.cards[member.id]->view));
        structure.push_back(member.minimized ? 1U : 0U);
      }
    }
    if (const WorkspaceGroup* group = state.output->workspaceGroup()) {
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        structure.push_back(reinterpret_cast<uintptr_t>(group->workspaceAt(index)));
      }
    }
    const bool reflowed = state.placed && structure != state.structure;
    state.structure = std::move(structure);
    if (reflowed) {
      beginReflow();
    }
    if (!state.placed || style != state.placedStyle || row != state.placedRow || input != state.placedInput) {
      state.placement = ov::placeAt(style, input, row);
      state.placedInput = std::move(input);
      state.placedStyle = style;
      state.placedRow = row;
      state.placed = true;
      state.placeOf.assign(state.cards.size(), nullptr);
      for (const ov::WindowPlace& place : state.placement.windows) {
        if (place.id < state.placeOf.size()) {
          state.placeOf[place.id] = &place;
        }
      }
    }
    const ov::Input& in = state.placedInput;
    const ov::Placement& placement = state.placement;
    const double p = m_progress;
    const ov::Rect output{0.0, 0.0, in.width, in.height};
    const auto toLayout = [&outputBox](ov::Rect rect) {
      rect.x += outputBox.x;
      rect.y += outputBox.y;
      return rect;
    };
    const auto toLocal = [&outputBox](ov::Rect rect) {
      rect.x -= outputBox.x;
      rect.y -= outputBox.y;
      return rect;
    };
    const WorkspaceGroup* group = state.output->workspaceGroup();

    state.cardsHorizontal = style != ov::Style::Strip || in.horizontal;
    state.workspaceViews.assign(state.workspaceBackgrounds.size(), WorkspaceView{});
    for (const ov::Card& card : placement.cards) {
      if (card.workspace >= state.workspaceViews.size()) {
        continue;
      }
      const Workspace* workspace = group != nullptr ? group->workspaceAt(card.workspace) : nullptr;
      const ov::Rect placed =
          workspace != nullptr ? toLocal(reflowRect(workspace, toLayout(card.rect), reflowed)) : card.rect;
      const wlr_box box = layoutBox(ov::lerpRect(ov::cardDeskRect(style, in, card, row), placed, p), outputBox);
      state.workspaceViews[card.workspace] = {
          .box = box,
          .fit =
              {static_cast<double>(box.width) / outputBox.width, static_cast<double>(box.x),
               static_cast<double>(box.y)},
          .kind = card.kind,
          .shown = true,
      };
    }
    state.mainBox.reset();
    if (placement.main.has_value() && placement.mainFit.has_value() && !state.workspaceViews.empty()) {
      const ov::Fit& fit = *placement.mainFit;
      state.mainWorkspace =
          static_cast<size_t>(std::clamp(std::lround(row), 0L, static_cast<long>(state.workspaceViews.size() - 1)));
      state.mainBox = layoutBox(ov::lerpRect(output, *placement.main, p), outputBox);
      state.mainFit = {1.0 + (fit.s - 1.0) * p, outputBox.x + fit.ox * p, outputBox.y + fit.oy * p};
    }

    const std::array<float, 4> backgroundColor = tint(config().colors.overview.workspaceBackground, m_progress);
    for (size_t index = 0; index < state.workspaceBackgrounds.size(); ++index) {
      const WorkspaceBackground& background = state.workspaceBackgrounds[index];
      const WorkspaceView& view = state.workspaceViews[index];
      wlr_scene_node_set_enabled(&background.tree->node, view.shown);
      if (!view.shown) {
        continue;
      }
      const double z = view.fit.s;
      const wlr_box& full = view.box;
      const int backgroundRadius = static_cast<int>(std::lround(config().appearance.cornerRadius * z));
      layoutWorkspaceBackground(background.fill, full, backgroundRadius, backgroundColor);
      if (background.mirrors.empty()) {
        continue;
      }
      // The card is the viewport onto the mirrored stack: a partially anchored surface must not reach past it.
      wlr_scene_tree_set_clip(background.tree, &full);
      for (const auto& mirror : background.mirrors) {
        wlr_box box{};
        const bool visible = desktopSourceBox(*mirror->source, box);
        wlr_scene_node_set_enabled(&mirror->buffer->node, visible);
        if (!visible) {
          continue;
        }
        wlr_scene_node_set_position(
            &mirror->buffer->node, full.x + static_cast<int>(std::lround(box.x * z)),
            full.y + static_cast<int>(std::lround(box.y * z))
        );
        const int destWidth = std::max(1, static_cast<int>(std::lround(box.width * z)));
        const int destHeight = std::max(1, static_cast<int>(std::lround(box.height * z)));
        wlr_scene_buffer_set_dest_size(mirror->buffer, destWidth, destHeight);
        // Lets the scene skip the background beneath a wallpaper that declares itself opaque.
        setScaledOpaqueRegion(
            mirror->buffer, mirror->source->surface->opaque_region, box.width, box.height, destWidth, destHeight
        );
        // Only a surface spanning the whole output takes the card's rounding; a smaller one keeps its own edges.
        const bool spansOutput =
            box.x <= 0 && box.y <= 0 && box.x + box.width >= outputBox.width && box.y + box.height >= outputBox.height;
        wlr_scene_buffer_set_corner_radii(mirror->buffer, corner_radii_all(spansOutput ? backgroundRadius : 0));
      }
    }

    const View* liveTarget = liveTargetView();
    for (size_t k = 0; k < state.cards.size(); ++k) {
      Card& card = *state.cards[k];
      const ov::WindowPlace* place = k < state.placeOf.size() ? state.placeOf[k] : nullptr;
      if (place == nullptr) {
        card.opacity = 0.0F;
        layoutCard(card, liveTarget);
        continue;
      }
      const wlr_box& world = card.view->presentedBox();
      const ov::Rect desk = ov::deskRect(in, localRect(world, outputBox), place->workspace, row);
      const ov::Rect placed = toLocal(reflowRect(card.view, toLayout(place->rect), reflowed));
      const ov::Rect rect = ov::lerpRect(desk, placed, p);
      if (&card != m_dragCard) {
        card.box = layoutBox(rect, outputBox);
      } else {
        // The pointer owns a dragged card: a drop reflows it from where it was let go.
        m_shownRects[card.view] = {static_cast<double>(card.box.x), static_cast<double>(card.box.y), rect.w, rect.h};
      }
      card.scale = world.width > 0 ? rect.w / world.width : 1.0;
      card.opacity = static_cast<float>(ov::windowOpacity(*place, card.view->minimized(), p));
      if (!m_filter.empty() && !cardMatches(card)) {
        card.opacity *= kFilterMissOpacity;
      }
      card.clip = {};
      if (place->clip.has_value()) {
        const ov::Insets& cut = *place->clip;
        const double z = card.scale;
        card.clip = {
            .x = static_cast<int>(std::lround(cut.left * z)),
            .y = static_cast<int>(std::lround(cut.top * z)),
            .width = static_cast<int>(std::lround((world.width - cut.left - cut.right) * z)),
            .height = static_cast<int>(std::lround((world.height - cut.top - cut.bottom) * z)),
        };
      }
      layoutCard(card, liveTarget);
    }
    layoutChrome(state, liveTarget);
  }

  void Overview::layoutChrome(OutputState& state, const View* liveTarget) {
    if (state.chrome == nullptr) {
      return;
    }
    const WorkspaceGroup* group = state.output->workspaceGroup();
    m_chromeWorkspaces.clear();
    for (size_t index = 0; index < state.workspaceViews.size(); ++index) {
      const WorkspaceView& view = state.workspaceViews[index];
      const Workspace* workspace = group != nullptr ? group->workspaceAt(index) : nullptr;
      m_chromeWorkspaces.push_back({
          .name = workspace != nullptr ? std::string_view(workspace->name()) : std::string_view(),
          .arrangement = workspace != nullptr ? arrangementLabel(workspace->layoutMode()) : std::string_view(),
          .card = view.box,
          .kind = view.kind,
          .shown = view.shown && workspace != nullptr,
      });
    }
    m_chromeWindows.clear();
    const auto addWindow = [&](const Card& card) {
      if (card.opacity <= 0.0F || card.view == nullptr) {
        return;
      }
      const char* title = card.view->title();
      wlr_box visible = card.box;
      if (card.clip.width > 0 && card.clip.height > 0) {
        visible = {card.box.x + card.clip.x, card.box.y + card.clip.y, card.clip.width, card.clip.height};
      }
      std::optional<wlr_box> within;
      const bool inMain = state.mainBox.has_value() && card.workspaceIndex == state.mainWorkspace;
      if (!inMain && card.workspaceIndex < state.workspaceViews.size()) {
        const WorkspaceView& holder = state.workspaceViews[card.workspaceIndex];
        // A thumb is a glance at the workspace; only full-size workspaces label their windows.
        if (holder.kind == ov::CardKind::Thumb) {
          return;
        }
        within = holder.box;
      }
      m_chromeWindows.push_back({
          .key = card.view,
          .title = title != nullptr ? std::string_view(title) : std::string_view(),
          .box = visible,
          .within = within,
          .opacity = card.opacity,
          .selected = card.view == liveTarget,
          .dragged = &card == m_dragCard,
      });
    };
    // Capsules stack the way their cards do: walk the output tree bottom to top. A dragged card lives outside it.
    wlr_scene_node* node = nullptr;
    wl_list_for_each(node, &state.tree->children, link) {
      for (const auto& card : state.cards) {
        if (card->tree != nullptr && &card->tree->node == node) {
          addWindow(*card);
          break;
        }
      }
    }
    if (m_dragCard != nullptr && m_dragCard->owner == &state) {
      addWindow(*m_dragCard);
    }
    const size_t current = m_chromeWorkspaces.empty()
        ? 0
        : static_cast<size_t>(
              std::clamp(std::lround(state.rowScroll.current()), 0L, static_cast<long>(m_chromeWorkspaces.size() - 1))
          );
    state.chrome->update({
        .style = state.placedStyle,
        .progress = m_progress,
        .output = state.outputBox,
        .usable = state.usableBox,
        .scale = std::max(1.0, std::ceil(static_cast<double>(state.output->wlr()->scale))),
        .placement = &state.placement,
        .current = current,
        .workspaces = m_chromeWorkspaces,
        .windows = m_chromeWindows,
        .titlesAlways = config().overview.titles == OverviewTitles::Always,
        .filter = m_filter,
        .hovered = m_hoverView,
        .closeHovered = m_closeHover,
        .hintHidden = m_edgeOutput == state.output && (m_edgeStep != 0 || m_edgePan != 0),
    });
    // Cards rebuilt after the chrome was made land above it.
    state.chrome->raise();
  }

  void Overview::beginReflow() {
    const auto& animation = config().animation;
    if (!animation.enabled || !animation.overview.enabled || !m_active) {
      m_reflowFrom.clear();
      m_reflow.snap(1.0);
      return;
    }
    // A reflow over one still running restarts from where everything is drawn, so nothing snaps back.
    for (auto& [key, from] : m_reflowFrom) {
      if (const auto shown = m_shownRects.find(key); shown != m_shownRects.end()) {
        from = shown->second;
      }
    }
    m_reflow.snap(0.0);
    const auto& curve = animation.overview.workspaceCurve;
    if (curve.easing == Easing::Spring) {
      m_reflow.settleSpring(1.0, curve.spring);
    } else {
      m_reflow.retarget(1.0, animation.overview.durationMs, curve);
    }
    scheduleFrames();
  }

  ov::Rect Overview::reflowRect(const void* key, const ov::Rect& target, bool began) {
    if (began) {
      // Only a rect that jumped glides; one following its window's own layout spring is already smooth.
      if (const auto shown = m_shownRects.find(key); shown != m_shownRects.end()) {
        const ov::Rect& last = shown->second;
        const double moved = std::max(
            {std::abs(last.x - target.x), std::abs(last.y - target.y), std::abs(last.w - target.w),
             std::abs(last.h - target.h)}
        );
        if (moved > 0.5) {
          m_reflowFrom[key] = last;
        }
      }
    }
    ov::Rect shown = target;
    if (m_reflow.animating()) {
      if (const auto from = m_reflowFrom.find(key); from != m_reflowFrom.end()) {
        shown = ov::lerpRect(from->second, target, m_reflow.current());
      }
    }
    m_shownRects[key] = shown;
    return shown;
  }

  void Overview::applyPinnedOpacity(float alpha) const {
    // A window unpinned while the overview is up becomes a card, so it returns to full opacity underneath.
    for (const auto& view : m_server->registry().all()) {
      view->setOverviewOpacity(view->pinned() ? alpha : 1.0F);
    }
  }

  View* Overview::liveTargetView() const {
    const Workspace* workspace = preferredWorkspace();
    return workspace != nullptr ? workspace->focusedView() : nullptr;
  }

  void Overview::syncCardEffects(Card& card) {
    View* view = card.view;
    Workspace* workspace = view->workspace();
    const BorderEffectGate cardGate{
        .focused = workspace != nullptr && workspace->focusedView() == view && &card != m_dragCard,
        .decorated = card.border != nullptr && card.border->node.enabled && card.tree->node.enabled,
        .urgent = view->urgent(),
        .fullscreen = view->scheduledFullscreen(),
    };
    view->syncAnimationEffects(
        card.tree, card.border != nullptr ? &card.border->node : nullptr, &card.surfaceTree->node, &cardGate,
        card.owner->output, static_cast<float>(card.scale)
    );
  }

  void Overview::refreshEffectBindings() {
    if (m_active) {
      syncCardEffects();
    }
  }

  void Overview::syncCardEffects() {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        syncCardEffects(*card);
      }
    }
  }

  std::array<float, 4> Overview::cardBorderColor(const Card& card, const View* liveTarget) const {
    const auto& border = card.view != nullptr ? card.view->borderColors() : config().colors.border;
    const Workspace* workspace = card.view != nullptr ? card.view->workspace() : nullptr;
    if (workspace == nullptr || workspace->focusedView() != card.view || &card == m_dragCard) {
      return border.unfocused;
    }
    // No window holds the seat while the overview is up, so exactly one card wears the focused color: the one a focus
    // or close action would act on. Every other row still marks the card it would land on, with a weaker mix.
    if (card.view == liveTarget) {
      return border.focused;
    }
    return mixColor(border.unfocused, border.focused, kLandingTargetBlend);
  }

  void Overview::applyProgress() {
    if (m_shortcutsDirty) {
      m_shortcutsDirty = false;
      updateShortcutAssignments();
    }
    for (const auto& state : m_outputs) {
      layoutOutput(*state);
    }
    // Pinned windows get no card: the real ones fade over the filmstrip instead of blinking at either end.
    const float chrome = desktopChromeAlpha();
    applyPinnedOpacity(chrome);
    wlr_scene_node_set_enabled(&m_server->pinnedTree()->node, chrome > 0.0F);
    scheduleFrames();
  }

  void Overview::scheduleFrames() const {
    for (const auto& state : m_outputs) {
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  // -: background mirrors

  void Overview::collectDesktopSurfaces(const Output& output, std::vector<DesktopEntry>& out) const {
    out.clear();
    if (!config().overview.workspaceWallpaper) {
      return;
    }
    // Scene child order is bottom to top, so walking the two layer trees in place reproduces what renders below the
    // windows. Children that are not a mapped layer surface (an unmap snapshot, for instance) have no surface to
    // mirror and drop out here.
    for (const uint32_t layer : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
      wlr_scene_tree* layerTree = output.layerTree(layer);
      if (layerTree == nullptr) {
        continue;
      }
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &layerTree->children, link) {
        if (child->type != WLR_SCENE_NODE_TREE || !child->enabled) {
          continue;
        }
        wlr_scene_tree* tree = wlr_scene_tree_from_node(child);
        for (const auto& candidate : m_server->layerSurfaces()) {
          if (!candidate->mapped() || candidate->scene() == nullptr || candidate->scene()->tree != tree) {
            continue;
          }
          wlr_surface* surface = candidate->layerSurface()->surface;
          if (surface->buffer == nullptr || surface->current.width <= 0 || surface->current.height <= 0) {
            break;
          }
          out.push_back({.surface = surface, .tree = tree});
          break;
        }
      }
    }
  }

  void Overview::clearDesktop(OutputState& state) const {
    state.desktop.clear();
    for (WorkspaceBackground& background : state.workspaceBackgrounds) {
      for (const auto& mirror : background.mirrors) {
        mirror->outputSample.disconnect();
        mirror->frameDone.disconnect();
        if (mirror->buffer != nullptr) {
          wlr_scene_node_destroy(&mirror->buffer->node);
        }
      }
      background.mirrors.clear();
    }
  }

  void Overview::refreshDesktop(OutputState& state) {
    std::vector<DesktopEntry> resolved;
    collectDesktopSurfaces(*state.output, resolved);
    bool same = resolved.size() == state.desktop.size();
    for (size_t index = 0; same && index < resolved.size(); ++index) {
      same = state.desktop[index]->surface == resolved[index].surface;
    }
    if (same) {
      // A surface keeps its scene tree, but the tree can be reparented between layers, so refresh the pointer the
      // mirror geometry reads.
      for (size_t index = 0; index < resolved.size(); ++index) {
        state.desktop[index]->tree = resolved[index].tree;
      }
      return;
    }

    clearDesktop(state);
    state.desktop.reserve(resolved.size());
    for (const DesktopEntry& entry : resolved) {
      auto source = std::make_unique<DesktopSurface>();
      source->overview = this;
      source->state = &state;
      source->surface = entry.surface;
      source->tree = entry.tree;
      source->commit.connect(&entry.surface->events.commit, source.get(), onDesktopSurfaceCommit);
      source->destroy.connect(&entry.surface->events.destroy, source.get(), onDesktopSurfaceDestroy);
      state.desktop.push_back(std::move(source));
    }
    for (WorkspaceBackground& background : state.workspaceBackgrounds) {
      createRowMirrors(state, background);
    }
    syncDesktopMirrors(state);
  }

  void Overview::createRowMirrors(OutputState& state, WorkspaceBackground& background) const {
    if (background.tree == nullptr) {
      return;
    }
    background.mirrors.reserve(state.desktop.size());
    // Created in stack order, above the fill: scene child order is what keeps a bottom-layer copy over the wallpaper.
    for (const auto& source : state.desktop) {
      wlr_scene_buffer* buffer = wlr_scene_buffer_create(background.tree, nullptr);
      if (buffer == nullptr) {
        continue;
      }
      wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
      buffer->point_accepts_input = rejectInput;
      auto mirror = std::make_unique<DesktopMirror>();
      mirror->source = source.get();
      mirror->buffer = buffer;
      mirror->outputSample.connect(&buffer->events.output_sample, mirror.get(), onDesktopMirrorOutputSample);
      mirror->frameDone.connect(&buffer->events.frame_done, mirror.get(), onDesktopMirrorFrameDone);
      background.mirrors.push_back(std::move(mirror));
    }
  }

  void Overview::syncDesktopMirrors(const OutputState& state) const {
    for (const WorkspaceBackground& background : state.workspaceBackgrounds) {
      for (const auto& mirror : background.mirrors) {
        wlr_surface* surface = mirror->source->surface;
        // The surface holds the authoritative committed buffer; a scene buffer may already have released the client
        // one after importing its texture.
        wlr_buffer* committed = surface->buffer != nullptr ? &surface->buffer->base : nullptr;
        wlr_scene_buffer_set_buffer_options options{
            .damage = committed != nullptr ? &surface->buffer_damage : nullptr,
            .wait_timeline = nullptr,
            .wait_point = 0,
        };
        if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
          options.wait_timeline = sync->acquire_timeline;
          options.wait_point = sync->acquire_point;
        }
        wlr_scene_buffer_set_buffer_with_options(mirror->buffer, committed, &options);
        if (committed == nullptr) {
          continue;
        }
        wlr_fbox src{};
        wlr_surface_get_buffer_source_box(surface, &src);
        wlr_scene_buffer_set_source_box(mirror->buffer, &src);
        wlr_scene_buffer_set_transform(mirror->buffer, surface->current.transform);
        // layoutOutput owns the destination geometry; only the color description follows the live node.
        if (wlr_scene_buffer* live = sourceBufferForSurface(&mirror->source->tree->node, surface)) {
          wlr_scene_buffer_copy_color(mirror->buffer, live);
        }
        if (ColorManager* colorManager = m_server->colorManager()) {
          colorManager->applySurfaceDescriptionToBuffer(surface, mirror->buffer);
        }
      }
    }
  }

  bool Overview::desktopSourceBox(const DesktopSurface& source, wlr_box& out) {
    // The output's layer trees sit at its origin and hold every layer surface as a direct child, so the child's own
    // offset is already output-local. Layout coordinates are unavailable here: the bottom layer is disabled while its
    // copies stand in, and wlr_scene_node_coords reports nothing through a disabled ancestor.
    if (!source.tree->node.enabled) {
      return false;
    }
    out = {source.tree->node.x, source.tree->node.y, source.surface->current.width, source.surface->current.height};
    return out.width > 0 && out.height > 0;
  }

  void Overview::onDesktopSurfaceCommit(DesktopSurface* source, void* /*data*/) {
    source->overview->syncDesktopMirrors(*source->state);
    source->overview->layoutOutput(*source->state);
    wlr_output_schedule_frame(source->state->output->wlr());
  }

  void Overview::onDesktopSurfaceDestroy(DesktopSurface* source, void* /*data*/) {
    OutputState* state = source->state;
    // Drop this surface's copies only. Re-resolving here would re-bind a surface that is already being destroyed;
    // the next layout notices the shorter stack and rebuilds from what is left.
    for (WorkspaceBackground& background : state->workspaceBackgrounds) {
      std::erase_if(background.mirrors, [source](const std::unique_ptr<DesktopMirror>& mirror) {
        if (mirror->source != source) {
          return false;
        }
        mirror->outputSample.disconnect();
        mirror->frameDone.disconnect();
        if (mirror->buffer != nullptr) {
          wlr_scene_node_destroy(&mirror->buffer->node);
        }
        return true;
      });
    }
    std::erase_if(state->desktop, [source](const std::unique_ptr<DesktopSurface>& candidate) {
      return candidate.get() == source;
    });
    wlr_output_schedule_frame(state->output->wlr());
  }

  void Overview::onDesktopMirrorOutputSample(DesktopMirror* mirror, void* data) {
    auto* event = static_cast<wlr_scene_output_sample_event*>(data);
    wlr_surface* surface = mirror->source->surface;
    // No wl_surface.enter here: a mirrored surface leaves its outputs exactly as a window behind a card does.
    if (event->direct_scanout) {
      wlr_presentation_surface_scanned_out_on_output(surface, event->output->output);
    } else {
      wlr_presentation_surface_textured_on_output(surface, event->output->output);
    }
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
        sync != nullptr && event->release_timeline != nullptr) {
      wlr_linux_drm_syncobj_v1_state_add_release_point(
          sync, event->release_timeline, event->release_point, event->output->output->event_loop
      );
    }
  }

  void Overview::onDesktopMirrorFrameDone(DesktopMirror* mirror, void* data) {
    auto* event = static_cast<wlr_scene_frame_done_event*>(data);
    // The real bottom layer is hidden, so these copies are what keeps its clients drawing. A second row's copy finds
    // the callback list already empty.
    wlr_surface_send_frame_done(mirror->source->surface, &event->when);
  }

  Overview::WorkspaceBackground Overview::createWorkspaceBackground(OutputState& state) const {
    WorkspaceBackground background{};
    background.tree = wlr_scene_tree_create(state.tree);
    if (background.tree == nullptr) {
      return background;
    }
    // Rows stay below every card even when created after the overview scene was populated.
    wlr_scene_node_place_above(&background.tree->node, &state.backgroundTint->node);
    const std::array<float, 4> color = tint(config().colors.overview.workspaceBackground, m_progress);
    background.fill = wlr_scene_rect_create(background.tree, 1, 1, color.data());
    createRowMirrors(state, background);
    return background;
  }

  // -: cards

  void Overview::syncCardBuffer(CardSurface& entry) {
    wlr_surface* surface = entry.surface;
    if (surface == nullptr || entry.buffer == nullptr) {
      return;
    }

    wlr_scene_buffer_set_buffer_options options{
        .damage = &surface->buffer_damage,
        .wait_timeline = nullptr,
        .wait_point = 0,
    };
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
      options.wait_timeline = sync->acquire_timeline;
      options.wait_point = sync->acquire_point;
    }
    // A scene buffer may clear its `buffer` pointer after importing a texture and releasing the client buffer. The
    // surface retains the authoritative committed buffer, including for hidden workspaces.
    wlr_buffer* committed = surface->buffer != nullptr ? &surface->buffer->base : nullptr;
    if (committed == nullptr) {
      options.damage = nullptr;
    }
    wlr_scene_buffer_set_buffer_with_options(entry.buffer, committed, &options);

    // Presentation state comes from the surface's committed state, NEVER from the view's scene buffers: workspace
    // slides clip those (applyPresentation → applyPresentedCrop), and a window parked on a hidden workspace keeps the
    // sliver crop. Copying it smears subsurface-presented content (games) into a single stretched line. layoutCard then
    // re-crops the root surface and re-scales every entry for the thumbnail.
    wlr_fbox src{};
    wlr_surface_get_buffer_source_box(surface, &src);
    wlr_scene_buffer_set_source_box(entry.buffer, &src);
    int width = 0;
    int height = 0;
    viewSurfaceSize(entry.sourceBuffer, surface, &width, &height);
    wlr_scene_buffer_set_dest_size(entry.buffer, width, height);
    wlr_scene_buffer_set_transform(entry.buffer, surface->current.transform);
    // A window whose rule ignores the opaque region shows none on its own buffer; the card follows it.
    pixman_region32_t none;
    pixman_region32_init(&none);
    const bool ignored = entry.sourceBuffer != nullptr
        && pixman_region32_empty(&entry.sourceBuffer->opaque_region)
        && pixman_region32_not_empty(&surface->opaque_region);
    wlr_scene_buffer_set_opaque_region(entry.buffer, ignored ? &none : &surface->opaque_region);
    pixman_region32_fini(&none);

    // Protocol-derived display properties (alpha-modifier, color management)
    // are clip-independent, so the view's scene buffer is a safe source.
    if (wlr_scene_buffer* source = entry.sourceBuffer) {
      wlr_scene_buffer_set_opacity(entry.buffer, source->opacity);
      wlr_scene_buffer_copy_color(entry.buffer, source);
    }
    // Scene surfaces reset their color state on every commit. This passive
    // mirror may copy that state before the render boundary repairs it, so
    // apply the authoritative committed description directly to the mirror.
    if (ColorManager* colorManager = entry.card->overview->m_server->colorManager()) {
      colorManager->applySurfaceDescriptionToBuffer(surface, entry.buffer);
    }
  }

  void Overview::forEachCardSurface(Card* card, void (*visit)(wlr_surface*, int, int, void*)) {
    View* view = card->view;
    wlr_surface* root = view->rootSurface();
    if (root == nullptr) {
      return;
    }
    CardWalk own{.card = card, .view = view};
    wlr_surface_for_each_surface(root, visit, &own);
    if (view->attachments().empty()) {
      return;
    }
    // A dialog hangs in the window's frame, so the scene holds both at their real offset.
    wlr_scene_buffer* rootBuffer = sourceBufferForSurface(&view->sceneTree()->node, root);
    if (rootBuffer == nullptr) {
      return;
    }
    int rootX = 0;
    int rootY = 0;
    wlr_scene_node_coords(&rootBuffer->node, &rootX, &rootY);
    const auto walk = [&](const auto& self, const View* parent) -> void {
      for (View* dialog : parent->attachments()) {
        wlr_surface* surface = dialog->rootSurface();
        wlr_scene_buffer* buffer =
            surface != nullptr ? sourceBufferForSurface(&dialog->sceneTree()->node, surface) : nullptr;
        if (buffer != nullptr) {
          int x = 0;
          int y = 0;
          wlr_scene_node_coords(&buffer->node, &x, &y);
          CardWalk offset{.card = card, .view = dialog, .dx = x - rootX, .dy = y - rootY};
          wlr_surface_for_each_surface(surface, visit, &offset);
        }
        self(self, dialog);
      }
    };
    walk(walk, view);
  }

  void Overview::addCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    const auto* walk = static_cast<const CardWalk*>(data);
    Card* card = walk->card;
    wlr_scene_buffer* source = sourceBufferForSurface(&walk->view->sceneTree()->node, surface);
    if (source == nullptr) {
      return;
    }
    wlr_scene_buffer* buffer = wlr_scene_buffer_create(card->surfaceTree, nullptr);
    if (buffer == nullptr) {
      return;
    }
    auto entry = std::make_unique<CardSurface>();
    entry->card = card;
    entry->view = walk->view;
    entry->surface = surface;
    entry->sourceBuffer = source;
    entry->buffer = buffer;
    entry->sx = sx + walk->dx;
    entry->sy = sy + walk->dy;
    entry->isRoot = surface == card->view->rootSurface();
    wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
    buffer->point_accepts_input = rejectInput;
    entry->commit.connect(&surface->events.commit, entry.get(), onCardSurfaceCommit);
    entry->destroy.connect(&surface->events.destroy, entry.get(), onCardSurfaceDestroy);
    entry->outputSample.connect(&buffer->events.output_sample, entry.get(), onCardBufferOutputSample);
    entry->frameDone.connect(&buffer->events.frame_done, entry.get(), onCardBufferFrameDone);
    syncCardBuffer(*entry);
    card->surfaces.push_back(std::move(entry));
  }

  void Overview::syncCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    const auto* walk = static_cast<const CardWalk*>(data);
    for (const auto& entry : walk->card->surfaces) {
      if (entry->surface == surface) {
        // A surface given a new subsurface role has a new scene node; the old one went with the old role.
        entry->sourceBuffer = sourceBufferForSurface(&walk->view->sceneTree()->node, surface);
        if (entry->sourceBuffer == nullptr) {
          return;
        }
        entry->inTree = true;
        entry->sx = sx + walk->dx;
        entry->sy = sy + walk->dy;
        syncCardBuffer(*entry);
        return;
      }
    }
    addCardSurface(surface, sx, sy, data);
  }

  void Overview::syncCardSurfaces(Card* card) {
    for (const auto& entry : card->surfaces) {
      entry->inTree = false;
    }
    forEachCardSurface(card, syncCardSurface);
  }

  void Overview::onCardSurfaceCommit(CardSurface* entry, void* /*data*/) {
    Card* card = entry->card;
    Overview* self = card->overview;
    if (!self->m_active || card->owner == nullptr) {
      return;
    }
    // The source scene surface reconfigures on every commit. Refresh the
    // passive buffer mirrors, then re-derive their overview crop and scale.
    syncCardSurfaces(card);
    self->layoutCard(*card, self->liveTargetView());
    wlr_output_schedule_frame(card->owner->output->wlr());
  }

  void Overview::onCardSurfaceDestroy(CardSurface* entry, void* /*data*/) {
    Card* card = entry->card;
    entry->outputSample.disconnect();
    entry->frameDone.disconnect();
    if (entry->buffer != nullptr) {
      wlr_scene_node_destroy(&entry->buffer->node);
      entry->buffer = nullptr;
    }
    std::erase_if(card->surfaces, [entry](const std::unique_ptr<CardSurface>& candidate) {
      return candidate.get() == entry;
    });
  }

  void Overview::onCardBufferOutputSample(CardSurface* entry, void* data) {
    auto* event = static_cast<wlr_scene_output_sample_event*>(data);
    wlr_output* output = event->output->output;
    if (event->direct_scanout) {
      wlr_presentation_surface_scanned_out_on_output(entry->surface, output);
    } else {
      wlr_presentation_surface_textured_on_output(entry->surface, output);
    }
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(entry->surface);
        sync != nullptr && event->release_timeline != nullptr) {
      wlr_linux_drm_syncobj_v1_state_add_release_point(
          sync, event->release_timeline, event->release_point, output->event_loop
      );
    }
  }

  void Overview::onCardBufferFrameDone(CardSurface* entry, void* data) {
    auto* event = static_cast<wlr_scene_frame_done_event*>(data);
    wlr_surface_send_frame_done(entry->surface, &event->when);
  }

  Overview::Card* Overview::createCard(OutputState& state, View* view, size_t workspaceIndex) {
    // An attached dialog shows over its parent's card instead of on one of its own.
    if (view == nullptr || !view->mapped() || view->pinned() || view->attached()) {
      return nullptr;
    }
    wlr_surface* surface = view->rootSurface();
    if (surface == nullptr) {
      return nullptr;
    }
    auto card = std::make_unique<Card>();
    card->overview = this;
    card->owner = &state;
    card->view = view;
    card->workspaceIndex = workspaceIndex;
    card->tree = wlr_scene_tree_create(state.tree);
    if (card->tree == nullptr) {
      return nullptr;
    }
    card->surfaceTree = wlr_scene_tree_create(card->tree);
    if (card->surfaceTree == nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      return nullptr;
    }
    const std::array<float, 4> innerColor = tint(view->borderColors().unfocused, 1.0);
    const std::array<float, 4> outerColor = tint(view->borderColors().outer, 1.0);
    card->border = wlr_scene_border_create(card->tree, innerColor.data(), outerColor.data());
    if (card->border == nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      return nullptr;
    }
    Card* raw = card.get();
    state.cards.push_back(std::move(card));
    m_shortcutsDirty = true;

    forEachCardSurface(raw, addCardSurface);

    // Animation schedules the first output frame. The passive card buffers then
    // pace clients from frames where their content was actually sampled.
    m_server->scheduleSuspensionSync();
    return raw;
  }

  void Overview::snapshotCardForClose(Card& card) {
    if (card.tree == nullptr || card.owner == nullptr || card.owner->output == nullptr || card.view == nullptr) {
      return;
    }

    wlr_scene_tree* snapshot = wlr_scene_tree_create(m_tree);
    if (snapshot == nullptr) {
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), card.owner->output->wlr(), &outputBox);
    if (outputBox.width > 0 && outputBox.height > 0) {
      wlr_scene_tree_set_clip(snapshot, &outputBox);
    }

    // The copied surfaces keep the card's window and overlay slots, with time frozen, on a tree of their own.
    wlr_scene_tree* surfaces = wlr_scene_tree_create(snapshot);
    if (surfaces == nullptr) {
      wlr_scene_node_destroy(&snapshot->node);
      return;
    }
    wlr_scene_node_set_position(
        &surfaces->node, card.tree->node.x + card.surfaceTree->node.x, card.tree->node.y + card.surfaceTree->node.y
    );
    int buffersCopied = 0;
    for (const auto& entry : card.surfaces) {
      wlr_scene_buffer* source = entry->buffer;
      if (source == nullptr || source->buffer == nullptr || !source->node.enabled) {
        continue;
      }
      wlr_scene_buffer* copy = wlr_scene_buffer_create(surfaces, source->buffer);
      if (copy == nullptr) {
        continue;
      }
      wlr_scene_node_set_position(&copy->node, source->node.x, source->node.y);
      if (source->dst_width > 0 && source->dst_height > 0) {
        wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      }
      if (source->src_box.width > 0 && source->src_box.height > 0) {
        wlr_scene_buffer_set_source_box(copy, &source->src_box);
      }
      wlr_scene_buffer_set_transform(copy, source->transform);
      wlr_scene_buffer_set_corner_radii(copy, source->corners);
      wlr_scene_buffer_set_corner_box(copy, &source->corner_box);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_node_set_clip_region(&copy->node, wlr_scene_node_get_clip_region(&source->node));
      wlr_scene_buffer_copy_color(copy, source);
      if (ColorManager* colorManager = m_server->colorManager()) {
        colorManager->applySurfaceDescriptionToBuffer(entry->surface, copy);
      }
      wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
      ++buffersCopied;
    }
    if (buffersCopied == 0) {
      wlr_scene_node_destroy(&surfaces->node);
    } else if (card.view->effects().needsSurface()) {
      wlr_scene_node_copy_animations_for_snapshot(&surfaces->node, &card.surfaceTree->node);
    }

    if (const auto* frame = dynamic_cast<const SsdFrame*>(card.frame.get())) {
      auto* chrome = wlr_scene_tree_create(snapshot);
      wlr_scene_node_set_position(&chrome->node, card.tree->node.x, card.tree->node.y);
      frame->snapshot(chrome);
    }
    std::vector<BorderSnapshot> borders;
    if (card.border != nullptr && card.border->node.enabled) {
      snapshotBorder(
          snapshot, *card.border, card.tree->node.x + card.border->node.x, card.tree->node.y + card.border->node.y,
          &card.border->node,
          {
              .innerColor = cardBorderColor(card, liveTargetView()),
              .outerColor = card.view->borderColors().outer,
              .innerWidth = card.view->decorationBorderWidth(),
              .outerWidth = card.view->decorationOuterBorderWidth(),
              .cornerRadius = card.view->decorationCornerRadius(),
              .padding = card.borderPadding,
          },
          card.view->presentedOpacity(), borders
      );
    }

    if (buffersCopied == 0 && borders.empty()) {
      wlr_scene_node_destroy(&snapshot->node);
      return;
    }
    wlr_scene_node_copy_animations_for_snapshot(&snapshot->node, &card.tree->node);
    // The frozen card owns its captured geometry and windows_out lifecycle. Retain an interrupted windows_in effect,
    // but do not carry the live card's windows_move effect into the close snapshot.
    wlr_scene_node_set_animation(&snapshot->node, static_cast<unsigned>(AnimationEvent::WindowsMove), nullptr, nullptr);
    (void)m_server->animateCloseSnapshot(card.owner->output, snapshot, snapshot, std::move(borders), {});
    wlr_output_schedule_frame(card.owner->output->wlr());
  }

  void Overview::destroyCard(Card* card) {
    card->view->effects().detachNodes(
        card->surfaceTree != nullptr ? &card->surfaceTree->node : nullptr,
        card->border != nullptr ? &card->border->node : nullptr
    );
    card->surfaces.clear();
    // The chrome copy's nodes hang under the card tree, so it goes before the tree does.
    card->chrome.reset();
    card->frame.reset();
    destroyCardShadow(*card);
    if (card->tree != nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      card->tree = nullptr;
      card->surfaceTree = nullptr;
    }
    card->border = nullptr;
    m_server->scheduleSuspensionSync();
  }

  void Overview::dropCard(View* view) {
    for (const auto& state : m_outputs) {
      const auto it =
          std::ranges::find_if(state->cards, [view](const std::unique_ptr<Card>& card) { return card->view == view; });
      if (it == state->cards.end()) {
        continue;
      }
      destroyCard(it->get());
      state->cards.erase(it);
      m_shortcutsDirty = true;
      return;
    }
  }

  void Overview::rebuildCard(View* view) {
    dropCard(view);
    Workspace* workspace = view->workspace();
    if (workspace == nullptr || !view->mapped()) {
      return;
    }
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr) {
      return;
    }
    createCard(*state, view, workspace->index());
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
  }

  Overview::OutputState* Overview::stateFor(const Output* output) {
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    return it == m_outputs.end() ? nullptr : it->get();
  }

  Overview::OutputState* Overview::stateForWorkspace(const Workspace* workspace) {
    if (workspace == nullptr || workspace->group() == nullptr) {
      return nullptr;
    }
    return stateFor(workspace->group()->output());
  }

  Overview::Card* Overview::findCard(const View* view) {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->view == view) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  bool Overview::presentsLive(const View* view) const {
    if (!m_active || view->presentationSuppressed()) {
      return false;
    }
    return std::ranges::any_of(m_outputs, [view](const auto& state) {
      return std::ranges::any_of(state->cards, [view](const auto& card) { return card->view == view; });
    });
  }

  void Overview::renderCardShortcut(Card& card) {
    if (card.badge != nullptr) {
      wlr_scene_node_destroy(&card.badge->node);
    }
    card.badge = nullptr;
    card.badgeRect = nullptr;
    card.badgeText = nullptr;
    card.badgeWidth = 0;
    card.badgeHeight = 0;

    if (card.shortcut.empty() || card.tree == nullptr || card.owner == nullptr || card.owner->output == nullptr) {
      return;
    }

    const double scale = std::max(1.0, std::ceil(static_cast<double>(card.owner->output->wlr()->scale)));
    const size_t matched = card.shortcutMatched == SIZE_MAX ? 0 : std::min(card.shortcutMatched, card.shortcut.size());
    const std::string_view shortcut(card.shortcut);
    const auto& colors = config().colors;
    const std::array<float, 4>& badgeColor = colors.overview.badge;
    card.badgeBackground = keycapBackgroundColor(colors.background, badgeColor);
    const std::string markup = std::format(
        "<span foreground='{}' weight='bold'>{}</span><span foreground='{}' weight='bold'>{}</span>",
        rgbaHex(colors.textPrimary), escapeMarkup(shortcut.substr(0, matched)), rgbaHex(badgeColor),
        escapeMarkup(shortcut.substr(matched))
    );
    TextBufferResult rendered = renderTextBuffer({
        .markup = markup,
        .font = "monospace 19",
        .maxWidth = 350,
        .padding = 0,
        .scale = scale,
        .bgA = 0.0,
    });
    if (rendered.buffer == nullptr) {
      return;
    }

    card.badge = wlr_scene_tree_create(card.tree);
    if (card.badge == nullptr) {
      wlr_buffer_drop(rendered.buffer);
      return;
    }
    // Keycap proportions: the label's line box sets the height, and the badge is
    // never narrower than it is tall, so a single character reads as a square.
    constexpr int kBadgeSidePad = 8;
    const int badgeHeight = rendered.logicalHeight;
    const int badgeWidth = std::max(rendered.logicalWidth + 2 * kBadgeSidePad, badgeHeight);
    const std::array<float, 4> background = tint(card.badgeBackground, 1.0);
    card.badgeRect = wlr_scene_rect_create(card.badge, badgeWidth, badgeHeight, background.data());
    card.badgeText = wlr_scene_buffer_create(card.badge, rendered.buffer);
    wlr_buffer_drop(rendered.buffer);
    if (card.badgeRect == nullptr || card.badgeText == nullptr) {
      wlr_scene_node_destroy(&card.badge->node);
      card.badge = nullptr;
      card.badgeRect = nullptr;
      card.badgeText = nullptr;
      return;
    }

    wlr_scene_node_set_position(
        &card.badgeText->node, (badgeWidth - rendered.logicalWidth) / 2, (badgeHeight - rendered.logicalHeight) / 2
    );
    wlr_scene_buffer_set_dest_size(card.badgeText, rendered.logicalWidth, rendered.logicalHeight);
    card.badgeText->point_accepts_input = rejectInput;
    card.badgeWidth = badgeWidth;
    card.badgeHeight = badgeHeight;
    wlr_scene_node_raise_to_top(&card.badge->node);
    wlr_scene_node_set_enabled(&card.badge->node, false);
  }

  void Overview::assignShortcuts() {
    m_shortcutInput.clear();
    m_shortcutsDirty = true;
    applyProgress();
  }

  void Overview::updateShortcutAssignments() {
    const auto updateCard = [this](Card& card, std::string label) {
      const bool changed = card.shortcut != label;
      card.shortcut = std::move(label);
      card.shortcutMatched = 0;
      if (changed) {
        renderCardShortcut(card);
      }
    };
    const auto clearAll = [&]() {
      for (const auto& state : m_outputs) {
        for (const auto& card : state->cards) {
          updateCard(*card, {});
        }
      }
    };

    if (!m_active || m_closing || !config().overview.shortcuts || config().overview.shortcutKeys.size() < 2) {
      clearAll();
      return;
    }

    std::vector<OutputState*> orderedStates;
    orderedStates.reserve(m_outputs.size());
    Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    if (OutputState* state = stateFor(preferred)) {
      orderedStates.push_back(state);
    }
    for (const auto& state : m_outputs) {
      if (orderedStates.empty() || state.get() != orderedStates.front()) {
        orderedStates.push_back(state.get());
      }
    }

    // Badges follow the settled placement at the row the overview is heading to, read from layout targets, so a
    // window still sliding into place already has its final order.
    std::vector<Card*> eligible;
    for (OutputState* state : orderedStates) {
      if (state == nullptr || !refreshOutputBoxes(*state)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      const double row = state->rowScroll.target();
      const ov::Input input = placementInput(*state, true);
      const ov::Placement settled = ov::placeAt(configuredStyle(), input, row);
      const ov::Rect output{0.0, 0.0, input.width, input.height};
      const auto onOutput = [&output](const ov::Rect& rect) {
        return rect.x < output.w && rect.y < output.h && rect.x + rect.w > 0 && rect.y + rect.h > 0;
      };
      const size_t chosen = static_cast<size_t>(std::max(0L, std::lround(row)));

      std::vector<size_t> visible;
      visible.reserve(group->workspaceCount());
      for (const ov::Card& card : settled.cards) {
        if (card.workspace < group->workspaceCount() && (onOutput(card.rect) || card.workspace == chosen)) {
          visible.push_back(card.workspace);
        }
      }
      std::ranges::stable_sort(visible, [row](size_t left, size_t right) {
        const double leftDistance = std::abs(static_cast<double>(left) - row);
        const double rightDistance = std::abs(static_cast<double>(right) - row);
        return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
      });

      struct PositionedCard {
        Card* card = nullptr;
        double x = 0;
        double y = 0;
      };
      // Reading order inside each workspace: rows of a horizontal workspace axis, columns otherwise.
      const bool horizontalWorkspaces = input.horizontal;
      for (const size_t index : visible) {
        std::vector<PositionedCard> previewCards;
        for (size_t k = 0; k < state->cards.size(); ++k) {
          Card* card = state->cards[k].get();
          if (card->workspaceIndex != index || card->view == nullptr || !card->view->mapped()) {
            continue;
          }
          const ov::WindowPlace* place = ov::findWindow(settled, static_cast<uint32_t>(k));
          if (place == nullptr || place->opacity <= 0.0) {
            continue;
          }
          previewCards.push_back({.card = card, .x = std::round(place->rect.x), .y = std::round(place->rect.y)});
        }
        std::ranges::stable_sort(
            previewCards, [horizontalWorkspaces](const PositionedCard& left, const PositionedCard& right) {
              if (horizontalWorkspaces) {
                return left.y == right.y ? left.x < right.x : left.y < right.y;
              }
              return left.x == right.x ? left.y < right.y : left.x < right.x;
            }
        );
        for (const PositionedCard& positioned : previewCards) {
          eligible.push_back(positioned.card);
        }
      }
    }

    const auto assignmentFor = [this](const View* view) -> ShortcutAssignment* {
      const auto assignment = std::ranges::find_if(m_shortcutAssignments, [view](const ShortcutAssignment& item) {
        return item.view == view;
      });
      return assignment == m_shortcutAssignments.end() ? nullptr : &*assignment;
    };
    for (Card* card : eligible) {
      if (assignmentFor(card->view) == nullptr) {
        m_shortcutAssignments.push_back({.view = card->view, .label = {}});
      }
    }

    m_shortcutLabelCapacity = std::max(m_shortcutLabelCapacity, m_shortcutAssignments.size());
    const std::vector<std::string> labels = shortcutLabels(m_shortcutLabelCapacity, config().overview.shortcutKeys);
    std::vector<bool> used(labels.size(), false);
    const auto labelIndex = [&labels](std::string_view label) {
      for (size_t index = 0; index < labels.size(); ++index) {
        if (labels[index] == label) {
          return index;
        }
      }
      return labels.size();
    };

    for (ShortcutAssignment& assignment : m_shortcutAssignments) {
      const size_t index = labelIndex(assignment.label);
      if (index == labels.size() || used[index]) {
        assignment.label.clear();
      } else {
        used[index] = true;
      }
    }
    size_t nextUnused = 0;
    for (ShortcutAssignment& assignment : m_shortcutAssignments) {
      if (!assignment.label.empty()) {
        continue;
      }
      while (nextUnused < used.size() && used[nextUnused]) {
        ++nextUnused;
      }
      if (nextUnused == labels.size()) {
        break;
      }
      assignment.label = labels[nextUnused];
      used[nextUnused] = true;
    }

    for (Card* card : eligible) {
      ShortcutAssignment* assignment = assignmentFor(card->view);
      updateCard(*card, assignment != nullptr ? assignment->label : std::string{});
    }
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (std::ranges::find(eligible, card.get()) == eligible.end()) {
          updateCard(*card, {});
        }
      }
    }
  }

  void Overview::populateCards(OutputState& state) {
    WorkspaceGroup* group = state.output->workspaceGroup();
    if (group == nullptr) {
      return;
    }
    for (size_t row = 0; row < group->workspaceCount(); ++row) {
      Workspace* workspace = group->workspaceAt(row);
      if (workspace == nullptr) {
        continue;
      }
      // Tiled, then floating, then fullscreen: mirrors the per-workspace scene
      // layer split so overlapping cards stack the way the real windows do.
      for (int pass = 0; pass < 3; ++pass) {
        for (View* view : workspace->allViews()) {
          if (view == nullptr || !view->mapped() || view->pinned()) {
            continue;
          }
          const bool fullscreen = view->currentFullscreen();
          const int layer = fullscreen ? 2 : (view->tiled() ? 0 : 1);
          if (layer == pass) {
            createCard(state, view, row);
          }
        }
      }
    }
  }

  void Overview::buildState() {
    ANDROMEDA_ZONE("Overview::buildState");
    m_tree = m_server->overviewTree();
    for (const auto& output : m_server->outputs()) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr || group->workspaceCount() == 0 || !output->onDesktop()) {
        continue;
      }
      auto state = std::make_unique<OutputState>();
      state->output = output.get();
      state->tree = wlr_scene_tree_create(m_tree);
      if (state->tree == nullptr) {
        continue;
      }
      if (config().overview.backgroundBlur && config().appearance.blur.enabled) {
        state->backgroundBlur = wlr_scene_blur_create(m_server->overviewBlurTree(), 1, 1);
        if (state->backgroundBlur != nullptr) {
          wlr_scene_blur_set_should_only_blur_bottom_layer(state->backgroundBlur, config().appearance.blur.optimized);
        }
      }
      const std::array<float, 4> backgroundTint = tint(config().colors.overview.backgroundTint, 0.0);
      state->backgroundTint = wlr_scene_rect_create(state->tree, 1, 1, backgroundTint.data());
      wlr_scene_rect_set_corner_radius(state->backgroundTint, 0);
      state->workspaceBackgrounds.reserve(group->workspaceCount());
      for (size_t row = 0; row < group->workspaceCount(); ++row) {
        state->workspaceBackgrounds.push_back(createWorkspaceBackground(*state));
      }
      state->tileShadows = wlr_scene_tree_create(state->tree);
      state->activeWorkspaceIndex = group->active() != nullptr ? group->active()->index() : 0;
      state->rowScroll.snap(static_cast<double>(state->activeWorkspaceIndex));
      OutputState* raw = state.get();
      m_outputs.push_back(std::move(state));
      populateCards(*raw);
      raw->chrome = std::make_unique<ov::Chrome>(raw->tree);
    }
  }

  // -: open/close

  bool Overview::beginPresentation() {
    ANDROMEDA_ZONE("Overview::beginPresentation");
    if (m_active) {
      return true;
    }
    if (m_server->sessionLocked()) {
      return false;
    }
    // A data-device drag owns wlroots' pointer and keyboard grabs until the initiating button is released. Taking
    // overview input ownership now would hide that release from the drag and leave both grabs active.
    if (m_server->seat()->wlr()->drag != nullptr) {
      kLog.debug("overview open ignored during active client drag");
      return false;
    }
    m_server->cursor()->resetMode();
    // An in-flight three-finger switch settles on the filmstrip instead of the hidden desktop slide.
    const Gestures::SwitchPick switchPick = m_server->gestures()->pickSwitchForOverview();
    for (const auto& output : m_server->outputs()) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      group->slideFinish();
      // Settle the visual scroll onto the layout scroll so cards at progress 0
      // sit exactly where the real windows are.
      if (Workspace* workspace = group->active()) {
        workspace->arrange(false);
      }
    }
    // Before buildState: reconcileDynamic must not invalidate cards mid-build.
    if (switchPick.group != nullptr) {
      switchPick.group->activate(switchPick.target, false);
    }

    buildState();
    if (m_outputs.empty()) {
      return false;
    }
    m_server->cursor()->resetWheelAccumulation();

    if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
      scratchpad->hideAll();
    }

    m_active = true;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pendingFocus = nullptr;
    m_pointerOutput = m_server->outputFromWlr(m_server->preferredOutput());
    m_server->notifyOverviewChanged();

    // Initialize every View's canonical presentation box before cards consume it. Hidden workspaces normally skip
    // scene presentation entirely, but while overview is active their hidden nodes carry the same position and size
    // state that would be shown if the workspace were active.
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        for (size_t index = 0; index < group->workspaceCount(); ++index) {
          if (Workspace* workspace = group->workspaceAt(index)) {
            workspace->arrange(false);
          }
        }
      }
    }
    if (switchPick.group != nullptr) {
      if (OutputState* state = stateFor(switchPick.group->output())) {
        // Read the row after activate: reconcileDynamic may have dropped the workspace the swipe left.
        const auto row = static_cast<double>(switchPick.target->index());
        state->rowScroll.snap(row + switchPick.offset);
        animateRow(*state, row, switchPick.velocity);
      }
    }
    assignShortcuts();

    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, false);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, false);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    // Bottom-layer surfaces are mirrored into every row, so the real ones step aside the way windows do. The
    // background layer stays: it is the blur source and what shows around the filmstrip.
    if (config().overview.workspaceWallpaper) {
      wlr_scene_node_set_enabled(&m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node, false);
    }

    m_server->clearKeyboardFocus();
    m_server->cursor()->clearPointerFocus();
    m_server->cursor()->clearConstraint();

    applyProgress();
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, true);
    for (const auto& state : m_outputs) {
      state->output->markBlurBackgroundDirty();
    }
    kLog.debug("overview opened on {} output(s)", m_outputs.size());
    return true;
  }

  void Overview::open() {
    ANDROMEDA_ZONE("Overview::open");
    if (m_active) {
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
        scratchpad->hideAll();
      }
      m_closing = false;
      m_server->notifyOverviewChanged();
      m_pendingFocus = nullptr;
      if (m_progress < 1.0 || m_targetProgress < 1.0) {
        startAnimation(1.0, false);
      }
      return;
    }
    if (!beginPresentation()) {
      return;
    }
    startAnimation(1.0, false);
  }

  void Overview::toggle() {
    if (m_active && !m_closing) {
      close();
    } else {
      open();
    }
  }

  void Overview::close() { beginClose(nullptr); }

  void Overview::closeToWorkspace(Workspace* workspace, View* focus) {
    if (!m_active || m_closing) {
      return;
    }
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->active() != workspace) {
      // No slide: the real trees are hidden, the filmstrip is the transition.
      workspace->group()->activate(workspace, false);
    }
    if (focus != nullptr && focus->mapped()) {
      // Overview focus keeps keyboard input withheld, but it updates the workspace and starts any scrolling-column
      // reveal. Begin it before the closing zoom so both animations receive their first tick together. finishAnimation
      // repeats the focus after teardown to deliver keyboard focus once the real trees own input again.
      m_server->focusView(focus, FocusReason::PointerPress);
      if (focus->workspace() != nullptr && focus->workspace()->layoutConfig().scrolling.centerOnOverviewSelect) {
        focus->workspace()->centerFocusedColumn();
      }
    }
    beginClose(focus);
  }

  void Overview::beginClose(View* focus, double releaseVelocity) {
    if (!m_active || m_closing) {
      return;
    }
    m_server->cursor()->resetWheelAccumulation();
    // A close releases any navigation mid-gesture. Input events carry monotonic milliseconds, so the release sample
    // shares their clock and bleeds the speed of fingers that came to rest before the close.
    endNavigation(false, monotonicMsec(), m_navigationSource);
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    endThumbDrag(false, 0, 0);
    hideDropHint();
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_hoverView = nullptr;
    m_closeHover = false;
    m_closePress = nullptr;
    clearMiddlePress();
    m_pendingFocus = focus;
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output->workspaceGroup();
      if (group != nullptr && group->active() != nullptr) {
        animateRow(*state, static_cast<double>(group->active()->index()));
      }
    }
    startAnimation(0.0, true, releaseVelocity);
  }

  void Overview::forceClose() {
    if (!m_active) {
      return;
    }
    m_zoomAnim.snap(0.0);
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    m_pendingFocus = nullptr;
    teardown();
    restoreFocus(nullptr);
  }

  void Overview::startAnimation(double target, bool closing, double releaseVelocity) {
    m_closing = closing;
    m_server->notifyOverviewChanged();
    m_targetProgress = target;
    m_progressFrom = m_progress;
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      m_zoomAnim.snap(1.0);
      m_progress = target;
      for (const auto& state : m_outputs) {
        state->rowScroll.snap(state->rowScroll.target());
      }
      finishAnimation();
      return;
    }
    m_zoomAnim.snap(0.0);
    if (overview.curve.easing == Easing::Spring) {
      // Physics mode, so the tail can end once it no longer moves a pixel. The zoom runs the span from where the
      // gesture left off to the target, so a release speed given in progress per second is that speed over this span.
      const double span = m_targetProgress - m_progressFrom;
      m_zoomAnim.settleSpring(1.0, overview.curve.spring, span != 0.0 ? releaseVelocity / span : 0.0);
    } else {
      m_zoomAnim.retarget(1.0, overview.durationMs, overview.curve);
    }
    // Animations only tick from an output frame; kick one so an idle desktop starts the zoom.
    scheduleFrames();
  }

  void Overview::animateRow(OutputState& state, double target, double releaseVelocity) {
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      state.rowScroll.snap(target);
      applyProgress();
      return;
    }
    if (overview.workspaceCurve.easing == Easing::Spring) {
      state.rowScroll.settleSpring(target, overview.workspaceCurve.spring, releaseVelocity);
    } else {
      state.rowScroll.retarget(target, overview.durationMs, overview.workspaceCurve);
    }
    // Animations only tick from an output frame; kick one so an idle desktop
    // starts both timelines from the same frame.
    scheduleFrames();
  }

  bool Overview::tickEdgePan(double nowMsec) {
    Workspace* workspace = m_edgePan != 0 ? m_edgeWorkspace.get() : nullptr;
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr || (m_dragCard == nullptr && !m_thumbDragging)) {
      return false;
    }
    const double elapsed = m_edgePanLastMsec < 0 ? 0.0 : nowMsec - m_edgePanLastMsec;
    m_edgePanLastMsec = nowMsec;
    const double max = scrolling->maxScroll(workspace->scrollViewportExtent());
    const double next = std::clamp(scrolling->scroll() + m_edgePan * kEdgePanPxPerMs * elapsed, 0.0, max);
    if (next != scrolling->scroll()) {
      scrolling->setScroll(next);
      workspace->markArrange(false);
      if (m_dragCard != nullptr) {
        retargetDrop(m_dragX, m_dragY);
      }
    }
    // Keep frames coming only while there is room to pan.
    return m_edgePan < 0 ? next > 0.0 : next < max;
  }

  bool Overview::tickAnimations(double nowMsec) {
    bool active = m_dropHint != nullptr && m_dropHint->tickAnimations(nowMsec);
    active = tickEdgePan(nowMsec) || active;
    const bool zoomTicked = m_zoomAnim.tick(nowMsec);
    if (zoomTicked && m_zoomAnim.animating() && m_zoomAnim.curve().easing == Easing::Spring) {
      static_cast<void>(m_zoomAnim.finishSpringTail(motionPixelsPerUnit(false)));
    }
    if (zoomTicked) {
      const double value = m_zoomAnim.current();
      m_progress = m_progressFrom + (m_targetProgress - m_progressFrom) * value;
    }
    bool rowTicked = m_reflow.tick(nowMsec);
    if (rowTicked && m_reflow.animating() && m_reflow.curve().easing == Easing::Spring) {
      static_cast<void>(m_reflow.finishSpringTail(motionPixelsPerUnit(true)));
    }
    if (!m_reflow.animating()) {
      m_reflowFrom.clear();
    }
    active = active || m_reflow.animating();
    for (const auto& state : m_outputs) {
      const bool ticked = state->rowScroll.tick(nowMsec);
      if (ticked && state->rowScroll.animating() && state->rowScroll.curve().easing == Easing::Spring) {
        static_cast<void>(state->rowScroll.finishSpringTail(motionPixelsPerUnit(true)));
      }
      rowTicked = ticked || rowTicked;
      active = active || state->rowScroll.animating();
    }
    if (zoomTicked || rowTicked || m_cardPresentationDirty) {
      applyProgress();
    }
    if (rowTicked && m_dragCard != nullptr) {
      // Rows slide under a held card while the edge steps them; the target follows what is under it.
      retargetDrop(m_dragX, m_dragY);
    }
    m_cardPresentationDirty = false;
    // Card layout runs only while the overview moves; effects sync every tick and their time moves only on the card
    // output's effect frames.
    if (m_active && effectRegistry().active()) {
      syncCardEffects();
    }
    for (const auto& state : m_outputs) {
      bindAnimationEffect(
          &state->tree->node, AnimationEvent::Overview, m_zoomAnim.animating() ? m_zoomAnim : state->rowScroll,
          m_closing ? -1.0F : 1.0F
      );
    }
    if (zoomTicked && !m_zoomAnim.animating()) {
      finishAnimation();
    }
    return m_zoomAnim.animating() || active;
  }

  bool Overview::hasActiveAnimations() const {
    return m_zoomAnim.animating()
        || m_reflow.animating()
        || std::ranges::any_of(m_outputs, [](const auto& state) { return state->rowScroll.animating(); })
        || (m_dropHint != nullptr && m_dropHint->hasActiveAnimations());
  }

  void Overview::finishAnimation() {
    if (!m_closing) {
      applyProgress();
      return;
    }
    View* focus = m_pendingFocus.get();
    m_pendingFocus = nullptr;
    // The real trees stay hidden until teardown, so a workspace switch during the close runs as the row scroll. The
    // close still ends with the zoom, so binds keep their timing; a row still travelling hands its offset and velocity
    // to the real slide. beginClose retargets each row to the active workspace, so a row resting there is not carried.
    std::vector<std::tuple<WorkspaceGroup*, double, double>> carried;
    for (const auto& state : m_outputs) {
      const AnimatedValue& row = state->rowScroll;
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group != nullptr && row.animating() && row.from() != row.target()) {
        carried.emplace_back(group, std::clamp(row.current() - row.target(), -1.0, 1.0), row.velocity());
      }
    }
    teardown();
    // Real trees are visible again: settle each active workspace so window
    // positions match where the cards landed.
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        if (Workspace* workspace = group->active()) {
          workspace->markArrange(false);
        }
      }
    }
    for (const auto& [group, offset, velocity] : carried) {
      if (group->slideBegin(offset < 0, offset > 0)) {
        group->slideApply(offset);
        group->slideSettle(0, velocity);
      }
    }
    restoreFocus(focus);
  }

  void Overview::restoreFocus(View* focus) {
    if (focus != nullptr && focus->mapped()) {
      m_server->focusView(focus, FocusReason::PointerPress);
    } else {
      m_server->refocus();
    }
    // Focus chrome changed while the windows were hidden (the open cleared it, focus returned during the close or
    // just now), so they reappear with the settled result.
    for (const auto& view : m_server->registry().all()) {
      view->settleFocusChrome();
    }
  }

  void Overview::teardown() {
    m_server->cursor()->resetWheelAccumulation();
    m_reflow.snap(1.0);
    m_shownRects.clear();
    m_reflowFrom.clear();
    cancelNavigation();
    clearMiddlePress();
    hideDropHint();
    stopEdgeScroll();
    for (const auto& state : m_outputs) {
      clearDesktop(*state);
      for (const auto& card : state->cards) {
        destroyCard(card.get());
      }
      state->cards.clear();
      if (state->backgroundBlur != nullptr) {
        wlr_scene_node_destroy(&state->backgroundBlur->node);
        state->backgroundBlur = nullptr;
      }
      if (state->tree != nullptr) {
        wlr_scene_node_destroy(&state->tree->node);
        state->tree = nullptr;
      }
      state->output->markBlurBackgroundDirty();
      wlr_output_schedule_frame(state->output->wlr());
    }
    m_outputs.clear();
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, false);

    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->pinnedTree()->node, true);
    if (m_active) {
      applyPinnedOpacity(1.0F);
    }
    wlr_scene_node_set_enabled(&m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node, true);

    m_active = false;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pointerOutput = nullptr;
    m_server->notifyOverviewChanged();
    endThumbDrag(false, 0, 0);
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_hoverView = nullptr;
    m_closeHover = false;
    m_closePress = nullptr;
    m_dragCard = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_cardPresentationDirty = false;
    m_gestureOpenedHere = false;
    m_shortcutInput.clear();
    m_filter.clear();
    m_shortcutAssignments.clear();
    m_shortcutLabelCapacity = 0;
    m_server->reconcileDynamicWorkspaces();
    m_server->scheduleSuspensionSync();
  }

  // -: gesture

  void Overview::gestureUpdate(double progress) {
    if (!m_active) {
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedHere = true;
    }
    m_zoomAnim.snap(0.0);
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    m_closing = false;
    m_server->notifyOverviewChanged();
    m_progress = std::min(progress, 1.0);
    m_targetProgress = m_progress;
    applyProgress();
  }

  void Overview::gestureEnd(int stop, double releaseVelocity) {
    if (!m_active) {
      return;
    }
    m_gestureOpenedHere = false;
    if (stop > 0) {
      startAnimation(1.0, false, releaseVelocity);
      return;
    }
    beginClose(nullptr, releaseVelocity);
  }

  // -: hooks

  void Overview::onViewMapped(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    if (view->attached()) {
      onViewAttachmentsChanged(view->placementRoot());
      return;
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr || findCard(view) != nullptr) {
      return;
    }
    if (createCard(*state, view, workspace->index()) == nullptr) {
      return;
    }
    assignShortcuts();
  }

  void Overview::onViewAttachmentsChanged(View* parent) {
    if (!m_active || parent == nullptr) {
      return;
    }
    Card* card = findCard(parent->placementRoot());
    if (card == nullptr || card->owner == nullptr) {
      return;
    }
    // The card's own surfaces stay; the dialogs' are mirrored again from the current attachments.
    std::erase_if(card->surfaces, [card](const std::unique_ptr<CardSurface>& entry) {
      if (entry->view == card->view) {
        return false;
      }
      entry->outputSample.disconnect();
      entry->frameDone.disconnect();
      if (entry->buffer != nullptr) {
        wlr_scene_node_destroy(&entry->buffer->node);
      }
      return true;
    });
    syncCardSurfaces(card);
    layoutOutput(*card->owner);
    wlr_output_schedule_frame(card->owner->output->wlr());
  }

  void Overview::onViewPinnedChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    // A pinned window shows through the overview only at the desktop chrome's alpha; a card mirrors full opacity.
    view->setOverviewOpacity(view->pinned() ? desktopChromeAlpha() : 1.0F);
    Card* card = findCard(view);
    OutputState* state = card != nullptr ? card->owner : stateForWorkspace(view->workspace());
    if (view->pinned()) {
      if (card == nullptr) {
        return;
      }
      if (m_pressCard == card) {
        m_pressCard = nullptr;
      }
      if (m_middlePressCard == card) {
        clearMiddlePress();
      }
      if (m_drop.view == view) {
        m_drop.view = nullptr;
        m_drop.edge = 0;
        hideDropHint();
      }
      if (m_dragCard == card) {
        hideDropHint();
        stopEdgeScroll();
        m_dragCard = nullptr;
        m_dragSourceWorkspace = nullptr;
        m_dragSourceWidth.reset();
        m_drop = {};
        m_dropWorkspaceGroup = nullptr;
        m_server->cursor()->overrideCursor(nullptr);
      }
      std::erase_if(m_shortcutAssignments, [view](const ShortcutAssignment& assignment) {
        return assignment.view == view;
      });
      dropCard(view);
    } else if (card == nullptr && state != nullptr && view->workspace() != nullptr) {
      createCard(*state, view, view->workspace()->index());
    }
    if (state != nullptr) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
    assignShortcuts();
  }

  void Overview::onViewFloatingChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    // Cards stack tiled, then floating (populateCards): rebuild so this one
    // moves to its new layer instead of keeping its old spot.
    rebuildCard(view);
    assignShortcuts();
  }

  void Overview::onViewUnmapped(View* view, bool animateClose) {
    if (!m_active || view == nullptr) {
      return;
    }
    std::erase_if(m_shortcutAssignments, [view](const ShortcutAssignment& assignment) {
      return assignment.view == view;
    });
    if (m_pendingFocus == view) {
      m_pendingFocus = nullptr;
    }
    if (m_pressCard != nullptr && m_pressCard->view == view) {
      m_pressCard = nullptr;
    }
    if (m_hoverView == view) {
      m_hoverView = nullptr;
      m_closeHover = false;
    }
    if (m_closePress == view) {
      m_closePress = nullptr;
    }
    if (m_middlePressCard != nullptr && m_middlePressCard->view == view) {
      m_middlePressCard = nullptr;
    }
    if (m_drop.view == view) {
      m_drop.view = nullptr;
      m_drop.edge = 0;
      hideDropHint();
    }
    if (m_dragCard != nullptr && m_dragCard->view == view) {
      hideDropHint();
      stopEdgeScroll();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_drop = {};
      m_dropWorkspaceGroup = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (Card* card = findCard(view); card != nullptr && animateClose) {
      snapshotCardForClose(*card);
    }
    dropCard(view);
    // The overview keeps the focus chrome while it owns the seat, so a closed focused window hands focus to the nearest
    // survivor now rather than at zoom-out. Ask before layout detachment so the closing view still identifies its row
    // and column; the replacement is its predecessor, or the next neighbor at the leading edge.
    if (workspace != nullptr && workspace->focusedView() == view) {
      View* replacement = workspace->focusReplacementForRemoval(view);
      if (replacement != nullptr) {
        // The layout judges the pair once this column is gone, so the reveal this unblocks may only fit the survivor.
        workspace->noteRemovalOfFocusedColumn(workspace->layout().columnOf(view));
        if (workspace->active()) {
          m_server->focusView(replacement, FocusReason::Startup);
        } else {
          workspace->setFocusedView(replacement);
        }
      } else {
        workspace->setFocusedView(nullptr);
      }
    }
    if (state != nullptr) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
    assignShortcuts();
  }

  void Overview::onViewWorkspaceChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    if (view->pinned()) {
      onViewPinnedChanged(view);
      return;
    }

    Card* card = findCard(view);
    Workspace* workspace = view->workspace();
    OutputState* target = stateForWorkspace(workspace);
    if (card == nullptr) {
      if (target != nullptr) {
        createCard(*target, view, workspace->index());
        layoutOutput(*target);
        wlr_output_schedule_frame(target->output->wlr());
      }
      assignShortcuts();
      return;
    }

    OutputState* source = card->owner;
    if (target == nullptr) {
      if (m_pressCard == card) {
        m_pressCard = nullptr;
      }
      if (m_middlePressCard == card) {
        m_middlePressCard = nullptr;
      }
      dropCard(view);
      if (source != nullptr) {
        layoutOutput(*source);
        wlr_output_schedule_frame(source->output->wlr());
      }
      assignShortcuts();
      return;
    }

    card->workspaceIndex = workspace->index();
    if (source == target) {
      layoutOutput(*target);
      wlr_output_schedule_frame(target->output->wlr());
      assignShortcuts();
      return;
    }
    if (source == nullptr) {
      rebuildCard(view);
      assignShortcuts();
      return;
    }

    const auto it = std::ranges::find_if(source->cards, [card](const std::unique_ptr<Card>& candidate) {
      return candidate.get() == card;
    });
    if (it == source->cards.end()) {
      rebuildCard(view);
      assignShortcuts();
      return;
    }

    std::unique_ptr<Card> moved = std::move(*it);
    source->cards.erase(it);
    moved->owner = target;
    wlr_scene_node_reparent(&moved->tree->node, target->tree);
    target->cards.push_back(std::move(moved));
    layoutOutput(*source);
    layoutOutput(*target);
    wlr_output_schedule_frame(source->output->wlr());
    wlr_output_schedule_frame(target->output->wlr());
    assignShortcuts();
  }

  void Overview::onWorkspaceActivated(WorkspaceGroup* group) {
    if (!m_active || group == nullptr || group->active() == nullptr) {
      return;
    }
    if (m_navigationOutput == group->output()) {
      cancelNavigation();
    }
    OutputState* target = stateFor(group->output());
    if (target == nullptr) {
      return;
    }
    const auto row = static_cast<double>(group->active()->index());
    target->activeWorkspaceIndex = group->active()->index();
    if (std::abs(target->rowScroll.target() - row) < 0.001 && target->rowScroll.animating()) {
      return;
    }
    if (m_closing) {
      m_pendingFocus = nullptr;
    }
    animateRow(*target, row);
    assignShortcuts();
  }

  void Overview::onWorkspaceArranged(Workspace* workspace) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateForWorkspace(workspace)) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onWorkspaceInventoryChanged(WorkspaceGroup* group) {
    if (!m_active || group == nullptr) {
      return;
    }
    if (m_navigationOutput == group->output()) {
      cancelNavigation();
    }
    OutputState* state = stateFor(group->output());
    if (state == nullptr) {
      return;
    }
    // Renumbering rows on one side of the active workspace must shift the filmstrip scroll by the same amount,
    // otherwise every surviving card visibly jumps even though its workspace identity did not move.
    if (Workspace* active = group->active()) {
      const double delta = static_cast<double>(active->index()) - static_cast<double>(state->activeWorkspaceIndex);
      state->activeWorkspaceIndex = active->index();
      state->rowScroll.translate(delta);
    }
    syncWorkspaceRows(*state, *group);
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
    assignShortcuts();
  }

  void Overview::onFocusChanged() {
    if (!m_active) {
      return;
    }
    cancelNavigation();
    if (m_closing) {
      m_pendingFocus = nullptr;
    }
    applyProgress();
  }

  void Overview::onViewPresentationChanged(View* view) {
    if (!m_active || view == nullptr || stateForWorkspace(view->workspace()) == nullptr) {
      return;
    }
    m_cardPresentationDirty = true;
  }

  void Overview::onDesktopLayerChanged(Output* output) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateFor(output)) {
      // layoutOutput re-resolves the stack before placing the rows.
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onOutputRemoved(Output* output) {
    if (m_navigationOutput == output) {
      // The output may already be tearing down its workspace group.
      m_navigationOutput = nullptr;
      cancelNavigation();
    }
    if (!m_active) {
      return;
    }
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    if (it == m_outputs.end()) {
      return;
    }
    if (m_drop.workspace != nullptr && stateForWorkspace(m_drop.workspace) == it->get()) {
      hideDropHint();
      m_drop = {};
    }
    if (m_dropWorkspaceGroup != nullptr && m_dropWorkspaceGroup->output() == output) {
      hideDropHint();
      m_dropWorkspaceGroup = nullptr;
    }
    if (m_dragCard != nullptr && m_dragCard->owner == it->get()) {
      hideDropHint();
      stopEdgeScroll();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_drop = {};
      m_dropWorkspaceGroup = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
    if (m_pressCard != nullptr && m_pressCard->owner == it->get()) {
      m_pressCard = nullptr;
    }
    if (m_middleOutput == output) {
      clearMiddlePress();
    }
    clearDesktop(**it);
    for (const auto& card : (*it)->cards) {
      destroyCard(card.get());
    }
    (*it)->cards.clear();
    if ((*it)->tree != nullptr) {
      wlr_scene_node_destroy(&(*it)->tree->node);
    }
    m_outputs.erase(it);
    if (m_outputs.empty()) {
      forceClose();
    } else {
      assignShortcuts();
    }
  }

  // -: hit testing

  Overview::Card* Overview::cardAt(double lx, double ly, const Card* skip) {
    // Topmost first: later outputs and later cards paint over earlier ones.
    // Hit testing uses the same output clip as rendering. The dragged card is
    // reparented to the unclipped root and hits everywhere.
    for (const auto& state : std::views::reverse(m_outputs)) {
      for (const auto& card : std::views::reverse(state->cards)) {
        if (card.get() == skip || card->tree == nullptr || !card->tree->node.enabled) {
          continue;
        }
        wlr_box hit = card->box;
        if (card.get() != m_dragCard && !wlr_box_intersection(&hit, &card->box, &state->outputBox)) {
          continue;
        }
        if (boxContains(hit, lx, ly)) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  Workspace*
  Overview::workspaceAtPoint(double lx, double ly, OutputState** outState, size_t* outIndex, bool extendScrollingAxis) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      // A card can only own points on its own output, so one overhanging a neighbour never steals its targets.
      if (group == nullptr || !boxContains(state->outputBox, lx, ly)) {
        continue;
      }
      const bool strip = configuredStyle() == ov::Style::Strip;
      const bool horizontalWorkspaces = group->workspaceAxis() == WorkspaceAxis::Horizontal;
      for (size_t index = 0; index < state->workspaceViews.size(); ++index) {
        Workspace* workspace = group->workspaceAt(index);
        if (workspace == nullptr) {
          continue;
        }
        bool hit = workspaceViewContains(*state, index, lx, ly);
        // A Strip row's scrolling columns overhang its frame across the whole output, so drag targeting covers that
        // band too or the extreme gaps become dead zones. Background clicks keep the frame.
        if (!hit
            && extendScrollingAxis
            && strip
            && workspace->scrollingLayout() != nullptr
            && state->workspaceViews[index].shown) {
          wlr_box band = state->workspaceViews[index].box;
          if (horizontalWorkspaces) {
            band.y = state->outputBox.y;
            band.height = state->outputBox.height;
          } else {
            band.x = state->outputBox.x;
            band.width = state->outputBox.width;
          }
          hit = boxContains(band, lx, ly);
        }
        if (!hit) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outIndex != nullptr) {
          *outIndex = index;
        }
        return workspace;
      }
    }
    return nullptr;
  }

  WorkspaceGroup*
  Overview::workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox) {
    const bool strip = configuredStyle() == ov::Style::Strip;
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !group->dynamic() || group->workspaceCount() >= kMaxWorkspaces) {
        continue;
      }
      // Gaps run between neighbouring cards along the way the cards step. A Grid's wrap has no gap.
      const bool horizontalWorkspaces = state->cardsHorizontal;
      const size_t previewCount = std::min(group->workspaceCount(), state->workspaceViews.size());
      for (size_t index = 0; index < previewCount; ++index) {
        if (!state->workspaceViews[index].shown) {
          continue;
        }
        const wlr_box lower = state->workspaceViews[index].box;
        // Index zero has no preceding card. In the Strip its insertion gap begins at the output edge, so dynamic
        // workspaces can also be inserted before the first row; the thumb rows keep their margins free.
        int before = 0;
        if (index == 0) {
          if (!strip) {
            continue;
          }
          before = horizontalWorkspaces ? state->outputBox.x : state->outputBox.y;
        } else {
          if (!state->workspaceViews[index - 1].shown) {
            continue;
          }
          const wlr_box upper = state->workspaceViews[index - 1].box;
          before = horizontalWorkspaces ? upper.x + upper.width : upper.y + upper.height;
        }
        const int after = horizontalWorkspaces ? lower.x : lower.y;
        if (after <= before) {
          continue;
        }
        const wlr_box gap = horizontalWorkspaces ? wlr_box{before, lower.y, after - before, lower.height}
                                                 : wlr_box{lower.x, before, lower.width, after - before};
        wlr_box visible{};
        if (!wlr_box_intersection(&visible, &gap, &state->outputBox) || !boxContains(visible, lx, ly)) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outIndex != nullptr) {
          *outIndex = index;
        }
        if (outHintBox != nullptr) {
          // A bar across the gap: thin along the workspace axis, preview-sized across it.
          if (horizontalWorkspaces) {
            const int width = std::clamp(visible.width / 3, 4, 18);
            *outHintBox = {
                .x = visible.x + (visible.width - width) / 2,
                .y = visible.y,
                .width = width,
                .height = visible.height,
            };
          } else {
            const int height = std::clamp(visible.height / 3, 4, 18);
            *outHintBox = {
                .x = visible.x,
                .y = visible.y + (visible.height - height) / 2,
                .width = visible.width,
                .height = height,
            };
          }
        }
        return group;
      }
    }
    return nullptr;
  }

  Workspace* Overview::preferredWorkspace() const {
    Output* output = m_server->outputFromWlr(m_server->preferredOutput());
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return nullptr;
    }
    return output->workspaceGroup()->active();
  }

  void Overview::clearMiddlePress() {
    if (m_middleScrolling) {
      m_server->gestures()->endPointerScroll(true, 0);
    }
    if (m_middleDragging) {
      m_server->cursor()->overrideCursor(nullptr);
    }
    m_middlePressCard = nullptr;
    m_middleOutput = nullptr;
    m_middlePressed = false;
    m_middleDragging = false;
    m_middlePanning = false;
    m_middleScrolling = false;
    m_middleAccum = 0;
  }

  // -: input

  bool Overview::handleButton(uint32_t button, bool pressed, double lx, double ly, uint32_t timeMsec) {
    if (pressed) {
      cancelNavigation();
    }
    if (!interactive()) {
      return true; // Swallow everything while zooming back in.
    }

    if (!pressed) {
      if (button == BTN_MIDDLE) {
        Card* card = m_middlePressCard;
        const bool closeCard = m_middlePressed && !m_middleDragging;
        if (m_middleScrolling) {
          // A release settles the strip; clearMiddlePress() cancels whatever is still running.
          m_server->gestures()->endPointerScroll(false, timeMsec);
          m_middleScrolling = false;
        }
        clearMiddlePress();
        if (closeCard && card != nullptr && card->view != nullptr && card->view->mapped()) {
          card->view->requestClose();
        }
        return true;
      }
      if (button != BTN_LEFT) {
        return true;
      }
      if (m_closePress != nullptr) {
        const View* pressed = std::exchange(m_closePress, nullptr);
        // Sliding off the button before letting go cancels, like any button.
        if (closeButtonAt(lx, ly) == pressed) {
          if (Card* closing = findCard(pressed); closing != nullptr && closing->view != nullptr) {
            closing->view->requestClose();
          }
        }
        return true;
      }
      if (m_dragCard != nullptr) {
        endDrag(true);
        return true;
      }
      if (m_thumbDragging) {
        endThumbDrag(true, lx, ly);
        return true;
      }
      Card* card = m_pressCard;
      Workspace* workspace = m_pressWorkspace.get();
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      if (card != nullptr && card->view != nullptr && card->view->mapped()) {
        closeToWorkspace(card->view->workspace(), card->view);
      } else if (workspace != nullptr) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    }

    Card* card = cardAt(lx, ly);
    if (button == BTN_MIDDLE) {
      m_middlePressCard = card;
      Workspace* workspace = card != nullptr && card->view != nullptr
          ? card->view->workspace()
          : workspaceAtPoint(lx, ly, nullptr, nullptr, false);
      m_middleOutput = workspace != nullptr && workspace->group() != nullptr ? workspace->group()->output() : nullptr;
      m_middlePressX = lx;
      m_middlePressY = ly;
      m_middleAccum = 0;
      m_middlePressed = true;
      m_middleDragging = false;
      m_middlePanning = false;
      m_middleScrolling = false;
      return true;
    }
    if (button != BTN_LEFT) {
      return true;
    }

    if (const View* close = closeButtonAt(lx, ly)) {
      m_closePress = close;
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      return true;
    }
    // A thumb's miniature windows are part of the thumb: pressing one picks up the workspace, as in the prototype.
    if (card != nullptr && cardInThumb(*card)) {
      card = nullptr;
    }
    m_pressCard = card;
    m_pressWorkspace = nullptr;
    m_pressX = lx;
    m_pressY = ly;
    m_pressThumb = false;
    if (card == nullptr) {
      m_pressWorkspace = workspaceAtPoint(lx, ly, nullptr, nullptr, false);
      m_pressThumb = m_pressWorkspace != nullptr && thumbAt(lx, ly) == m_pressWorkspace.get();
    }
    return true;
  }

  void Overview::handleMotion(double lx, double ly, uint32_t timeMsec) {
    if (!interactive()) {
      return;
    }
    // The live target resolves through preferredOutput(), and the overview owns motion while it is up: repaint when
    // the pointer crosses outputs, whether by hand or through the warp a keybind performs to change output.
    if (Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
        output != m_pointerOutput) {
      m_pointerOutput = output;
      applyProgress();
    }
    if (m_middlePressed) {
      const WorkspaceGroup* group = m_middleOutput != nullptr ? m_middleOutput->workspaceGroup() : nullptr;
      const bool horizontal = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
      if (!m_middleDragging) {
        const double dx = lx - m_middlePressX;
        const double dy = ly - m_middlePressY;
        if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
          return;
        }
        m_middleDragging = true;
        // Travel across the workspace axis pans the strip; travel along it steps rows.
        m_middlePanning = (std::abs(dx) > std::abs(dy)) != horizontal;
        if (m_middlePanning) {
          m_middleScrolling = m_server->gestures()->beginPointerScroll(m_middlePressX, m_middlePressY);
          if (m_middleScrolling) {
            m_server->gestures()->updatePointerScroll(dx, dy, timeMsec);
          }
        }
        m_middleAccum = 0;
        m_middlePressX = lx;
        m_middlePressY = ly;
        m_server->cursor()->overrideCursor("grabbing");
      }
      if (m_middlePanning) {
        if (m_middleScrolling) {
          m_server->gestures()->updatePointerScroll(lx - m_middlePressX, ly - m_middlePressY, timeMsec);
        }
        m_middlePressX = lx;
        m_middlePressY = ly;
        return;
      }
      m_middleAccum += horizontal ? lx - m_middlePressX : ly - m_middlePressY;
      m_middlePressX = lx;
      m_middlePressY = ly;
      while (m_middleAccum <= -kMiddleScrollStepPx) {
        m_middleAccum += kMiddleScrollStepPx;
        selectRelativeWorkspace(1, m_middleOutput.get());
      }
      while (m_middleAccum >= kMiddleScrollStepPx) {
        m_middleAccum -= kMiddleScrollStepPx;
        selectRelativeWorkspace(-1, m_middleOutput.get());
      }
      return;
    }
    if (m_dragCard != nullptr) {
      updateDrag(lx, ly);
      return;
    }
    if (m_pressThumb && m_pressWorkspace != nullptr) {
      if (!m_thumbDragging) {
        const double dx = lx - m_pressX;
        const double dy = ly - m_pressY;
        if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
          return;
        }
        m_thumbDragging = true;
        m_server->cursor()->overrideCursor("grabbing");
      }
      updateThumbDrag(lx, ly);
      return;
    }
    if (m_pressCard == nullptr) {
      updateHover(lx, ly);
      return;
    }
    const double dx = lx - m_pressX;
    const double dy = ly - m_pressY;
    if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
      return;
    }
    beginDrag();
    updateDrag(lx, ly);
  }

  bool Overview::selectRelativeWorkspace(int delta, Output* output) {
    if (!interactive()) {
      return false;
    }
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return false;
    }
    WorkspaceGroup* group = output->workspaceGroup();
    if (group->active() == nullptr) {
      return false;
    }
    const int index = static_cast<int>(group->active()->index()) + delta;
    if (index < 0 || index >= static_cast<int>(group->workspaceCount())) {
      return false;
    }
    // select() lands in onWorkspaceActivated, which animates the filmstrip onto the new
    // workspace, so the caller does not have to arrange anything.
    group->select(group->workspaceAt(static_cast<size_t>(index)));
    return true;
  }

  const View* Overview::closeButtonAt(double lx, double ly) const {
    for (const auto& state : m_outputs) {
      if (state->chrome != nullptr) {
        if (const void* key = state->chrome->closeAt(lx, ly)) {
          return static_cast<const View*>(key);
        }
      }
    }
    return nullptr;
  }

  void Overview::updateHover(double lx, double ly) {
    const View* close = closeButtonAt(lx, ly);
    const View* hover = close;
    if (hover == nullptr) {
      const Card* card = cardAt(lx, ly);
      hover = card != nullptr ? card->view : nullptr;
    }
    const bool closeHover = close != nullptr;
    if (hover == m_hoverView && closeHover == m_closeHover) {
      return;
    }
    m_hoverView = hover;
    m_closeHover = closeHover;
    // Only the labels and the close button follow the pointer; the cards stay where they are.
    const View* liveTarget = liveTargetView();
    for (const auto& state : m_outputs) {
      layoutChrome(*state, liveTarget);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  bool Overview::handleAxisNotch(bool vertical, double direction, double lx, double ly) {
    cancelNavigation();
    if (!interactive()) {
      return true;
    }
    Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
    const WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    const bool horizontalWorkspaces = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
    const int sign = direction < 0 ? -1 : 1;
    // Wheel input commits discrete targets on its physical axis, unlike continuous touchpad navigation.
    if (vertical != horizontalWorkspaces) {
      selectRelativeWorkspace(sign, output);
      return true;
    }
    Workspace* workspace = workspaceAtPoint(lx, ly, nullptr, nullptr, true);
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return true;
    }
    View* target = vertical ? workspace->focusVertical(sign) : workspace->focusAdjacent(sign);
    if (target == nullptr) {
      return true;
    }
    clearShortcutInput();
    if (workspace->active()) {
      m_server->focusView(target, FocusReason::Gesture);
    } else {
      workspace->setFocusedView(target);
    }
    workspace->activateFocusedColumn();
    scrolling->snapVisible(scrolling->columnOf(target), workspace->scrollViewportExtent());
    workspace->markArrange(true);
    return true;
  }

  Workspace* Overview::pointerScrollWorkspace(double lx, double ly) {
    if (!interactive()) {
      return nullptr;
    }
    return workspaceAtPoint(lx, ly, nullptr, nullptr, true);
  }

  Workspace* Overview::navigationWorkspace() const {
    const WorkspaceGroup* group = m_navigationOutput != nullptr ? m_navigationOutput->workspaceGroup() : nullptr;
    if (group != nullptr) {
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        if (group->workspaceAt(index) == m_navigationWorkspace) {
          return m_navigationWorkspace.get();
        }
      }
    }
    return nullptr;
  }

  void Overview::onNavigationDeviceDestroyed(Overview* self, void* /*data*/) { self->cancelNavigation(); }

  void Overview::cancelNavigation() { endNavigation(true, 0, m_navigationSource); }

  void Overview::beginNavigation(wlr_pointer* pointer, NavigationSource source, double lx, double ly) {
    cancelNavigation();
    if (!interactive() || dragging() || pointer == nullptr) {
      return;
    }
    Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return;
    }
    m_navigationOutput = output;
    m_navigationHorizontalWorkspaces = output->workspaceGroup()->workspaceAxis() == WorkspaceAxis::Horizontal;
    m_navigationWorkspace = workspaceAtPoint(lx, ly, nullptr, nullptr, true);
    if (m_navigationWorkspace == nullptr || m_navigationWorkspace->group() != output->workspaceGroup()) {
      m_navigationWorkspace = output->workspaceGroup()->active();
    }
    m_navigationPointer = pointer;
    m_navigationDeviceDestroy.connect(&pointer->base.events.destroy, this, onNavigationDeviceDestroyed);
    m_navigationSource = source;
    m_navigation.reset();
    m_navigationStarted = false;
  }

  void Overview::updateNavigation(double dx, double dy, uint32_t timeMsec) {
    if (m_navigationOutput == nullptr || !interactive()) {
      return;
    }
    m_navigation.update(dx, dy, timeMsec);
    const auto axis = m_navigation.axis();
    if (axis == OverviewNavigation::Axis::Pending) {
      return;
    }
    OutputState* state = stateFor(m_navigationOutput);
    WorkspaceGroup* group = m_navigationOutput->workspaceGroup();
    if (state == nullptr || group == nullptr || group->active() == nullptr) {
      cancelNavigation();
      return;
    }
    const auto& overview = config().overview;
    const double factor =
        axis == OverviewNavigation::Axis::Horizontal ? overview.scrollFactorHorizontal : overview.scrollFactorVertical;
    const auto travel = OverviewNavigation::travelFor(m_navigationSource);
    if ((axis == OverviewNavigation::Axis::Horizontal) == m_navigationHorizontalWorkspaces) {
      if (!m_navigationStarted) {
        m_navigationStart = state->rowScroll.current();
        m_navigationScale =
            OverviewNavigation::travelScale(1.0, workspaceScale(group->active()), factor, travel.workspace);
        m_navigationStarted = true;
      }
      const auto last = static_cast<double>(group->workspaceCount() - 1);
      // snap() also stops any settle still running on this output; row animations elsewhere and the zoom continue.
      state->rowScroll.snap(
          GesturePhysics::rubberBand(
              m_navigationStart + m_navigation.position() * m_navigationScale, 0.0, last,
              GesturePhysics::kOverscrollLimit
          )
      );
      applyProgress();
      return;
    }
    Workspace* workspace = navigationWorkspace();
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return;
    }
    const auto viewport = static_cast<double>(workspace->scrollViewportExtent());
    if (!m_navigationStarted) {
      m_navigationStart = scrolling->scroll();
      m_navigationCentered = scrolling->centeredRest();
      m_navigationScale = OverviewNavigation::travelScale(viewport, workspaceScale(workspace), factor, travel.viewport);
      m_navigationStarted = true;
    }
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    // Centering an edge column parks the strip past the last column, so the band opens onto the scroll the gesture
    // started from: the fingers bring the strip back in, not the pan's first frame.
    scrolling->setScroll(
        GesturePhysics::rubberBand(
            m_navigationStart + m_navigation.position() * m_navigationScale, std::min(0.0, m_navigationStart),
            std::max(maximum, m_navigationStart), viewport * GesturePhysics::kOverscrollLimit
        )
    );
    workspace->markArrange(false);
  }

  void Overview::endNavigation(bool cancelled, uint32_t timeMsec, NavigationSource source) {
    if (source != m_navigationSource) {
      return;
    }
    Workspace* workspace = navigationWorkspace();
    Output* output = m_navigationOutput;
    // Clear the gesture before selecting or focusing below: both re-enter through onWorkspaceActivated and
    // onFocusChanged, which cancel the navigation belonging to this output.
    m_navigationOutput = nullptr;
    m_navigationWorkspace = nullptr;
    if (m_navigationPointer != nullptr) {
      m_navigationDeviceDestroy.disconnect();
      m_navigationPointer = nullptr;
    }
    m_scrollDx = m_scrollDy = 0;
    m_scrollStopX = m_scrollStopY = false;
    const bool started = m_navigationStarted;
    m_navigationStarted = false;
    if (!started || output == nullptr || !interactive()) {
      return;
    }
    if (!cancelled) {
      m_navigation.update(0, 0, timeMsec);
    }
    const double projected = m_navigationStart + m_navigation.projectedPosition() * m_navigationScale;
    if ((m_navigation.axis() == OverviewNavigation::Axis::Horizontal) == m_navigationHorizontalWorkspaces) {
      OutputState* state = stateFor(output);
      WorkspaceGroup* group = output->workspaceGroup();
      if (state == nullptr || group == nullptr || group->active() == nullptr) {
        return;
      }
      const auto last = static_cast<double>(group->workspaceCount() - 1);
      const int index = cancelled ? static_cast<int>(group->active()->index())
                                  : GesturePhysics::stepTarget(projected, 0, static_cast<int>(last));
      // Rubber-banded travel moves the filmstrip slower than the fingers, so the release carries the visible speed.
      const double position = m_navigationStart + m_navigation.position() * m_navigationScale;
      const double velocity = cancelled ? 0.0
                                        : m_navigation.velocity()
              * m_navigationScale
              * GesturePhysics::rubberBandDerivative(position, 0.0, last, GesturePhysics::kOverscrollLimit);
      if (index != static_cast<int>(group->active()->index())) {
        group->select(group->workspaceAt(static_cast<size_t>(index)));
        state = stateFor(output);
      }
      if (state != nullptr) {
        animateRow(*state, static_cast<double>(index), velocity);
      }
      return;
    }
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return;
    }
    const int viewport = workspace->scrollViewportExtent();
    const auto maximum = static_cast<double>(scrolling->maxScroll(viewport));
    if (cancelled) {
      scrolling->setScroll(
          m_navigationCentered ? m_navigationStart : std::clamp(m_navigationStart, 0.0, maximum), m_navigationCentered
      );
      workspace->markArrange(true);
      return;
    }
    double bestDistance = std::numeric_limits<double>::max();
    double bestPosition = std::clamp(projected, 0.0, maximum);
    int bestColumn = -1;
    for (size_t index = 0; index < scrolling->columns().size(); ++index) {
      if (scrolling->columns()[index].views.empty()) {
        continue;
      }
      const auto column = static_cast<int>(index);
      for (const double position : scrolling->snapScrolls(column, viewport)) {
        const double distance = std::abs(position - projected);
        if (distance < bestDistance) {
          bestDistance = distance;
          bestPosition = position;
          bestColumn = column;
        }
      }
    }
    // Match normal strip navigation: focus the outermost fully visible column
    // in the travel direction, retaining the selected card within that column.
    const int direction = projected >= scrolling->scroll() ? 1 : -1;
    const auto count = static_cast<int>(scrolling->columns().size());
    for (int index = bestColumn + direction; bestColumn >= 0 && index >= 0 && index < count; index += direction) {
      const double x = scrolling->columnX(index, viewport);
      const double width = scrolling->columnWidth(index, viewport);
      if (x < bestPosition || x + width > bestPosition + viewport) {
        break;
      }
      bestColumn = index;
    }
    View* target = workspace->focusedView();
    const bool focusMoved = bestColumn >= 0 && (target == nullptr || scrolling->columnOf(target) != bestColumn);
    if (focusMoved) {
      // A tab group is entered on the tab it shows, as a directional focus move enters it.
      target = columnEntry(scrolling->columns()[static_cast<size_t>(bestColumn)]);
    }
    scrolling->setScroll(bestPosition);
    // Do not activate a different workspace merely because its preview was
    // panned. Its selected card will receive focus if the user enters that row.
    if (target != nullptr) {
      if (workspace->active()) {
        m_server->focusView(target, FocusReason::Gesture);
      } else {
        workspace->setFocusedView(target);
      }
    }
    // The pan chose the column, so the centering policy says where it rests. A pan back on the focused column has no
    // side to derive, so the last focus move's pair is judged instead.
    if (focusMoved) {
      workspace->activateFocusedColumn();
    } else {
      workspace->reevaluateFocusedColumn();
    }
    workspace->markArrange(true);
  }

  void Overview::handleTouchpadAxis(
      wlr_pointer* pointer, bool vertical, double delta, uint32_t timeMsec, double lx, double ly
  ) {
    // A three-finger swipe owns the overview until it ends; finger scrolling cannot arrive during one anyway.
    if (m_navigationPointer != nullptr && m_navigationSource != NavigationSource::Scroll) {
      return;
    }
    if (pointer != m_navigationPointer) {
      if (delta == 0) {
        return;
      }
      beginNavigation(pointer, NavigationSource::Scroll, lx, ly);
    }
    if (m_navigationPointer == nullptr) {
      return;
    }
    if (vertical) {
      m_scrollDy += delta;
      m_scrollStopY = delta == 0;
    } else {
      m_scrollDx += delta;
      m_scrollStopX = delta == 0;
    }
    m_scrollTime = timeMsec;
  }

  void Overview::handleTouchpadFrame() {
    if (m_navigationPointer == nullptr || m_navigationSource != NavigationSource::Scroll) {
      return;
    }
    if (m_scrollDx != 0 || m_scrollDy != 0) {
      updateNavigation(m_scrollDx, m_scrollDy, m_scrollTime);
    }
    const auto axis = m_navigation.axis();
    const bool stop = axis == OverviewNavigation::Axis::Horizontal ? m_scrollStopX
        : axis == OverviewNavigation::Axis::Vertical               ? m_scrollStopY
                                                                   : m_scrollStopX || m_scrollStopY;
    m_scrollDx = m_scrollDy = 0;
    m_scrollStopX = m_scrollStopY = false;
    if (stop) {
      endNavigation(false, m_scrollTime, NavigationSource::Scroll);
    }
  }

  void Overview::refreshShortcutMatches() {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->shortcut.empty()) {
          continue;
        }
        const size_t matched = shortcutStartsWith(card->shortcut, m_shortcutInput) ? m_shortcutInput.size() : SIZE_MAX;
        const size_t oldDisplayed = card->shortcutMatched == SIZE_MAX ? 0 : card->shortcutMatched;
        const size_t newDisplayed = matched == SIZE_MAX ? 0 : matched;
        card->shortcutMatched = matched;
        if (oldDisplayed != newDisplayed) {
          renderCardShortcut(*card);
        }
      }
    }
    applyProgress();
    scheduleFrames();
  }

  void Overview::clearShortcutInput() {
    m_shortcutInput.clear();
    refreshShortcutMatches();
  }

  bool Overview::handleShortcutKey(uint32_t keysym) {
    if (!config().overview.shortcuts || dragging()) {
      return false;
    }
    if (!m_shortcutInput.empty() && keysym == XKB_KEY_BackSpace) {
      m_shortcutInput.pop_back();
      refreshShortcutMatches();
      return true;
    }
    if (!m_shortcutInput.empty() && keysym == XKB_KEY_Escape) {
      clearShortcutInput();
      return true;
    }

    const char character = shortcutCharacter(keysym);
    if (character == '\0' || std::ranges::none_of(config().overview.shortcutKeys, [character](char configured) {
          return asciiLower(configured) == character;
        })) {
      return false;
    }

    std::string candidate = m_shortcutInput + character;
    for (int attempt = 0; attempt < 2; ++attempt) {
      for (const auto& state : m_outputs) {
        for (const auto& card : state->cards) {
          if (card->shortcut.size() == candidate.size()
              && shortcutStartsWith(card->shortcut, candidate)
              && card->view != nullptr
              && card->view->mapped()) {
            closeToWorkspace(card->view->workspace(), card->view);
            return true;
          }
        }
      }
      for (const auto& state : m_outputs) {
        for (const auto& card : state->cards) {
          if (shortcutStartsWith(card->shortcut, candidate)) {
            m_shortcutInput = candidate;
            refreshShortcutMatches();
            return true;
          }
        }
      }
      if (m_shortcutInput.empty()) {
        break;
      }
      m_shortcutInput.clear();
      candidate.assign(1, character);
    }

    clearShortcutInput();
    return true;
  }

  bool Overview::handleKeybindAction(KeybindAction action) {
    const bool directional = action == KeybindAction::WindowFocusLeft
        || action == KeybindAction::WindowFocusRight
        || action == KeybindAction::WindowFocusOrOutputLeft
        || action == KeybindAction::WindowFocusOrOutputRight
        || action == KeybindAction::WindowFocusUp
        || action == KeybindAction::WindowFocusDown
        || action == KeybindAction::WindowFocusOrWorkspaceUp
        || action == KeybindAction::WindowFocusOrWorkspaceDown
        || action == KeybindAction::WindowFocusOrOutputUp
        || action == KeybindAction::WindowFocusOrOutputDown;
    const bool outputFocus = action == KeybindAction::OutputFocusLeft
        || action == KeybindAction::OutputFocusRight
        || action == KeybindAction::OutputFocusUp
        || action == KeybindAction::OutputFocusDown;

    if (m_closing && (directional || outputFocus)) {
      m_pendingFocus = nullptr;
    }

    if (directional && interactive() && !m_shortcutInput.empty()) {
      clearShortcutInput();
    }
    return false;
  }

  bool Overview::cardMatches(const Card& card) const {
    if (card.view == nullptr) {
      return false;
    }
    const char* title = card.view->title();
    const char* appId = card.view->appId();
    return ov::filterMatches(m_filter, title != nullptr ? title : "", appId != nullptr ? appId : "");
  }

  std::vector<Overview::Card*> Overview::selectableCards() {
    Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    struct Entry {
      Card* card;
      std::tuple<bool, size_t, bool, size_t, int, int> key;
    };
    std::vector<Entry> entries;
    for (size_t outputIndex = 0; outputIndex < m_outputs.size(); ++outputIndex) {
      const OutputState& state = *m_outputs[outputIndex];
      const WorkspaceGroup* group = state.output->workspaceGroup();
      const Workspace* active = group != nullptr ? group->active() : nullptr;
      for (size_t k = 0; k < state.cards.size(); ++k) {
        Card& card = *state.cards[k];
        const bool placed = k < state.placeOf.size() && state.placeOf[k] != nullptr;
        if (!placed || card.view == nullptr || !card.view->mapped() || card.opacity <= 0.0F || !cardMatches(card)) {
          continue;
        }
        const bool onActive = active != nullptr && card.view->workspace() == active;
        entries.push_back({
            .card = &card,
            .key = {
                state.output != preferred, outputIndex, !onActive, card.workspaceIndex,
                card.box.y + card.box.height / 2, card.box.x + card.box.width / 2
            },
        });
      }
    }
    std::ranges::stable_sort(entries, {}, &Entry::key);
    std::vector<Card*> cards;
    cards.reserve(entries.size());
    for (const Entry& entry : entries) {
      cards.push_back(entry.card);
    }
    return cards;
  }

  Overview::Card* Overview::cardOf(const View* view) {
    if (view == nullptr) {
      return nullptr;
    }
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->view == view) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  Overview::Card* Overview::selectedCard() {
    Card* card = cardOf(liveTargetView());
    return card != nullptr && cardMatches(*card) ? card : nullptr;
  }

  void Overview::selectCard(Card& card) {
    View* view = card.view;
    Workspace* workspace = view != nullptr ? view->workspace() : nullptr;
    if (workspace == nullptr || workspace->group() == nullptr) {
      return;
    }
    clearShortcutInput();
    if (workspace->group()->active() != workspace) {
      workspace->group()->select(workspace);
    }
    if (workspace->active()) {
      m_server->focusView(view, FocusReason::Gesture);
    } else {
      workspace->setFocusedView(view);
    }
    if (ScrollingLayout* scrolling = workspace->scrollingLayout()) {
      workspace->activateFocusedColumn();
      scrolling->snapVisible(scrolling->columnOf(view), workspace->scrollViewportExtent());
      workspace->markArrange(true);
    }
    applyProgress();
    scheduleFrames();
  }

  void Overview::setFilter(std::string filter) {
    if (filter == m_filter) {
      return;
    }
    m_filter = std::move(filter);
    if (!m_filter.empty() && selectedCard() == nullptr) {
      const std::vector<Card*> matches = selectableCards();
      if (!matches.empty()) {
        selectCard(*matches.front());
        return;
      }
    }
    applyProgress();
    scheduleFrames();
  }

  bool Overview::moveSelection(int dx, int dy) {
    const std::vector<Card*> cards = selectableCards();
    if (cards.empty()) {
      return true;
    }
    const Card* from = selectedCard();
    if (from == nullptr) {
      selectCard(*cards.front());
      return true;
    }
    const auto centre = [](const Card& card) {
      return ov::Point{card.box.x + card.box.width / 2.0, card.box.y + card.box.height / 2.0};
    };
    std::vector<ov::Point> centres;
    centres.reserve(cards.size());
    for (const Card* card : cards) {
      centres.push_back(card == from ? ov::Point{1e9, 1e9} : centre(*card));
    }
    const ov::Direction direction = dx < 0 ? ov::Direction::Left
        : dx > 0                           ? ov::Direction::Right
        : dy < 0                           ? ov::Direction::Up
                                           : ov::Direction::Down;
    const int pick = ov::pickInDirection(centres, centre(*from), direction);
    if (pick >= 0) {
      selectCard(*cards[static_cast<size_t>(pick)]);
    }
    return true;
  }

  bool Overview::cycleSelection(int step) {
    const std::vector<Card*> cards = selectableCards();
    if (cards.empty()) {
      return true;
    }
    const Card* current = selectedCard();
    int index = -1;
    for (size_t k = 0; k < cards.size(); ++k) {
      if (cards[k] == current) {
        index = static_cast<int>(k);
      }
    }
    selectCard(*cards[ov::cycleIndex(cards.size(), index, step)]);
    return true;
  }

  bool Overview::activateFilter() {
    Card* card = selectedCard();
    if (card == nullptr) {
      const std::vector<Card*> matches = selectableCards();
      card = matches.empty() ? nullptr : matches.front();
    }
    if (card != nullptr) {
      closeToWorkspace(card->view->workspace(), card->view);
      return true;
    }
    // Nothing open matches: hand the query to the launcher, which opens over the overview with it typed in.
    if (DesktopShell* shell = m_server->desktopShell()) {
      (void)shell->sendAction(std::format("panel-open launcher {}", m_filter));
    }
    return true;
  }

  bool Overview::handleFallbackKey(uint32_t keysym) {
    if (!interactive()) {
      return false;
    }
    if (m_filter.empty() && handleShortcutKey(keysym)) {
      return true;
    }
    if (!m_filter.empty()) {
      switch (keysym) {
      case XKB_KEY_Escape:
        setFilter({});
        return true;
      case XKB_KEY_Return:
      case XKB_KEY_KP_Enter:
        return activateFilter();
      case XKB_KEY_Left:
        return moveSelection(-1, 0);
      case XKB_KEY_Right:
        return moveSelection(1, 0);
      case XKB_KEY_Up:
        return moveSelection(0, -1);
      case XKB_KEY_Down:
        return moveSelection(0, 1);
      default:
        break;
      }
    }
    switch (keysym) {
    case XKB_KEY_BackSpace: {
      std::string filter = m_filter;
      ov::popFilterCharacter(filter);
      setFilter(std::move(filter));
      return true;
    }
    case XKB_KEY_Tab:
      return cycleSelection(1);
    case XKB_KEY_ISO_Left_Tab:
      return cycleSelection(-1);
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
      if (Card* card = selectedCard()) {
        card->view->requestClose();
      }
      return true;
    default:
      break;
    }
    if (m_filter.empty()) {
      if (const int digit = ov::workspaceDigit(keysym); digit >= 0) {
        Output* output = m_server->outputFromWlr(m_server->preferredOutput());
        WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
        if (group != nullptr && static_cast<size_t>(digit) < group->workspaceCount()) {
          clearShortcutInput();
          group->select(group->workspaceAt(static_cast<size_t>(digit)));
        }
        return true;
      }
    }
    if (std::string text = ov::filterText(keysym, m_filter.empty()); !text.empty()) {
      clearShortcutInput();
      setFilter(m_filter + text);
      return true;
    }
    auto dispatch = [this](KeybindAction action) {
      Keybind bind;
      bind.action = action;
      return m_server->executeKeybindAction(bind);
    };
    // Local windows first, exactly like the composite focus actions. Only an arrow
    // pointing along the output's workspace axis falls through to a workspace step.
    const auto arrow = [this, &dispatch](int sign, bool horizontalArrow, KeybindAction action) {
      Workspace* workspace = preferredWorkspace();
      if (workspace == nullptr || workspace->group() == nullptr) {
        return true;
      }
      const bool alongWorkspaceAxis =
          (workspace->group()->workspaceAxis() == WorkspaceAxis::Horizontal) == horizontalArrow;
      const View* neighbor = horizontalArrow ? workspace->focusAdjacent(sign) : workspace->focusVertical(sign);
      if (neighbor != nullptr || !alongWorkspaceAxis) {
        return dispatch(action);
      }
      clearShortcutInput();
      selectRelativeWorkspace(sign, workspace->group()->output());
      return true;
    };
    switch (keysym) {
    case XKB_KEY_Escape:
      close();
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      clearShortcutInput();
      if (Workspace* workspace = preferredWorkspace()) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    case XKB_KEY_Left:
      return arrow(-1, true, KeybindAction::WindowFocusLeft);
    case XKB_KEY_Right:
      return arrow(1, true, KeybindAction::WindowFocusRight);
    case XKB_KEY_Up:
      return arrow(-1, false, KeybindAction::WindowFocusUp);
    case XKB_KEY_Down:
      return arrow(1, false, KeybindAction::WindowFocusDown);
    default:
      return false;
    }
  }

  // -: drag

  void Overview::beginDrag() {
    Card* card = m_pressCard;
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    if (card == nullptr || card->view == nullptr || !card->view->mapped()) {
      return;
    }
    View* view = card->view;
    m_dragCard = card;
    m_dragOffsetX = m_pressX - card->box.x;
    m_dragOffsetY = m_pressY - card->box.y;
    m_dragSourceWorkspace = view->workspace();
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    m_dragSourcePromotes = false;
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;

    if (m_dragSourceWorkspace != nullptr && view->tiled()) {
      m_dragSourceColumn = m_dragSourceWorkspace->layout().columnOf(view);
      m_dragSourceRow = m_dragSourceWorkspace->layout().rowOf(view);
      m_dragSourceWidth = captureDropColumnWidth(*m_dragSourceWorkspace, view);
      const MasterStackLayout* master = m_dragSourceWorkspace->masterLayout();
      m_dragSourcePromotes = master != nullptr && master->isSoleMaster(view) && master->columns().size() > 1;
      if (m_dragSourceColumn >= 0) {
        // Detach so the source row closes the gap live, exactly like a normal
        // tile drag; arrange() re-lays that output's cards through the hook.
        m_dragSourceWorkspace->layoutDetach(view, false);
      }
    }
    wlr_scene_node_reparent(&card->tree->node, m_tree);
    wlr_scene_node_raise_to_top(&card->tree->node);
    if (card->owner != nullptr) {
      layoutCard(*card, liveTargetView());
    }
    m_server->cursor()->overrideCursor("grabbing");
  }

  void Overview::updateDrag(double lx, double ly) {
    Card* card = m_dragCard;
    card->box.x = static_cast<int>(std::lround(lx - m_dragOffsetX));
    card->box.y = static_cast<int>(std::lround(ly - m_dragOffsetY));
    wlr_scene_node_set_position(&card->tree->node, card->box.x, card->box.y);
    m_dragX = lx;
    m_dragY = ly;
    retargetDrop(lx, ly);
    updateEdgeScroll(lx, ly);
  }

  void Overview::retargetDrop(double lx, double ly) {
    Card* card = m_dragCard;
    OutputState* state = nullptr;
    size_t workspaceIndex = 0;
    wlr_box workspaceHint{};
    if (WorkspaceGroup* group = plusTargetAt(lx, ly, &state, &workspaceHint)) {
      m_drop = {};
      m_dropWorkspaceGroup = group;
      m_dropWorkspaceIndex = group->workspaceCount();
      if (m_dropHint == nullptr) {
        m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
      }
      m_dropHint->show(state->output, workspaceHint, kPlusHintRadius, HintLook::Ring);
      wlr_scene_node_raise_to_top(&card->tree->node);
      scheduleFrames();
      return;
    }
    if (WorkspaceGroup* group = workspaceGapAt(lx, ly, &state, &workspaceIndex, &workspaceHint)) {
      m_drop = {};
      m_dropWorkspaceGroup = group;
      m_dropWorkspaceIndex = workspaceIndex;
      showWorkspaceInsertHint(state->output, workspaceHint);
      scheduleFrames();
      return;
    }
    m_dropWorkspaceGroup = nullptr;

    if (View* other = swapTargetAt(lx, ly)) {
      m_drop = {.workspace = other->workspace()};
      const Card* target = cardAt(lx, ly, card);
      if (m_dropHint == nullptr) {
        m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
      }
      m_dropHint->show(
          target->owner->output, target->box,
          static_cast<int>(std::lround(config().appearance.cornerRadius * target->scale)), HintLook::Ring
      );
      wlr_scene_node_raise_to_top(&card->tree->node);
      scheduleFrames();
      return;
    }

    size_t workspacePosition = 0;
    Workspace* workspace = workspaceAtPoint(lx, ly, &state, &workspacePosition, true);
    m_drop = {.workspace = workspace};
    if (workspace == nullptr || state == nullptr) {
      hideDropHint();
      scheduleFrames();
      return;
    }

    // Map the pointer out of the card (or the main area) and back into workspace world space.
    const ov::Fit fit = workspaceFit(*state, workspacePosition);
    const double worldX = state->outputBox.x + (lx - fit.ox) / fit.s;
    const double worldY = state->outputBox.y + (ly - fit.oy) / fit.s;

    if (card->view->tiled()) {
      // The overview applies its own projection after target selection. Keep
      // strip hints attached to content that is outside the normal viewport but
      // visible in the overview margin. Other layouts retain their normal
      // usable-area bounds.
      const bool scrolling = workspace->scrollingLayout() != nullptr;
      m_drop = computeDropTarget(
          *workspace, worldX, worldY, card->view,
          DropTargetOptions{
              .clipHintToUsable = !scrolling,
              .reserveScrollingViewportEdges = false,
              .endpointGapsOutsideColumns = scrolling,
          }
      );
    } else {
      m_drop = {
          .workspace = workspace,
          .column = static_cast<int>(workspace->layout().columns().size()),
      };
    }
    if (m_drop.hintBox.width > 0 && m_drop.hintBox.height > 0) {
      showDropHint(m_drop.hintBox, *state, workspacePosition);
    } else {
      hideDropHint();
    }
    scheduleFrames();
  }

  void Overview::endDrag(bool drop) {
    Card* card = m_dragCard;
    if (card == nullptr) {
      return;
    }
    View* view = card->view;
    WorkspaceGroup* insertionGroup = drop ? m_dropWorkspaceGroup : nullptr;
    View* swapWith = drop && insertionGroup == nullptr ? swapTargetAt(m_dragX, m_dragY) : nullptr;
    Workspace* target = drop ? m_drop.workspace : nullptr;
    DropTarget targetDrop = m_drop;
    const wlr_box cardBox = card->box;
    OutputState* dropState = target != nullptr ? stateForWorkspace(target) : nullptr;

    m_dragCard = nullptr;
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    hideDropHint();
    stopEdgeScroll();
    m_server->cursor()->overrideCursor(nullptr);

    if (view == nullptr || !view->mapped()) {
      return;
    }

    bool insertedWorkspace = false;
    if (insertionGroup != nullptr) {
      target = insertionGroup->insertDynamicWorkspace(m_dropWorkspaceIndex);
      dropState = target != nullptr ? stateForWorkspace(target) : nullptr;
      if (target != nullptr && dropState != nullptr) {
        targetDrop = {
            .workspace = target,
            .column = 0,
        };
        insertedWorkspace = true;
      }
    }

    if (swapWith == nullptr && target != nullptr && target->layoutMode() == LayoutMode::Floating && view->tiled()) {
      view->floatForLayout();
    }
    if (swapWith != nullptr) {
      swapDragged(*view, *swapWith);
    } else if (target != nullptr && view->tiled()) {
      applyDrop(
          *m_server, *view, *target, targetDrop, m_dragSourceWidth.has_value() ? &*m_dragSourceWidth : nullptr,
          /*animate=*/false
      );
    } else if (target != nullptr && dropState != nullptr && insertedWorkspace) {
      view->rememberFloatingPosition();
      if (view->workspace() != target) {
        view->moveToWorkspace(target);
      }
      if (view->floating()) {
        view->restoreFloatingPosition();
      }
      m_server->focusView(view, FocusReason::DragDrop);
    } else if (target != nullptr && dropState != nullptr) {
      // Floating: map the card origin back out of the thumbnail.
      {
        const ov::Fit fit = workspaceFit(*dropState, target->index());
        const int x = dropState->outputBox.x + static_cast<int>(std::lround((cardBox.x - fit.ox) / fit.s));
        const int y = dropState->outputBox.y + static_cast<int>(std::lround((cardBox.y - fit.oy) / fit.s));
        if (view->workspace() != target) {
          view->moveToWorkspace(target);
          target->exitFullscreenForIncomingView(view);
        }
        if (view->floating()) {
          view->setPosition(x, y);
          view->rememberFloatingPosition();
        }
        // Cards are laid out from the presented box, which setPosition does not refresh.
        target->syncViewPresentation(view);
      }
    } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
      // Cancelled or dropped on nothing: put the tile back where it came from.
      restoreDragSlot(*view);
      m_dragSourceWorkspace->arrange(false);
    }

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCard(view);
    applyProgress();
  }

  void Overview::restoreDragSlot(View& view) {
    Workspace& source = *m_dragSourceWorkspace;
    if (source.layoutMode() == LayoutMode::Floating) {
      source.layoutAttach(&view);
    } else if (m_dragSourceWidth.has_value()) {
      source.layout().insertView(&view, m_dragSourceColumn);
      const int column = source.layout().columnOf(&view);
      if (ColumnOps* ops = source.layout().columnOps()) {
        ops->setWidthFraction(column, m_dragSourceWidth->fraction);
        if (m_dragSourceWidth->fullWidth) {
          ops->toggleFullWidth(column);
        }
      }
      view.setMaximizedState(m_dragSourceWidth->fullWidth);
    } else if (source.dwindleLayout() != nullptr) {
      // Gap-index insert restores the exact flat position the drag removed.
      source.layout().insertView(&view, m_dragSourceColumn);
    } else if (m_dragSourceRow >= 0) {
      if (ColumnOps* ops = source.layout().columnOps()) {
        ops->insertViewIntoColumn(&view, m_dragSourceColumn, m_dragSourceRow);
      }
      // Lifting the only master window promoted the first stack window; send it back to the stack top.
      if (MasterStackLayout* master = source.masterLayout(); master != nullptr && m_dragSourcePromotes) {
        master->demoteToStack();
      }
    } else {
      source.layout().insertView(&view, m_dragSourceColumn);
    }
  }

  View* Overview::swapTargetAt(double lx, double ly) {
    if (m_dragCard == nullptr || m_dragSourceWorkspace == nullptr || m_dragSourceColumn < 0) {
      return nullptr;
    }
    const Card* card = cardAt(lx, ly, m_dragCard);
    if (card == nullptr || card->view == nullptr || !card->view->mapped() || !card->view->tiled()
        || card->box.width <= 0) {
      return nullptr;
    }
    const Workspace* workspace = card->view->workspace();
    if (workspace == nullptr || workspace->layoutMode() == LayoutMode::Floating) {
      return nullptr;
    }
    // A thumb is one target, "move here", as in the prototype: its miniature windows take no window drops.
    if (cardInThumb(*card)) {
      return nullptr;
    }
    const double share = (lx - card->box.x) / card->box.width;
    return share >= kSwapZoneStart && share <= 1.0 - kSwapZoneStart ? card->view : nullptr;
  }

  // Puts the dragged `view` where `other` is and `other` into the slot the drag took `view` from.
  void Overview::swapDragged(View& view, View& other) {
    Workspace& target = *other.workspace();
    // The drag left the source empty; a dynamic group would reap it before `other` arrives.
    const std::shared_ptr<WorkspaceLaunchAnchor> keepSource = m_dragSourceWorkspace->reserveForLaunch();
    DropTarget beside{.workspace = &target};
    if (target.dwindleLayout() != nullptr) {
      beside.view = &other;
      beside.edge = WLR_EDGE_RIGHT;
    } else {
      beside.column = target.layout().columnOf(&other) + 1;
    }
    applyDrop(*m_server, view, target, beside, nullptr, /*animate=*/false);
    target.layout().swapViews(&view, &other);
    target.layoutDetach(&other, false);
    if (&target != m_dragSourceWorkspace.get()) {
      other.moveToWorkspace(m_dragSourceWorkspace.get(), /*attachToLayout=*/false);
      m_dragSourceWorkspace->exitFullscreenForIncomingView(&other);
    }
    restoreDragSlot(other);
    other.setSceneParent(m_dragSourceWorkspace->viewLayer(true));
    // Each window takes the full-width state of the column it now sits in.
    const auto fullWidth = [](Workspace& workspace, const View& member) {
      ColumnOps* ops = workspace.layout().columnOps();
      const int column = workspace.layout().columnOf(&member);
      return ops != nullptr && column >= 0 && ops->isFullWidth(column);
    };
    view.setMaximizedState(fullWidth(target, view));
    other.setMaximizedState(fullWidth(*m_dragSourceWorkspace, other));
    target.arrange(false);
    m_dragSourceWorkspace->arrange(false);
    m_server->focusView(&view, FocusReason::DragDrop);
  }

  bool Overview::cardInThumb(const Card& card) const {
    if (card.owner == nullptr) {
      return false;
    }
    const OutputState& state = *card.owner;
    const bool inMain = state.mainBox.has_value() && card.workspaceIndex == state.mainWorkspace;
    return !inMain && card.workspaceIndex < state.workspaceViews.size()
        && state.workspaceViews[card.workspaceIndex].kind == ov::CardKind::Thumb;
  }

  Workspace* Overview::thumbAt(double lx, double ly, wlr_box* outBox) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !boxContains(state->outputBox, lx, ly)) {
        continue;
      }
      for (size_t index = 0; index < state->workspaceViews.size(); ++index) {
        const WorkspaceView& view = state->workspaceViews[index];
        if (view.shown && view.kind != ov::CardKind::Frame && boxContains(view.box, lx, ly)) {
          if (outBox != nullptr) {
            *outBox = view.box;
          }
          return group->workspaceAt(index);
        }
      }
    }
    return nullptr;
  }

  Workspace* Overview::thumbDropAt(double lx, double ly, wlr_box* outBox) {
    Workspace* target = thumbAt(lx, ly, outBox);
    const Workspace* source = m_pressWorkspace.get();
    return target != nullptr && source != nullptr && target != source && target->group() == source->group() ? target
                                                                                                            : nullptr;
  }

  void Overview::updateThumbDrag(double lx, double ly) {
    m_dragX = lx;
    m_dragY = ly;
    updateEdgeScroll(lx, ly);
    wlr_box box{};
    Workspace* target = thumbDropAt(lx, ly, &box);
    if (target == nullptr || target->group() == nullptr) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(target->group()->output(), box, kPlusHintRadius, HintLook::Ring);
    scheduleFrames();
  }

  void Overview::endThumbDrag(bool drop, double lx, double ly) {
    if (!m_thumbDragging) {
      m_pressThumb = false;
      return;
    }
    Workspace* target = drop ? thumbDropAt(lx, ly, nullptr) : nullptr;
    Workspace* source = m_pressWorkspace.get();
    m_thumbDragging = false;
    m_pressThumb = false;
    m_pressWorkspace = nullptr;
    hideDropHint();
    stopEdgeScroll();
    m_server->cursor()->overrideCursor(nullptr);
    if (target != nullptr && source != nullptr) {
      source->group()->moveWorkspace(source, target->index());
    }
  }

  void Overview::syncWorkspaceRows(OutputState& state, WorkspaceGroup& group) {
    const bool grew = state.workspaceBackgrounds.size() < group.workspaceCount();
    while (state.workspaceBackgrounds.size() > group.workspaceCount()) {
      WorkspaceBackground background = std::move(state.workspaceBackgrounds.back());
      state.workspaceBackgrounds.pop_back();
      for (const auto& mirror : background.mirrors) {
        mirror->outputSample.disconnect();
        mirror->frameDone.disconnect();
      }
      if (background.tree != nullptr) {
        wlr_scene_node_destroy(&background.tree->node);
      }
    }
    while (state.workspaceBackgrounds.size() < group.workspaceCount()) {
      state.workspaceBackgrounds.push_back(createWorkspaceBackground(state));
    }
    if (grew) {
      syncDesktopMirrors(state);
    }
    for (const auto& card : state.cards) {
      if (card->view != nullptr && card->view->workspace() != nullptr) {
        card->workspaceIndex = card->view->workspace()->index();
      }
    }
  }

  void Overview::showDropHint(const wlr_box& worldBox, const OutputState& state, size_t workspaceIndex) {
    if (worldBox.width <= 0 || worldBox.height <= 0) {
      hideDropHint();
      return;
    }
    const ov::Fit fit = workspaceFit(state, workspaceIndex);
    const double z = fit.s;
    const wlr_box mappedBox{
        .x = static_cast<int>(std::lround(fit.ox + (worldBox.x - state.outputBox.x) * z)),
        .y = static_cast<int>(std::lround(fit.oy + (worldBox.y - state.outputBox.y) * z)),
        .width = std::max(1, static_cast<int>(std::lround(worldBox.width * z))),
        .height = std::max(1, static_cast<int>(std::lround(worldBox.height * z))),
    };
    Output* output = state.output;
    wlr_box visibleBox{};
    if (!wlr_box_intersection(&visibleBox, &mappedBox, &state.outputBox)) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(
        output, visibleBox, static_cast<int>(std::lround(config().appearance.cornerRadius * z)), HintLook::Ring
    );
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
    }
  }

  void Overview::showWorkspaceInsertHint(Output* output, const wlr_box& box) {
    if (output == nullptr || box.width <= 0 || box.height <= 0) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(output, box, std::min(box.width, box.height) / 2, HintLook::Glow);
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
    }
  }

  void Overview::hideDropHint() {
    if (m_dropHint != nullptr) {
      m_dropHint->hideImmediate();
    }
  }

  WorkspaceGroup* Overview::plusTargetAt(double lx, double ly, OutputState** outState, wlr_box* outBox) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (!state->placement.plus.has_value() || group == nullptr || !group->dynamic()
          || group->workspaceCount() >= kMaxWorkspaces) {
        continue;
      }
      const ov::Rect& plus = *state->placement.plus;
      const wlr_box box{
          state->outputBox.x + static_cast<int>(std::lround(plus.x)),
          state->outputBox.y + static_cast<int>(std::lround(plus.y)),
          static_cast<int>(std::lround(plus.w)),
          static_cast<int>(std::lround(plus.h)),
      };
      wlr_box visible{};
      if (wlr_box_intersection(&visible, &box, &state->outputBox) && wlr_box_contains_point(&visible, lx, ly)) {
        *outState = state.get();
        *outBox = visible;
        return group;
      }
    }
    return nullptr;
  }

  void Overview::updateEdgeScroll(double lx, double ly) {
    Output* output = nullptr;
    int step = 0;
    int pan = 0;
    wlr_box edge{};
    wlr_edges strong = WLR_EDGE_NONE;
    OutputState* state = nullptr;
    for (const auto& candidate : m_outputs) {
      if (wlr_box_contains_point(&candidate->outputBox, lx, ly)) {
        state = candidate.get();
        break;
      }
    }
    Workspace* panned = nullptr;
    const ov::Style style = configuredStyle();
    WorkspaceGroup* group = state != nullptr ? state->output->workspaceGroup() : nullptr;
    if (group != nullptr && (style == ov::Style::Strip || style == ov::Style::Spaces)) {
      const wlr_box& box = state->outputBox;
      const bool horizontal = state->cardsHorizontal;
      const int bar = static_cast<int>(kEdgeZone);
      const double x = lx - box.x;
      const double y = ly - box.y;
      // Strip rows step along the workspace axis; the prototype has no row steps in the other styles.
      if (style == ov::Style::Strip) {
        const double along = horizontal ? x : y;
        const double alongExtent = horizontal ? box.width : box.height;
        if (along < kEdgeZone) {
          step = -1;
          edge = horizontal ? wlr_box{box.x, box.y, bar, box.height} : wlr_box{box.x, box.y, box.width, bar};
          strong = horizontal ? WLR_EDGE_LEFT : WLR_EDGE_TOP;
        } else if (along > alongExtent - kEdgeZone) {
          step = 1;
          edge = horizontal ? wlr_box{box.x + box.width - bar, box.y, bar, box.height}
                            : wlr_box{box.x, box.y + box.height - bar, box.width, bar};
          strong = horizontal ? WLR_EDGE_RIGHT : WLR_EDGE_BOTTOM;
        }
      }
      // The left and right edges pan a horizontal scrolling workspace: the Strip row under the pointer, or the
      // Spaces main area.
      const bool panAxisFree = style == ov::Style::Spaces || !horizontal;
      if (step == 0 && panAxisFree && (x < kEdgeZone || x > box.width - kEdgeZone)) {
        Workspace* workspace = nullptr;
        if (style == ov::Style::Spaces) {
          workspace = state->mainBox.has_value() ? group->workspaceAt(state->mainWorkspace) : nullptr;
        } else {
          size_t position = 0;
          OutputState* rowState = nullptr;
          workspace = workspaceAtPoint(lx, ly, &rowState, &position, true);
        }
        if (workspace != nullptr && workspace->scrollingLayout() != nullptr && !workspace->scrollingVertical()) {
          pan = x < kEdgeZone ? -1 : 1;
          panned = workspace;
          edge = pan < 0 ? wlr_box{box.x, box.y, bar, box.height}
                         : wlr_box{box.x + box.width - bar, box.y, bar, box.height};
          strong = pan < 0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
        }
      }
      if (step != 0 || pan != 0) {
        output = state->output;
      }
    }
    m_edgeWorkspace = panned;
    if (output == m_edgeOutput && step == m_edgeStep && pan == m_edgePan) {
      return;
    }
    const bool hintChanged = output != m_edgeOutput || ((step | pan) == 0) != ((m_edgeStep | m_edgePan) == 0);
    Output* previous = m_edgeOutput;
    m_edgeOutput = output;
    m_edgeStep = step;
    m_edgePan = pan;
    if (output == nullptr) {
      if (m_edgeTimer != nullptr) {
        wl_event_source_timer_update(m_edgeTimer, 0);
      }
      if (m_edgeHint != nullptr) {
        m_edgeHint->hideImmediate();
      }
    } else {
      if (m_edgeTimer == nullptr) {
        m_edgeTimer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onEdgeTimer, this);
      }
      wl_event_source_timer_update(m_edgeTimer, step != 0 ? kEdgeDelayMs : 0);
      m_edgePanLastMsec = -1.0;
      if (pan != 0) {
        scheduleFrames();
      }
      if (m_edgeHint == nullptr) {
        m_edgeHint = std::make_unique<HintRect>(*m_server, m_tree);
      }
      // A new edge fades in afresh, as in the prototype: the gradient must not morph across the output.
      m_edgeHint->hideImmediate();
      m_edgeHint->show(output, edge, 0, HintLook::Edge, strong);
      if (m_dragCard != nullptr) {
        wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
      }
    }
    if (hintChanged) {
      const View* liveTarget = liveTargetView();
      for (const auto& candidate : m_outputs) {
        if (candidate->output == output || candidate->output == previous) {
          layoutChrome(*candidate, liveTarget);
        }
      }
    }
    scheduleFrames();
  }

  void Overview::stopEdgeScroll() {
    if (m_edgeTimer != nullptr) {
      wl_event_source_timer_update(m_edgeTimer, 0);
    }
    if (m_edgeHint != nullptr) {
      m_edgeHint->hideImmediate();
    }
    const bool wasArmed = m_edgeOutput != nullptr;
    Output* previous = m_edgeOutput;
    m_edgeOutput = nullptr;
    m_edgeWorkspace = nullptr;
    m_edgeStep = 0;
    m_edgePan = 0;
    if (wasArmed) {
      if (OutputState* state = stateFor(previous); state != nullptr && state->chrome != nullptr) {
        layoutChrome(*state, liveTargetView());
      }
    }
  }

  int Overview::onEdgeTimer(void* data) {
    auto* self = static_cast<Overview*>(data);
    OutputState* state = self->m_edgeOutput != nullptr ? self->stateFor(self->m_edgeOutput) : nullptr;
    if ((self->m_dragCard == nullptr && !self->m_thumbDragging) || state == nullptr || !self->interactive()) {
      self->stopEdgeScroll();
      return 0;
    }
    if (self->m_edgeStep != 0) {
      self->selectRelativeWorkspace(self->m_edgeStep, state->output);
      wl_event_source_timer_update(self->m_edgeTimer, kEdgeRepeatMs);
      return 0;
    }
    self->stopEdgeScroll();
    return 0;
  }

} // namespace andromeda

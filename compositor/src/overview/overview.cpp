#include "overview/overview_internal.h"

namespace andromeda {

  using namespace ovimpl;


  Overview::Overview(Server& server) : m_server(&server) {
    m_server->registerAnimatable(this);
    // An icon that finished loading joins the capsules on the next chrome layout.
    m_appIcons = std::make_unique<ov::AppIcons>(wl_display_get_event_loop(server.display()), [this] {
      if (!m_active) {
        return;
      }
      const View* liveTarget = liveTargetView();
      for (const auto& state : m_outputs) {
        layoutChrome(*state, liveTarget);
        wlr_output_schedule_frame(state->output->wlr());
      }
    });
  }

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
    // A placed card keeps the box its edges snapped to: recomputing the size from the scale rounds it a second time
    // and steps an edge back. The size follows the presented box only when that box really changed since the output
    // was laid out (a client resize, more than the 1 px a second rounding moves), and for the drag, which owns the
    // dragged card's origin.
    const int scaledW = std::max(1, static_cast<int>(std::lround(world.width * z)));
    const int scaledH = std::max(1, static_cast<int>(std::lround(world.height * z)));
    if (&card == m_dragCard || std::abs(scaledW - card.box.width) > 1 || std::abs(scaledH - card.box.height) > 1) {
      card.box.width = scaledW;
      card.box.height = scaledH;
    }
    const int contentW = card.box.width;
    const int contentH = card.box.height;
    wlr_scene_node_set_position(&card.tree->node, card.box.x, card.box.y);
    wlr_scene_tree_set_clip(card.tree, card.clip.width > 0 && card.clip.height > 0 ? &card.clip : nullptr);
    layoutCardShadow(card, desktopChromeAlpha() * card.opacity);
    const float cardOpacity = (&card == m_dragCard ? config().appearance.dragOpacity : 1.0F) * card.opacity;
    const float presentedOpacity = view->presentedOpacity() * cardOpacity;

    const int borderWidth = view->decorationBorderWidth();
    const int outerBorderWidth = view->decorationOuterBorderWidth();
    const int total = borderWidth + outerBorderWidth;
    // The live window's rule (View::surfaceRadius): a 0 px border still rounds the window.
    const bool decorated = view->decorated() && !view->currentFullscreen() && !view->maximizedToEdges();
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
        // Card-local, edges snapped like the card's own, so a subsurface moves and scales with the card's edges.
        const ov::PixelBox sub = ov::snapEdges({
            (entry->sx - geometry.x) * fx,
            (entry->sy - geometry.y) * fy,
            surface->current.width * fx,
            surface->current.height * fy,
        });
        wlr_scene_node_set_enabled(&entry->buffer->node, true);
        wlr_scene_node_set_position(&entry->buffer->node, sub.x, sub.y);
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
      // Blur alpha and strength are normalized to [0, 1]; clamp an overshooting progress.
      const auto level = static_cast<float>(std::clamp(m_progress, 0.0, 1.0));
      if (blur->alpha != level) {
        wlr_scene_blur_set_alpha(blur, level);
      }
      // Full strength samples the cached wallpaper blur (one re-render per change, not one per frame); a strength
      // below 1 re-blurs the whole output live on every frame of the open. The open fades the blur in by alpha.
      if (blur->strength != 1.0F) {
        wlr_scene_blur_set_strength(blur, 1.0F);
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
    std::vector<uintptr_t>& structure = state.nextStructure;
    structure.assign({static_cast<uintptr_t>(style), input.workspaces.size()});
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
    state.structure.swap(structure);
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
    // Every name is built before any view into the list is taken, so no view outlives a reallocation.
    m_chromeNames.resize(state.workspaceViews.size());
    for (size_t index = 0; index < state.workspaceViews.size(); ++index) {
      const Workspace* workspace = group != nullptr ? group->workspaceAt(index) : nullptr;
      m_chromeNames[index].clear();
      if (workspace != nullptr) {
        // An unnamed workspace reads "Desktop 2", not a bare "2" (macOS, Windows).
        const bool named = workspace->named() || workspace->displayName() != workspace->name();
        m_chromeNames[index] = named ? workspace->displayName() : std::format("Desktop {}", workspace->displayName());
      }
    }
    for (size_t index = 0; index < state.workspaceViews.size(); ++index) {
      const WorkspaceView& view = state.workspaceViews[index];
      const Workspace* workspace = group != nullptr ? group->workspaceAt(index) : nullptr;
      m_chromeWorkspaces.push_back({
          .name = m_chromeNames[index],
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
      const char* appId = card.view->appId();
      const int iconPx = static_cast<int>(std::ceil(static_cast<double>(state.output->wlr()->scale))) * 16;
      m_chromeWindows.push_back({
          .key = card.view,
          .title = title != nullptr ? std::string_view(title) : std::string_view(),
          .icon = appId != nullptr ? m_appIcons->get(appId, std::max(16, iconPx)) : nullptr,
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
    std::optional<ov::ChromeMenu> menu;
    if (m_menu.has_value() && m_menu->output == state.output && group != nullptr) {
      const Workspace* workspace = group->workspaceAt(m_menu->workspace);
      const auto row = workspace != nullptr ? std::ranges::find(kMenuModes, workspace->layoutMode()) : kMenuModes.end();
      menu = ov::ChromeMenu{
          .workspace = m_menu->workspace,
          .hovered = m_menu->hovered,
          .current = row != kMenuModes.end() ? static_cast<int>(row - kMenuModes.begin()) : -1,
      };
    }
    std::optional<ov::ChromeWindowMenu> windowMenu;
    if (m_windowMenu.has_value() && m_windowMenu->output == state.output) {
      windowMenu = ov::ChromeWindowMenu{
          .x = m_windowMenu->x,
          .y = m_windowMenu->y,
          .rows = m_windowMenu->rows,
          .hovered = m_windowMenu->hovered,
          .dangerFrom = static_cast<int>(m_windowMenu->rows.size()) - 1,
      };
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
        .menu = menu,
        .windowMenu = windowMenu,
        .renaming = m_rename.has_value() && group != nullptr && m_rename->workspace.get() == group->workspaceAt(current)
            ? std::optional<std::string_view>(m_rename->text)
            : std::nullopt,
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
    applyProgress();

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
    // The wallpaper blur cache holds only the background layer, which the overview leaves as it is: open and close
    // reuse it instead of re-blurring the whole output.
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, true);
    kLog.debug("overview opened on {} output(s)", m_outputs.size());
    return true;
  }

  void Overview::open() {
    ANDROMEDA_ZONE("Overview::open");
    m_server->dismissSnapChrome();
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
    m_menu.reset();
    m_windowMenu.reset();
    finishRename(true);
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
    // A zoom already in flight hands on its speed, so a reversed toggle retargets instead of restarting (7.1.5).
    if (releaseVelocity == 0.0 && m_zoomAnim.animating()) {
      releaseVelocity = m_zoomAnim.velocity() * (m_targetProgress - m_progressFrom);
    }
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
    m_menu.reset();
    m_rename.reset();
    m_dragCard = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_cardPresentationDirty = false;
    m_gestureOpenedHere = false;
    m_filter.clear();
    m_server->reconcileDynamicWorkspaces();
    m_server->scheduleSuspensionSync();
  }

  // -: gesture

  void Overview::gestureUpdate(double progress) {
    bool opened = false;
    if (!m_active) {
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedHere = true;
      opened = true;
      m_progress = 0.0;
    }
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    m_closing = false;
    m_server->notifyOverviewChanged();
    // Past the open state the swipe meets resistance instead of a wall (rulebook 7.7 rubber band, iOS constants): the
    // overscroll approaches kOverscroll and gestureEnd springs it back to 1.
    constexpr double kOverscroll = 0.12;
    constexpr double kResistance = 0.55;
    const double beyond = progress - 1.0;
    const double target =
        beyond <= 0.0 ? progress : 1.0 + (1.0 - 1.0 / (beyond * kResistance / kOverscroll + 1.0)) * kOverscroll;
    // Opening builds every card before the first frame, and the events queued meanwhile can already reach the end:
    // when the finger is more than a step ahead of what is drawn, the zoom chases it on the frame clock, keeping its
    // speed across events, so a long or fast swipe still shows the motion (rulebook 7.7).
    constexpr double kMaxStep = 0.08;
    const bool chasing = m_zoomAnim.animating();
    if (opened || chasing || std::abs(target - m_progress) > kMaxStep) {
      const double span = target - m_progress;
      if (std::abs(span) < 1e-4) {
        m_zoomAnim.snap(0.0);
        m_progress = target;
        m_targetProgress = target;
        applyProgress();
        return;
      }
      const double progressPerSec = chasing ? m_zoomAnim.velocity() * (m_targetProgress - m_progressFrom) : 0.0;
      m_progressFrom = m_progress;
      m_targetProgress = target;
      m_zoomAnim.snap(0.0);
      const auto& overview = config().animation.overview;
      if (config().animation.enabled && overview.enabled && overview.curve.easing == Easing::Spring) {
        m_zoomAnim.settleSpring(1.0, overview.curve.spring, progressPerSec / span);
        applyProgress();
        return;
      }
    }
    m_zoomAnim.snap(0.0);
    m_progress = target;
    m_targetProgress = m_progress;
    // Touchpads report faster than the display: lay the cards out once on the next frame, not per event.
    m_cardPresentationDirty = true;
    scheduleFrames();
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
    applyProgress();
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
      dropCard(view);
    } else if (card == nullptr && state != nullptr && view->workspace() != nullptr) {
      createCard(*state, view, view->workspace()->index());
    }
    if (state != nullptr) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
    applyProgress();
  }

  void Overview::onViewFloatingChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    // Cards stack tiled, then floating (populateCards): rebuild so this one
    // moves to its new layer instead of keeping its old spot.
    rebuildCard(view);
    applyProgress();
  }

  void Overview::onViewUnmapped(View* view, bool animateClose) {
    if (!m_active || view == nullptr) {
      return;
    }
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
    applyProgress();
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
      applyProgress();
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
      applyProgress();
      return;
    }

    card->workspaceIndex = workspace->index();
    if (source == target) {
      layoutOutput(*target);
      wlr_output_schedule_frame(target->output->wlr());
      applyProgress();
      return;
    }
    if (source == nullptr) {
      rebuildCard(view);
      applyProgress();
      return;
    }

    const auto it = std::ranges::find_if(source->cards, [card](const std::unique_ptr<Card>& candidate) {
      return candidate.get() == card;
    });
    if (it == source->cards.end()) {
      rebuildCard(view);
      applyProgress();
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
    applyProgress();
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
    applyProgress();
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
    applyProgress();
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
      applyProgress();
    }
  }

} // namespace andromeda

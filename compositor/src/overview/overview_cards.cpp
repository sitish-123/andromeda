#include "overview/overview_internal.h"
#include "server/dsk_surface.h"

namespace andromeda {

  using namespace ovimpl;

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
    if (DskSurfaceManager* dsk = card->overview->m_server->dskSurfaces()) {
      dsk->markMirror(buffer, surface);
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
    // A card made during a search starts at its filter level instead of fading to it.
    card->filter.snap(m_filter.empty() || cardMatches(*card) ? 1.0 : kFilterMissOpacity);
    Card* raw = card.get();
    state.cards.push_back(std::move(card));

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
    DskSurfaceManager* dsk = m_server->dskSurfaces();
    for (const auto& entry : card.surfaces) {
      wlr_scene_buffer* source = entry->buffer;
      if (source == nullptr || source->buffer == nullptr || !source->node.enabled) {
        continue;
      }
      wlr_scene_buffer* copy = wlr_scene_buffer_create(surfaces, source->buffer);
      if (copy == nullptr) {
        continue;
      }
      if (dsk != nullptr && dsk->secureCopy(source)) {
        dsk->markSecureCopy(copy);
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
    if (m_pointer.forget(*card)) {
      hideDropHint();
      stopEdgeScroll();
      m_dragSourceWorkspace = nullptr;
      m_dragSourceWidth.reset();
      m_drop = {};
      m_dropWorkspaceGroup = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
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
      return;
    }
  }

  void Overview::rebuildCard(View* view) {
    const Card* old = findCard(view);
    const bool grows = old != nullptr && old->grows;
    dropCard(view);
    Workspace* workspace = view->workspace();
    if (workspace == nullptr || !view->mapped()) {
      return;
    }
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr) {
      return;
    }
    if (Card* card = createCard(*state, view, workspace->index())) {
      card->grows = grows;
    }
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
      const auto backgroundsStart = std::chrono::steady_clock::now();
      state->workspaceBackgrounds.reserve(group->workspaceCount());
      for (size_t row = 0; row < group->workspaceCount(); ++row) {
        state->workspaceBackgrounds.push_back(createWorkspaceBackground(*state));
      }
      state->tileShadows = wlr_scene_tree_create(state->tree);
      state->activeWorkspaceIndex = group->active() != nullptr ? group->active()->index() : 0;
      state->rowScroll.snap(static_cast<double>(state->activeWorkspaceIndex));
      OutputState* raw = state.get();
      m_outputs.push_back(std::move(state));
      const auto cardsStart = std::chrono::steady_clock::now();
      populateCards(*raw);
      const auto cardsEnd = std::chrono::steady_clock::now();
      raw->chrome = std::make_unique<ov::Chrome>(raw->tree);
      const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
      if (ms(backgroundsStart, std::chrono::steady_clock::now()) > 2.0) {
        kLog.debug(
            "overview build, output {}: {} workspace backgrounds {:.1f} ms, {} cards {:.1f} ms, chrome {:.1f} ms",
            m_outputs.size(), group->workspaceCount(), ms(backgroundsStart, cardsStart), raw->cards.size(),
            ms(cardsStart, cardsEnd), ms(cardsEnd, std::chrono::steady_clock::now())
        );
      }
    }
  }

} // namespace andromeda

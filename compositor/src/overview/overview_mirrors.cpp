#include "overview/overview_internal.h"

namespace andromeda {

  using namespace ovimpl;

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
    std::vector<DesktopEntry>& resolved = state.resolvedDesktop;
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

} // namespace andromeda

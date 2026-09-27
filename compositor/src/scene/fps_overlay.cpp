#include "scene/fps_overlay.h"

#include "config/config.h"
#include "output/output.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <wayland-server-core.h>

namespace {

  constexpr int kTickMs = 1000;
  constexpr int kMargin = 12;
  constexpr int kPadding = 8;

  std::string vrrLabel(const wlr_output& output) {
    if (output.adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) {
      return "VRR on";
    }
    return output.adaptive_sync_supported ? "VRR off" : "no VRR";
  }

} // namespace

namespace umbriel {

  FpsOverlay::FpsOverlay(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  FpsOverlay::~FpsOverlay() {
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
    }
  }

  void FpsOverlay::toggle() {
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
      m_timer = nullptr;
      if (m_tree != nullptr) {
        wlr_scene_node_destroy(&m_tree->node);
        m_tree = nullptr;
      }
      m_stats.clear();
      return;
    }
    m_timer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server.display()), onTick, this);
    wl_event_source_timer_update(m_timer, kTickMs);
    render();
  }

  void FpsOverlay::onPresent(wlr_output* output, const wlr_output_event_present& event) {
    if (!event.presented) {
      return;
    }
    Stats& stats = m_stats[output];
    const std::int64_t now = static_cast<std::int64_t>(event.when.tv_sec) * 1'000'000'000 + event.when.tv_nsec;
    if (stats.lastNs != 0) {
      const std::int64_t interval = now - stats.lastNs;
      stats.minIntervalNs = stats.minIntervalNs == 0 ? interval : std::min(stats.minIntervalNs, interval);
      stats.maxIntervalNs = std::max(stats.maxIntervalNs, interval);
    }
    stats.lastNs = now;
    ++stats.presents;
  }

  int FpsOverlay::onTick(void* data) {
    auto* self = static_cast<FpsOverlay*>(data);
    self->render();
    wl_event_source_timer_update(self->m_timer, kTickMs);
    return 0;
  }

  void FpsOverlay::render() {
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
    }
    m_tree = wlr_scene_tree_create(m_parent);
    const auto& colors = config().colors;

    for (const auto& output : m_server.outputs()) {
      wlr_output* wlr = output->wlr();
      if (!wlr->enabled) {
        continue;
      }
      Stats stats = m_stats[wlr];
      m_stats[wlr] = Stats{.lastNs = stats.lastNs};

      std::string line = std::format("{}  {:.0f} Hz · {} fps · {}", wlr->name, wlr->refresh / 1000.0, stats.presents,
                                     vrrLabel(*wlr));
      if (stats.minIntervalNs > 0) {
        line += std::format("\nflip {:.1f}–{:.1f} ms", stats.minIntervalNs / 1e6, stats.maxIntervalNs / 1e6);
      }
      TextBufferResult rendered = renderTextBuffer({
          .markup = std::format("<span foreground='{}'>{}</span>", rgbaHex(colors.textPrimary), escapeMarkup(line)),
          .font = "monospace 10",
          .padding = kPadding,
          .scale = std::max(1.0, std::ceil(static_cast<double>(wlr->scale))),
          .bgR = colors.background[0],
          .bgG = colors.background[1],
          .bgB = colors.background[2],
          .bgA = 0.85,
      });
      if (rendered.buffer == nullptr) {
        continue;
      }
      const wlr_box box = output->usableArea();
      wlr_scene_buffer* buffer = wlr_scene_buffer_create(m_tree, rendered.buffer);
      wlr_buffer_drop(rendered.buffer);
      if (buffer != nullptr) {
        wlr_scene_buffer_set_dest_size(buffer, rendered.logicalWidth, rendered.logicalHeight);
        wlr_scene_node_set_position(
            &buffer->node, box.x + box.width - rendered.logicalWidth - kMargin, box.y + kMargin
        );
      }
    }
  }

} // namespace umbriel

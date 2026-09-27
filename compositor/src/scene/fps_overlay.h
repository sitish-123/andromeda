#pragma once

#include <cstdint>
#include <unordered_map>

struct wl_event_source;
struct wlr_output;
struct wlr_output_event_present;
struct wlr_scene_tree;

namespace umbriel {
  class Server;

  // Per-output readout in the top-right corner of the usable area (below a bar): the mode's refresh rate, frames actually presented in the last
  // second, the shortest and longest flip interval, and adaptive-sync state. Counts present events, so with VRR on
  // the flip rate is the panel's refresh rate down to its minimum. Redrawing once a second adds one frame per second.
  class FpsOverlay {
  public:
    FpsOverlay(Server& server, wlr_scene_tree* parent);
    ~FpsOverlay();

    FpsOverlay(const FpsOverlay&) = delete;
    FpsOverlay& operator=(const FpsOverlay&) = delete;

    void toggle();
    [[nodiscard]] bool visible() const { return m_timer != nullptr; }
    void onPresent(wlr_output* output, const wlr_output_event_present& event);

  private:
    struct Stats {
      std::uint32_t presents = 0;
      std::int64_t lastNs = 0;
      std::int64_t minIntervalNs = 0;
      std::int64_t maxIntervalNs = 0;
    };

    static int onTick(void* data);
    void render();

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    wl_event_source* m_timer = nullptr;
    std::unordered_map<wlr_output*, Stats> m_stats;
  };

} // namespace umbriel

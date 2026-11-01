#pragma once

#include "render/animation/animation_manager.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Box;
class Button;
class DskSurfaceClient;
class ConfigService;
class Glyph;
class Label;
class Node;
class ProgressBar;
class RenderContext;
class WaylandConnection;
struct wl_output;

// One running task shown in the top-center island: a file transfer, a call, a recording, or media.
struct LiveActivity {
  std::string glyph;
  std::string title;
  std::string subtitle;
  // 0..1; no bar when empty.
  std::optional<float> progress;
  std::string actionGlyph;
  std::function<void()> action;
  // Shown before the main action, for example mute before decline.
  std::string secondaryGlyph;
  std::function<void()> secondaryAction;
};

// The live activity island: an overlay surface at the top center that takes the place of the bar's center island
// while an activity runs. It grows from the bar island height to 52 px and shrinks back when the activity ends.
class LiveActivityIsland {
public:
  using VisibilityCallback = std::function<void(bool)>;

  LiveActivityIsland();
  ~LiveActivityIsland();

  LiveActivityIsland(const LiveActivityIsland&) = delete;
  LiveActivityIsland& operator=(const LiveActivityIsland&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  // Runs with true when the island shows and false when it is gone, so the bar can hide its center island.
  void setVisibilityCallback(VisibilityCallback callback);
  void setActivity(std::optional<LiveActivity> activity);
  void onOutputChange();

private:
  struct Instance {
    wl_output* output = nullptr;
    float width = 0.0F;
    std::unique_ptr<LayerSurface> surface;
    // Our compositor morphs the island (clip and radius); null on other compositors, where the shell resizes it.
    std::unique_ptr<DskSurfaceClient> dsk;
    // sceneRoot must be destroyed before `animations`: ~Node() cancels its animations.
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    Box* island = nullptr;
    Node* content = nullptr;
    Glyph* icon = nullptr;
    Label* title = nullptr;
    Label* subtitle = nullptr;
    ProgressBar* progress = nullptr;
    Button* action = nullptr;
    Button* secondary = nullptr;
    float reveal = 0.0F;
    AnimationManager::Id revealAnimId = 0;
  };

  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, float width, float height);
  void applyContent(Instance& inst);
  void applyReveal(Instance& inst, float reveal);
  void animateReveal(Instance& inst, float to);
  [[nodiscard]] std::int32_t topMargin() const;
  void notifyVisibility(bool visible);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  VisibilityCallback m_visibilityCallback;
  std::optional<LiveActivity> m_activity;
  // Kept while the island shrinks after the activity ended.
  LiveActivity m_shown;
  bool m_visible = false;
  std::vector<std::unique_ptr<Instance>> m_instances;
};

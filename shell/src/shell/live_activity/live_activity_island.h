#pragma once

#include "pipewire/privacy_filter.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/center_island_space.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class Box;
class Button;
class DskSurfaceClient;
class ConfigService;
class Glyph;
class InputArea;
class NotificationManager;
class MprisService;
class PipeWireService;
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
  bool actionDestructive = false; // danger fill, for example decline
  // Shown before the main action, for example mute before decline.
  std::string secondaryGlyph;
  std::function<void()> secondaryAction;
};

// The center island of the top or bottom islands bar. At rest it is a pill in the bar row with the privacy dots and the
// unread notification count. A live activity grows it away from the bar edge, over windows, with a shadow and tint.
class LiveActivityIsland {
public:
  using VisibilityCallback = std::function<void(wl_output*, bool)>;
  using SpaceProvider = std::function<std::optional<CenterIslandSpace>(wl_output*)>;

  LiveActivityIsland();
  ~LiveActivityIsland();

  LiveActivityIsland(const LiveActivityIsland&) = delete;
  LiveActivityIsland& operator=(const LiveActivityIsland&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  // Runs per output with true when its island shows and false when it is gone, so the bar can hide that center section.
  void setVisibilityCallback(VisibilityCallback callback);
  void setSpaceProvider(SpaceProvider provider);
  // Sources of the rest content, and the click on the rest pill.
  void setRestSources(
      std::function<PipeWireService*()> pipewire, std::function<MprisService*()> mpris, NotificationManager* notifications,
      std::function<void(wl_output*)> open
  );
  // Date text at rest changes at midnight.
  void onTimeTick();
  void setActivity(std::optional<LiveActivity> activity);
  // The bar layout, the config, the privacy state or the unread count changed.
  void refresh();
  // A callable that refreshes while this island lives and does nothing after.
  [[nodiscard]] std::function<void()> refresher();
  void onOutputChange();

private:
  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    float radius = 0.0F;
  };

  struct Instance {
    wl_output* output = nullptr;
    float width = 0.0F; // surface width: the widest live card
    CenterIslandSpace space;
    float liveWidth = 0.0F;
    float restWidth = 0.0F;
    std::unique_ptr<LayerSurface> surface;
    // Our compositor morphs the island (clip and radius) and draws its shadow; null on other compositors.
    std::unique_ptr<DskSurfaceClient> dsk;
    // sceneRoot must be destroyed before `animations`: ~Node() cancels its animations.
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    Box* material = nullptr;
    Box* tint = nullptr;
    Box* ring = nullptr;
    InputArea* restArea = nullptr;
    Node* content = nullptr;
    Node* restContent = nullptr;
    Box* micDot = nullptr;
    Box* cameraDot = nullptr;
    Box* screenDot = nullptr;
    Node* mediaGroup = nullptr;
    Label* date = nullptr;
    Box* divider = nullptr;
    Label* count = nullptr;
    Glyph* icon = nullptr;
    Label* title = nullptr;
    Label* subtitle = nullptr;
    ProgressBar* progress = nullptr;
    Button* action = nullptr;
    Button* secondary = nullptr;
    float reveal = 0.0F;
    // Where reveal is heading; the size follows it at once when motion is reduced.
    float target = 0.0F;
    AnimationManager::Id revealAnimId = 0;
    std::optional<Rect> dskTarget;
  };

  void refreshNow();
  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, float width, float height);
  void applyContent(Instance& inst);
  void applyReveal(Instance& inst, float reveal);
  void animateReveal(Instance& inst, float to);
  void syncDsk(Instance& inst, bool animate);
  [[nodiscard]] Rect restRect(const Instance& inst) const;
  [[nodiscard]] Rect liveRect(const Instance& inst) const;
  void syncVisibility();

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  VisibilityCallback m_visibilityCallback;
  SpaceProvider m_spaceProvider;
  std::function<PipeWireService*()> m_pipewire;
  std::function<MprisService*()> m_mpris;
  std::string m_date;
  NotificationManager* m_notifications = nullptr;
  std::function<void(wl_output*)> m_open;
  PrivacyFilter m_micFilter;
  PrivacyFilter m_camFilter;
  PrivacyFilter m_screenFilter;
  std::optional<LiveActivity> m_activity;
  // Kept while the island shrinks after the activity ended.
  LiveActivity m_shown;
  std::unordered_set<wl_output*> m_visibleOutputs;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  bool m_refreshScheduled = false;
  std::vector<std::unique_ptr<Instance>> m_instances;
};

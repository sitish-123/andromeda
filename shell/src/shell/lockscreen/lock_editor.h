#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/core/shared_texture_cache.h"
#include "shell/lockscreen/lock_alert.h"
#include "shell/lockscreen/lock_cards.h"
#include "shell/lockscreen/lock_editor_bar.h"
#include "shell/lockscreen/lock_editor_free.h"
#include "shell/lockscreen/lock_editor_gallery.h"
#include "shell/lockscreen/lock_editor_slots.h"
#include "ui/auth/auth_column.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

class ConfigService;
class DesktopWidgetFactory;
struct DesktopWidgetRuntimeServices;
class Label;
class LockScreen;
class Node;
class RectNode;
class RenderContext;
class WallpaperNode;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

// The lock screen editor (DESIGN.md "Lock screen v2", mock section 2): a full-screen Overlay layer surface that previews
// the lock screen while the session is unlocked, with the editor bar on top. It edits a draft; Done writes the draft to
// the config and Escape drops it. It never runs on the session-lock surface.
class LockEditor {
public:
  LockEditor() = default;
  ~LockEditor();

  LockEditor(const LockEditor&) = delete;
  LockEditor& operator=(const LockEditor&) = delete;

  void initialize(
      WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache,
      LockScreen* lockScreen, const DesktopWidgetRuntimeServices& widgetServices
  );
  // Opens on the output under the pointer. No-op while open.
  void open();
  // Drops the draft.
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

private:
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(float width, float height);
  void layoutScene(float width, float height);
  void applyWallpaper();
  using Focus = std::variant<LockEditorBar::Hit, LockEditorGallery::Hit, LockEditorSlots::Hit, LockEditorFree::Hit>;
  void activate(LockEditorBar::Hit hit);
  void activateGallery(LockEditorGallery::Hit hit);
  // keyboard = opened by a key: the focus moves into the opened panel.
  void activateSlots(LockEditorSlots::Hit hit, bool keyboard);
  void closePanels();
  // Free mode: the lock widgets of this output as boxes; add, select, move by arrows (resize with Ctrl), Delete.
  void loadFreeItems(float width, float height);
  void activateFree(LockEditorFree::Hit hit);
  void nudgeFree(int dx, int dy, bool big, bool resize);
  void saveFree();
  // A layout pick drops this output's Free widgets: it asks first (rulebook 9.10) while there are any.
  [[nodiscard]] bool hasFreeWidgets() const;
  void pickLayout(int index);
  void closeAlert(bool confirm);
  // The draft widget list as the row shows it: an empty list in Split = that layout's own row.
  [[nodiscard]] std::vector<std::string> shownWidgets() const;
  void setFocus(const Focus& focus);
  [[nodiscard]] LockscreenConfig previewConfig() const;
  void moveFocus(int step);
  void save();
  void refresh();
  void startClock();

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  LockScreen* m_lockScreen = nullptr;

  std::unique_ptr<LayerSurface> m_surface;
  AnimationManager m_animations;
  std::unique_ptr<Node> m_root;
  WallpaperNode* m_wallpaper = nullptr;
  RectNode* m_dim = nullptr;
  Label* m_themeNotice = nullptr;
  AuthColumn m_auth;
  LockEditorBar m_bar;
  LockEditorBar::State m_draft;
  LockEditorGallery m_gallery;
  LockEditorGallery::State m_clock;
  LockCards m_cards;
  LockEditorSlots m_slots;
  LockEditorSlots::State m_slotState;
  // Draft lockscreen.widgets; written on Done only after an edit (an untouched empty list stays empty).
  std::vector<std::string> m_widgets;
  bool m_widgetsEdited = false;
  // Builds the Free boxes' live widgets (the lock's own widget types and services).
  std::unique_ptr<DesktopWidgetFactory> m_widgetFactory;
  LockEditorFree m_free;
  LockEditorFree::State m_freeState;
  bool m_freeLoaded = false;
  bool m_freeEdited = false;
  AuthRect m_freeDragStart;
  // Confirmed layout change: Done removes this output's Free widgets.
  bool m_freeDiscard = false;
  // A layout was picked in this session; without one Done keeps the saved layout (custom shows as Centered).
  bool m_layoutPicked = false;
  LockAlert m_alert;
  int m_pendingLayout = -1;
  // Frames left to wait for keyboard focus after open; -1 = focus logged.
  int m_focusFrames = 0;
  bool m_alertConfirmFocus = false;
  lock_free::Zones m_zones;
  float m_width = 0.0F;
  float m_height = 0.0F;
  float m_scale = 1.0F;

  std::string m_connector;
  std::uint32_t m_outputName = 0;
  std::string m_wallpaperPath;
  bool m_ownWallpaper = false;
  SharedTextureCache::Lease m_wallpaperLease;
  Timer m_clockTimer;
};

#pragma once

#include "wayland/output_scale.h"
#include "wayland/wayland_seat.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct wl_compositor;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_seat;
struct wl_shm;
struct wl_subcompositor;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct xdg_wm_base;
struct wp_cursor_shape_manager_v1;
struct ext_idle_notifier_v1;
struct ext_idle_notification_v1;
struct zwp_idle_inhibit_manager_v1;
struct ext_background_effect_manager_v1;
struct dsk_surface_manager_v1;
struct xdg_activation_v1;
struct zwp_text_input_manager_v3;
struct hyprland_focus_grab_manager_v1;
struct wp_fractional_scale_manager_v1;
struct wp_viewporter;
struct zwlr_output_manager_v1;
struct zwlr_output_head_v1;
struct zwlr_output_mode_v1;
class LayerSurface;
class FocusGrabService;
class TextInputService;

struct WaylandOutput {
  std::uint32_t name = 0;
  std::string interfaceName;
  std::string connectorName;
  std::string description;
  // From wlr-output-management-unstable-v1; wl_output has no serial. May stay empty.
  std::string make;
  std::string model;
  std::string serialNumber;
  std::uint32_t version = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t logicalWidth = 0;
  std::int32_t logicalHeight = 0;
  std::int32_t logicalX = 0;
  std::int32_t logicalY = 0;
  std::int32_t transform = 0;
  // wlr-output-management transform, when published for this output.
  std::optional<std::int32_t> headTransform;
  zxdg_output_v1* xdgOutput = nullptr;
  bool done = false;
  // wlr-output-management fixed scale factor for this output (0 == not reported).
  double headScaleFactor = 0.0;
  // Canonical published render scale as a /120 numerator (120 == 1.0). Resolved
  // from head scale, then mode/logical ratio, then integer wl_output.scale.
  std::int32_t configuredScaleNumerator = wayland::kScaleNumeratorBase;

  [[nodiscard]] float configuredScale() const noexcept {
    return static_cast<float>(configuredScaleNumerator) / static_cast<float>(wayland::kScaleNumeratorBase);
  }

  [[nodiscard]] std::int32_t effectiveTransform() const noexcept { return headTransform.value_or(transform); }

  [[nodiscard]] std::int32_t effectiveLogicalWidth() const noexcept {
    if (logicalWidth > 0) {
      return logicalWidth;
    }
    if (width <= 0 || scale <= 0) {
      return 0;
    }
    const std::int32_t scaled = width / scale;
    return scaled > 0 ? scaled : 1;
  }

  [[nodiscard]] std::int32_t effectiveLogicalHeight() const noexcept {
    if (logicalHeight > 0) {
      return logicalHeight;
    }
    if (height <= 0 || scale <= 0) {
      return 0;
    }
    const std::int32_t scaled = height / scale;
    return scaled > 0 ? scaled : 1;
  }

  [[nodiscard]] bool hasUsableGeometry() const noexcept {
    return effectiveLogicalWidth() > 0 && effectiveLogicalHeight() > 0;
  }
};

// Accumulates head fields until matched to a WaylandOutput, keyed by head proxy.
struct WaylandOutputHeadInfo {
  std::string name;
  std::string make;
  std::string model;
  std::string serialNumber;
  // wlr-output-management fixed scale factor (0 == not reported by this head).
  double scaleFactor = 0.0;
  std::optional<std::int32_t> transform;
};

class WaylandConnection {
public:
  enum class Purpose : std::uint8_t {
    Shell,
    Screencopy,
  };

  explicit WaylandConnection(Purpose purpose = Purpose::Shell);
  virtual ~WaylandConnection();

  WaylandConnection(const WaylandConnection&) = delete;
  WaylandConnection& operator=(const WaylandConnection&) = delete;

  using ChangeCallback = std::function<void()>;

  bool connect();
  bool
  connectUntil(std::chrono::steady_clock::time_point deadline, WaylandConnection* eventConnection, std::string& error);

  // Delegate setters
  void setOutputChangeCallback(ChangeCallback callback);
  void setOutputLifecycleCallbacks(std::function<void(wl_output*)> added, std::function<void(wl_output*)> removed);
  void setPointerEventCallback(WaylandSeat::PointerEventCallback callback);
  void setKeyboardEventCallback(WaylandSeat::KeyboardEventCallback callback);
  void setKeyboardModifiersCallback(WaylandSeat::KeyboardModifiersCallback callback);
  void setLockKeysChangeCallback(WaylandSeat::LockKeysChangeCallback callback);
  void setKeyboardLayoutChangeCallback(WaylandSeat::KeyboardLayoutChangeCallback callback);
  /// Fired when both `ext_idle_notifier_v1` and `wl_seat` are bound (including late registry globals).
  void setIdleCapabilitiesReadyCallback(ChangeCallback callback);
  void setTextInputService(TextInputService* textInputService);
  void setCursorShape(std::uint32_t serial, std::uint32_t shape);

  [[nodiscard]] int repeatPollTimeoutMs() const;
  void repeatTick();
  void stopKeyRepeat();

  // Queries
  [[nodiscard]] bool isConnected() const noexcept;
  [[nodiscard]] bool hasRequiredGlobals() const noexcept;
  [[nodiscard]] bool hasLayerShell() const noexcept;
  [[nodiscard]] bool hasSubcompositor() const noexcept;
  [[nodiscard]] bool hasXdgOutputManager() const noexcept;
  [[nodiscard]] bool hasXdgShell() const noexcept;
  [[nodiscard]] bool hasIdleNotifier() const noexcept;
  [[nodiscard]] bool hasIdleInhibitManager() const noexcept;
  [[nodiscard]] bool hasFractionalScale() const noexcept;
  [[nodiscard]] bool hasOutputManagement() const noexcept;
  [[nodiscard]] bool hasBackgroundEffectBlur() const noexcept;
  [[nodiscard]] ext_background_effect_manager_v1* backgroundEffectManager() const noexcept;
  // Our compositor only (dsk_surface_v1: compositor-run shape, elevation, motion, gestures); nullptr elsewhere.
  [[nodiscard]] dsk_surface_manager_v1* dskSurfaceManager() const noexcept { return m_dskSurfaceManager; }
  [[nodiscard]] wp_fractional_scale_manager_v1* fractionalScaleManager() const noexcept;
  [[nodiscard]] hyprland_focus_grab_manager_v1* hyprlandFocusGrabManager() const noexcept;
  [[nodiscard]] FocusGrabService* focusGrabService() const noexcept;
  [[nodiscard]] TextInputService* textInputService() const noexcept { return m_textInputService; }
  [[nodiscard]] wp_viewporter* viewporter() const noexcept;
  [[nodiscard]] wl_display* display() const noexcept;
  [[nodiscard]] wl_registry* registry() const noexcept { return m_registry; }
  /// Registry name and bound version of zwlr_output_manager_v1, 0 when absent.
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> outputManagerGlobal() const noexcept {
    return {m_outputManagerGlobalName, m_outputManagerGlobalVersion};
  }
  [[nodiscard]] std::string describeDisplayError(int operationErrno = 0) const;
  [[nodiscard]] wl_compositor* compositor() const noexcept;
  [[nodiscard]] wl_seat* seat() const noexcept;
  [[nodiscard]] wl_pointer* pointer() const noexcept;
  [[nodiscard]] wl_shm* shm() const noexcept;
  [[nodiscard]] wl_subcompositor* subcompositor() const noexcept;
  [[nodiscard]] zwlr_layer_shell_v1* layerShell() const noexcept;
  [[nodiscard]] xdg_wm_base* xdgWmBase() const noexcept;
  [[nodiscard]] ext_idle_notifier_v1* idleNotifier() const noexcept;
  /// Inhibitor-aware idle notification (`get_idle_notification`); honors `zwp_idle_inhibitor_v1`.
  [[nodiscard]] ext_idle_notification_v1* createIdleNotification(std::uint32_t timeoutMs) const;
  /// Input-only idle notification (`get_input_idle_notification`, notifier v2); ignores idle inhibitors.
  /// Returns null when the compositor's notifier is older than v2.
  [[nodiscard]] ext_idle_notification_v1* createInputIdleNotification(std::uint32_t timeoutMs) const;
  [[nodiscard]] zwp_idle_inhibit_manager_v1* idleInhibitManager() const noexcept;
  [[nodiscard]] const std::vector<WaylandOutput>& outputs() const noexcept;
  [[nodiscard]] WaylandOutput* findOutputByWl(wl_output* wlOutput);
  [[nodiscard]] const WaylandOutput* findOutputByWl(wl_output* wlOutput) const;
  [[nodiscard]] WaylandOutput* findOutputByXdg(zxdg_output_v1* xdgOutput);

  [[nodiscard]] bool hasXdgActivation() const noexcept;
  // appId marks the token as an app launch (the compositor shows the busy cursor until its window maps).
  [[nodiscard]] std::string requestActivationToken(wl_surface* surface, const std::string& appId = {}) const;
  // The same token without a blocking roundtrip: `done` runs from the event loop with it ("" without xdg-activation).
  void requestActivationTokenAsync(
      wl_surface* surface, const std::string& appId, std::function<void(std::string token)> done
  ) const;
  void activateSurface(wl_surface* surface);

  [[nodiscard]] wl_output* lastPointerOutput() const noexcept;
  [[nodiscard]] wl_surface* lastPointerSurface() const noexcept;
  [[nodiscard]] wl_surface* lastKeyboardSurface() const noexcept;
  [[nodiscard]] std::uint32_t keyboardModifiers() const noexcept;
  [[nodiscard]] bool hasPointerPosition() const noexcept;
  [[nodiscard]] double lastPointerX() const noexcept;
  [[nodiscard]] double lastPointerY() const noexcept;
  [[nodiscard]] WaylandSeat::InputSource lastInputSource() const noexcept;
  [[nodiscard]] std::string currentKeyboardLayoutName() const;
  [[nodiscard]] std::vector<std::string> keyboardLayoutNames() const;
  [[nodiscard]] WaylandSeat::LockKeysState keyboardLockKeysState() const;
  [[nodiscard]] std::uint32_t lastInputSerial() const noexcept;
  [[nodiscard]] double userIdleSeconds() const noexcept;
  [[nodiscard]] bool hasFreshPointerOutput(std::chrono::milliseconds maxAge) const noexcept;
  [[nodiscard]] wl_output* outputForSurface(wl_surface* surface) const noexcept;

  void registerSurfaceOutput(wl_surface* surface, wl_output* output);
  void notifySurfaceOutputEnter(wl_surface* surface, wl_output* output);
  void notifySurfaceOutputLeave(wl_surface* surface, wl_output* output);
  void registerLayerSurface(const LayerSurface& layerSurface);
  void unregisterSurface(wl_surface* surface);
  [[nodiscard]] zwlr_layer_surface_v1* layerSurfaceFor(wl_surface* surface) const noexcept;
  [[nodiscard]] const LayerSurface* layerSurfaceOwnerFor(wl_surface* surface) const noexcept;
  void notifyOutputReady(wl_output* output);

  // Registry listener entrypoints
  static void
  handleGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  static void handleGlobalRemove(void* data, wl_registry* registry, std::uint32_t name);

  void onBackgroundEffectCapabilities(std::uint32_t capabilities) noexcept;
  void notifyIdleCapabilitiesReady();

  // wlr-output-management-unstable-v1 entrypoints
  void onOutputManagerHead(zwlr_output_head_v1* head);
  void onOutputManagerDone();
  void onOutputManagerFinished(zwlr_output_manager_v1* manager);
  void onOutputHeadName(zwlr_output_head_v1* head, const char* name);
  void onOutputHeadMake(zwlr_output_head_v1* head, const char* make);
  void onOutputHeadModel(zwlr_output_head_v1* head, const char* model);
  void onOutputHeadSerialNumber(zwlr_output_head_v1* head, const char* serialNumber);
  void onOutputHeadMode(zwlr_output_head_v1* head, zwlr_output_mode_v1* mode);
  void onOutputHeadScale(zwlr_output_head_v1* head, double scaleFactor);
  void onOutputHeadTransform(zwlr_output_head_v1* head, std::int32_t transform);
  void onOutputModeFinished(zwlr_output_mode_v1* mode);
  void onOutputHeadFinished(zwlr_output_head_v1* head);
  // wl_output.name and the done event race; call from both sides to match either order.
  void matchPendingOutputHeads();
  // Recompute out.configuredScaleNumerator from current head/mode/logical/wl-scale
  // data. Returns true when the published numerator changed.
  bool recomputeConfiguredScale(WaylandOutput& out);

protected:
  // Binds a global the toolkit does not own. Returns true when it handled `interface`.
  virtual bool bindExtensionGlobal(
      wl_registry* /*registry*/, std::uint32_t /*name*/, std::string_view /*interface*/, std::uint32_t /*version*/
  ) {
    return false;
  }
  virtual void onSeatBound() {}
  // Releases extension globals. The base destructor cannot dispatch to it: a subclass destructor calls it too.
  virtual void cleanupExtensions() {}
  virtual void logExtensionSummary() const {}
  [[nodiscard]] Purpose purpose() const noexcept { return m_purpose; }

private:
  void bindGlobal(wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  void bindTextInputService();
  void cleanup();
  [[nodiscard]] bool setupDisplay(wl_display* display, std::string& error);
  void logStartupSummary() const;

  Purpose m_purpose = Purpose::Shell;
  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  wl_seat* m_seat = nullptr;
  wl_shm* m_shm = nullptr;
  wl_subcompositor* m_subcompositor = nullptr;
  zwlr_layer_shell_v1* m_layerShell = nullptr;
  zxdg_output_manager_v1* m_xdgOutputManager = nullptr;
  xdg_wm_base* m_xdgWmBase = nullptr;
  wp_cursor_shape_manager_v1* m_cursorShapeManager = nullptr;
  xdg_activation_v1* m_xdgActivation = nullptr;
  ext_idle_notifier_v1* m_idleNotifier = nullptr;
  zwp_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
  ext_background_effect_manager_v1* m_backgroundEffectManager = nullptr;
  dsk_surface_manager_v1* m_dskSurfaceManager = nullptr;
  wp_fractional_scale_manager_v1* m_fractionalScaleManager = nullptr;
  hyprland_focus_grab_manager_v1* m_hyprlandFocusGrabManager = nullptr;
  zwlr_output_manager_v1* m_outputManager = nullptr;
  std::uint32_t m_outputManagerGlobalName = 0;
  std::uint32_t m_outputManagerGlobalVersion = 0;
  std::unordered_map<zwlr_output_head_v1*, WaylandOutputHeadInfo> m_outputHeads;
  std::unordered_set<zwlr_output_mode_v1*> m_outputModes;
  std::unique_ptr<FocusGrabService> m_focusGrabService;
  wp_viewporter* m_viewporter = nullptr;
  bool m_backgroundEffectBlurSupported = false;
  zwp_text_input_manager_v3* m_textInputManager = nullptr;
  TextInputService* m_textInputService = nullptr;
  bool m_hasLayerShellGlobal = false;
  std::vector<WaylandOutput> m_outputs;
  ChangeCallback m_outputChangeCallback;
  ChangeCallback m_idleCapabilitiesReadyCallback;
  std::function<void(wl_output*)> m_outputAddedCallback;
  std::function<void(wl_output*)> m_outputRemovedCallback;
  std::unordered_map<wl_surface*, wl_output*> m_surfaceOutputMap;
  std::unordered_map<wl_surface*, std::vector<wl_output*>> m_surfaceOutputs;
  std::unordered_map<wl_surface*, const LayerSurface*> m_layerSurfaceMap;
  wl_output* m_lastPointerOutput = nullptr;
  std::chrono::steady_clock::time_point m_lastPointerOutputAt;
  WaylandSeat::PointerEventCallback m_pointerEventCallback;

  WaylandSeat m_seatHandler;
};

namespace wayland {

  struct DispatchTarget {
    WaylandConnection* connection = nullptr;
    std::string_view role;
  };

  enum class DispatchStatus : std::uint8_t {
    Completed,
    TimedOut,
    PollFailed,
    ConnectionFailed,
  };

  struct DispatchResult {
    DispatchStatus status = DispatchStatus::Completed;
    WaylandConnection* failedConnection = nullptr;
    std::string error;
  };

  [[nodiscard]] DispatchResult dispatchUntil(
      std::span<const DispatchTarget> targets, std::chrono::steady_clock::time_point deadline,
      const std::function<bool()>& completed
  );

} // namespace wayland

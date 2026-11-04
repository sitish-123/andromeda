#include "compositors/shell_wayland_connection.h"

#include "clipboard/clipboard_service.h"
#include "compositors/compositor_detect.h"
#include "compositors/virtual_keyboard_service.h"
#include "compositors/wayland_protocol_policy.h"
#include "core/log.h"
#include "desktop-unstable-v1-client-protocol.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "hyprland-toplevel-mapping-v1-client-protocol.h"
#include "org-kde-plasma-virtual-desktop-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr std::uint32_t kExtWorkspaceManagerVersion = 1;
  constexpr std::uint32_t kKdeVirtualDesktopManagementVersion = 4;
  constexpr std::uint32_t kWlrForeignToplevelManagerVersion = 3;
  constexpr std::uint32_t kExtForeignToplevelListVersion = 1;
  constexpr std::uint32_t kExtSessionLockManagerVersion = 1;
  constexpr std::uint32_t kHyprlandToplevelMappingManagerVersion = 1;
  constexpr std::uint32_t kVirtualKeyboardManagerVersion = 1;
  constexpr std::uint32_t kGammaControlManagerVersion = 1;
  constexpr std::uint32_t kDesktopShellVersion = 4;
  constexpr std::uint32_t kScreencopyManagerVersion = 3;
  constexpr std::uint32_t kImageCopyCaptureManagerVersion = 1;
  constexpr std::uint32_t kOutputImageCaptureSourceManagerVersion = 1;
  constexpr std::uint32_t kForeignToplevelImageCaptureSourceManagerVersion = 1;

  constexpr Logger kLog("wayland");

  void shellAction(void* data, dsk_shell_v1* /*shell*/, const char* command) {
    auto* self = static_cast<ShellWaylandConnection*>(data);
    self->onShellAction(command);
  }

  void shellLockKeys(void* data, dsk_shell_v1* /*shell*/, uint32_t caps, uint32_t num, uint32_t scroll) {
    auto* self = static_cast<ShellWaylandConnection*>(data);
    self->onCompositorLockKeys({.capsLock = caps != 0, .numLock = num != 0, .scrollLock = scroll != 0});
  }

  void shellBell(void* data, dsk_shell_v1* /*shell*/, const char* appId) {
    auto* self = static_cast<ShellWaylandConnection*>(data);
    self->onShellBell(appId);
  }

  void shellTextInputState(
      void* data, dsk_shell_v1* /*shell*/, uint32_t enabled, uint32_t purpose, uint32_t hint, int32_t x, int32_t y,
      int32_t width, int32_t height
  ) {
    auto* self = static_cast<ShellWaylandConnection*>(data);
    self->onShellTextInputState({
        .enabled = enabled != 0, .purpose = purpose, .hint = hint, .x = x, .y = y, .width = width, .height = height
    });
  }

  void shellRelock(void* data, dsk_shell_v1* /*shell*/) { static_cast<ShellWaylandConnection*>(data)->onShellRelock(); }

  void shellRecovered(void* data, dsk_shell_v1* /*shell*/) {
    static_cast<ShellWaylandConnection*>(data)->onShellRecovered();
  }

  void shellRestarted(void* data, dsk_shell_v1* /*shell*/) {
    static_cast<ShellWaylandConnection*>(data)->onShellRestarted();
  }

  constexpr dsk_shell_v1_listener kShellListener = {
      .action = shellAction,
      .lock_keys = shellLockKeys,
      .bell = shellBell,
      .text_input_state = shellTextInputState,
      .relock = shellRelock,
      .recovered = shellRecovered,
      .restarted = shellRestarted,
  };

  template <typename T> T* bindAs(wl_registry* registry, std::uint32_t name, const wl_interface& iface, std::uint32_t version) {
    return static_cast<T*>(wl_registry_bind(registry, name, &iface, version));
  }

} // namespace

ShellWaylandConnection::ShellWaylandConnection(Purpose purpose) : WaylandConnection(purpose) {}

// The base destructor cannot reach the override, so release shell globals before the display closes.
ShellWaylandConnection::~ShellWaylandConnection() { cleanupExtensions(); }

void ShellWaylandConnection::setWorkspaceManagerCallbacks(
    std::function<void(ext_workspace_manager_v1*)> extWorkspace, std::function<void(zdwl_ipc_manager_v2*)> dwlIpc
) {
  m_extWorkspaceManagerCallback = std::move(extWorkspace);
  m_dwlIpcManagerCallback = std::move(dwlIpc);
}

void ShellWaylandConnection::setKdeVirtualDesktopManagerCallback(
    std::function<void(org_kde_plasma_virtual_desktop_management*)> callback
) {
  m_kdeVirtualDesktopManagerCallback = std::move(callback);
}

void ShellWaylandConnection::setToplevelChangeCallback(ChangeCallback callback) {
  m_toplevelsHandler.setChangeCallback(callback);
  m_extForeignToplevels.setChangeCallback(std::move(callback));
}

void ShellWaylandConnection::setHyprlandToplevelMappingManagerCallback(
    std::function<void(hyprland_toplevel_mapping_manager_v1* manager)> callback
) {
  m_hyprlandToplevelMappingManagerCallback = std::move(callback);
}

void ShellWaylandConnection::setShellActionCallback(std::function<void(const std::string& command)> callback) {
  m_shellActionCallback = std::move(callback);
}

void ShellWaylandConnection::setShellBellCallback(std::function<void(const std::string& appId)> callback) {
  m_shellBellCallback = std::move(callback);
}

void ShellWaylandConnection::setCompositorLockKeysCallback(ChangeCallback callback) {
  m_compositorLockKeysCallback = std::move(callback);
}

void ShellWaylandConnection::setTextInputStateCallback(std::function<void(const TextInputState& state)> callback) {
  m_textInputStateCallback = std::move(callback);
}

void ShellWaylandConnection::setRelockCallback(std::function<void()> callback) {
  m_relockCallback = std::move(callback);
  if (m_relockCallback && std::exchange(m_relockPending, false)) {
    m_relockCallback();
  }
}

void ShellWaylandConnection::setRecoveredCallback(std::function<void()> callback) {
  m_recoveredCallback = std::move(callback);
}

bool ShellWaylandConnection::sendAppActivity(std::uint32_t pid, std::uint32_t flags) {
  if (m_desktopShell == nullptr || dsk_shell_v1_get_version(m_desktopShell) < DSK_SHELL_V1_APP_ACTIVITY_SINCE_VERSION) {
    return false;
  }
  dsk_shell_v1_app_activity(m_desktopShell, pid, flags);
  return true;
}

bool ShellWaylandConnection::insertText(const std::string& text) {
  if (m_desktopShell == nullptr || dsk_shell_v1_get_version(m_desktopShell) < DSK_SHELL_V1_INSERT_TEXT_SINCE_VERSION) {
    return false;
  }
  dsk_shell_v1_insert_text(m_desktopShell, text.c_str());
  return true;
}

bool ShellWaylandConnection::sendKey(std::uint32_t keysym, bool pressed) {
  if (m_desktopShell == nullptr || dsk_shell_v1_get_version(m_desktopShell) < DSK_SHELL_V1_KEY_SINCE_VERSION) {
    return false;
  }
  dsk_shell_v1_key(m_desktopShell, keysym, pressed ? 1 : 0);
  return true;
}

void ShellWaylandConnection::onShellTextInputState(const TextInputState& state) {
  if (m_textInputStateCallback) {
    m_textInputStateCallback(state);
  }
}

// Both can arrive in the bind roundtrip, before the application sets its callbacks: they wait until then.
void ShellWaylandConnection::onShellRelock() {
  if (m_relockCallback) {
    m_relockCallback();
  } else {
    m_relockPending = true;
  }
}

void ShellWaylandConnection::onShellRestarted() {
  if (m_restartedCallback) {
    m_restartedCallback();
  } else {
    m_restartedPending = true;
  }
}

void ShellWaylandConnection::setRestartedCallback(std::function<void()> callback) {
  m_restartedCallback = std::move(callback);
  if (m_restartedCallback && std::exchange(m_restartedPending, false)) {
    m_restartedCallback();
  }
}

void ShellWaylandConnection::onShellRecovered() {
  if (m_recoveredCallback) {
    m_recoveredCallback();
  }
}

void ShellWaylandConnection::onShellAction(const std::string& command) {
  if (m_shellActionCallback) {
    m_shellActionCallback(command);
  }
}

void ShellWaylandConnection::onShellBell(const std::string& appId) {
  if (m_shellBellCallback) {
    m_shellBellCallback(appId);
  }
}

void ShellWaylandConnection::onCompositorLockKeys(const WaylandSeat::LockKeysState& state) {
  m_compositorLockKeys = state;
  m_hasCompositorLockKeys = true;
  if (m_compositorLockKeysCallback) {
    m_compositorLockKeysCallback();
  }
}

void ShellWaylandConnection::setClipboardService(ClipboardService* clipboardService) {
  m_clipboardService = clipboardService;
  bindClipboardService();
}

void ShellWaylandConnection::setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService) {
  m_virtualKeyboardService = virtualKeyboardService;
  bindVirtualKeyboardService();
}

bool ShellWaylandConnection::setOutputsPower(bool on) {
  if (m_outputPowerManager == nullptr) {
    return false;
  }
  const auto mode = on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF;
  for (const WaylandOutput& output : outputs()) {
    zwlr_output_power_v1* power = zwlr_output_power_manager_v1_get_output_power(m_outputPowerManager, output.output);
    zwlr_output_power_v1_set_mode(power, mode);
    zwlr_output_power_v1_destroy(power);
  }
  wl_display_flush(display());
  return true;
}

std::optional<ActiveToplevel> ShellWaylandConnection::activeToplevel() const { return m_toplevelsHandler.current(); }

std::optional<ActiveToplevel> ShellWaylandConnection::matchToplevelByTitleAndAppId(
    std::string_view title, std::string_view appId, wl_output* preferredOutput
) const {
  return m_toplevelsHandler.matchByTitleAndAppId(title, appId, preferredOutput);
}

wl_output* ShellWaylandConnection::activeToplevelOutput() const { return m_toplevelsHandler.currentOutput(); }

bool ShellWaylandConnection::hasFullscreenToplevel() const { return m_toplevelsHandler.anyFullscreen(); }

std::vector<std::string> ShellWaylandConnection::runningAppIds(wl_output* outputFilter) const {
  if (compositors::isKde() && m_extForeignToplevels.isBound() && !m_hasForeignToplevelManagerGlobal) {
    (void)outputFilter;
    return m_extForeignToplevels.allAppIds();
  }
  return m_toplevelsHandler.allAppIds(outputFilter);
}

std::vector<ToplevelInfo> ShellWaylandConnection::windowsForApp(
    const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter
) const {
  if (compositors::isKde() && m_extForeignToplevels.isBound() && !m_hasForeignToplevelManagerGlobal) {
    (void)outputFilter;
    return m_extForeignToplevels.windowsForApp(idLower, wmClassLower);
  }
  return m_toplevelsHandler.windowsForApp(idLower, wmClassLower, outputFilter);
}

std::vector<ToplevelInfo> ShellWaylandConnection::windowsWithoutAppId(wl_output* outputFilter) const {
  if (compositors::isKde() && m_extForeignToplevels.isBound() && !m_hasForeignToplevelManagerGlobal) {
    (void)outputFilter;
    return m_extForeignToplevels.windowsWithoutAppId();
  }
  return m_toplevelsHandler.windowsWithoutAppId(outputFilter);
}

std::vector<ToplevelInfo>
ShellWaylandConnection::extWindowsForApp(const std::string& idLower, const std::string& wmClassLower) const {
  if (!m_extForeignToplevels.isBound()) {
    return {};
  }
  return m_extForeignToplevels.windowsForApp(idLower, wmClassLower);
}

std::vector<ToplevelInfo> ShellWaylandConnection::extWindowsWithoutAppId() const {
  return m_extForeignToplevels.isBound() ? m_extForeignToplevels.windowsWithoutAppId() : std::vector<ToplevelInfo>{};
}

ext_foreign_toplevel_handle_v1* ShellWaylandConnection::extHandleForIdentifier(std::string_view identifier) const {
  return m_extForeignToplevels.isBound() ? m_extForeignToplevels.extHandleForIdentifier(identifier) : nullptr;
}

bool ShellWaylandConnection::containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_toplevelsHandler.containsWlrHandle(handle);
}

void ShellWaylandConnection::activateToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_toplevelsHandler.activateHandle(handle, seat());
}

bool ShellWaylandConnection::minimizeToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  return m_toplevelsHandler.minimizeHandle(handle);
}

std::optional<bool> ShellWaylandConnection::toplevelMinimized(zwlr_foreign_toplevel_handle_v1* handle) const {
  return m_toplevelsHandler.minimized(handle);
}

void ShellWaylandConnection::closeToplevel(zwlr_foreign_toplevel_handle_v1* handle) {
  m_toplevelsHandler.closeHandle(handle);
}

bool ShellWaylandConnection::bindExtensionGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interfaceName, std::uint32_t version
) {
  // A screencopy connection binds only wl_shm and wl_output (base) and the screencopy manager.
  if (purpose() == Purpose::Screencopy && interfaceName != zwlr_screencopy_manager_v1_interface.name) {
    return false;
  }

  if (interfaceName == ext_workspace_manager_v1_interface.name) {
    m_hasExtWorkspaceGlobal = true;
    auto* manager = bindAs<ext_workspace_manager_v1>(
        registry, name, ext_workspace_manager_v1_interface, std::min(version, kExtWorkspaceManagerVersion)
    );
    if (m_extWorkspaceManagerCallback) {
      m_extWorkspaceManagerCallback(manager);
    } else {
      ext_workspace_manager_v1_destroy(manager);
    }
    return true;
  }

  if (interfaceName == org_kde_plasma_virtual_desktop_management_interface.name) {
    m_hasKdeVirtualDesktopGlobal = true;
    auto* manager = bindAs<org_kde_plasma_virtual_desktop_management>(
        registry, name, org_kde_plasma_virtual_desktop_management_interface,
        std::min(version, kKdeVirtualDesktopManagementVersion)
    );
    if (m_kdeVirtualDesktopManagerCallback) {
      m_kdeVirtualDesktopManagerCallback(manager);
    } else {
      org_kde_plasma_virtual_desktop_management_destroy(manager);
    }
    return true;
  }

  if (interfaceName == zdwl_ipc_manager_v2_interface.name) {
    m_hasDwlIpcGlobal = true;
    auto* manager = bindAs<zdwl_ipc_manager_v2>(registry, name, zdwl_ipc_manager_v2_interface, 2);
    if (m_dwlIpcManagerCallback) {
      m_dwlIpcManagerCallback(manager);
    } else {
      zdwl_ipc_manager_v2_release(manager);
    }
    return true;
  }

  if (interfaceName == zwlr_foreign_toplevel_manager_v1_interface.name) {
    m_hasForeignToplevelManagerGlobal = true;
    m_toplevelsHandler.bind(bindAs<zwlr_foreign_toplevel_manager_v1>(
        registry, name, zwlr_foreign_toplevel_manager_v1_interface, std::min(version, kWlrForeignToplevelManagerVersion)
    ));
    return true;
  }

  if (interfaceName == ext_foreign_toplevel_list_v1_interface.name) {
    // ext handles carry the exact identifier joined with Niri/Andromeda IPC window ids and are the source for toplevel
    // image capture. Keep the wlr manager bound as well because other shell features still consume its richer state.
    if (!wayland_protocol_policy::shouldBindExtForeignToplevelList(compositors::detect())) {
      return true;
    }
    m_hasExtForeignToplevelListGlobal = true;
    m_extForeignToplevels.bind(
        bindAs<ext_foreign_toplevel_list_v1>(
            registry, name, ext_foreign_toplevel_list_v1_interface, std::min(version, kExtForeignToplevelListVersion)
        ),
        display()
    );
    return true;
  }

  if (interfaceName == ext_session_lock_manager_v1_interface.name) {
    m_sessionLockManager = bindAs<ext_session_lock_manager_v1>(
        registry, name, ext_session_lock_manager_v1_interface, std::min(version, kExtSessionLockManagerVersion)
    );
    return true;
  }

  if (interfaceName == hyprland_toplevel_mapping_manager_v1_interface.name) {
    auto* manager = bindAs<hyprland_toplevel_mapping_manager_v1>(
        registry, name, hyprland_toplevel_mapping_manager_v1_interface,
        std::min(version, kHyprlandToplevelMappingManagerVersion)
    );
    if (m_hyprlandToplevelMappingManagerCallback) {
      m_hyprlandToplevelMappingManagerCallback(manager);
    } else {
      hyprland_toplevel_mapping_manager_v1_destroy(manager);
    }
    return true;
  }

  if (interfaceName == ext_data_control_manager_v1_interface.name) {
    if (m_dataControlManager != nullptr && m_dataControlOps != extDataControlOps()) {
      m_dataControlOps->destroyManager(m_dataControlManager);
      m_dataControlManager = nullptr;
    }
    if (m_dataControlManager == nullptr) {
      m_dataControlManager = extDataControlOps()->bindManager(registry, name, version);
      m_dataControlOps = extDataControlOps();
      bindClipboardService();
    }
    return true;
  }

  if (interfaceName == zwlr_data_control_manager_v1_interface.name) {
    if (m_dataControlManager == nullptr) {
      m_dataControlManager = wlrDataControlOps()->bindManager(registry, name, version);
      m_dataControlOps = wlrDataControlOps();
      bindClipboardService();
    }
    return true;
  }

  if (interfaceName == zwp_virtual_keyboard_manager_v1_interface.name) {
    m_virtualKeyboardManager = bindAs<zwp_virtual_keyboard_manager_v1>(
        registry, name, zwp_virtual_keyboard_manager_v1_interface, std::min(version, kVirtualKeyboardManagerVersion)
    );
    bindVirtualKeyboardService();
    return true;
  }

  if (interfaceName == zwlr_gamma_control_manager_v1_interface.name) {
    m_gammaControlManager = bindAs<zwlr_gamma_control_manager_v1>(
        registry, name, zwlr_gamma_control_manager_v1_interface, std::min(version, kGammaControlManagerVersion)
    );
    return true;
  }

  if (interfaceName == dsk_shell_v1_interface.name) {
    m_desktopShell =
        bindAs<dsk_shell_v1>(registry, name, dsk_shell_v1_interface, std::min(version, kDesktopShellVersion));
    dsk_shell_v1_add_listener(m_desktopShell, &kShellListener, this);
    return true;
  }

  if (interfaceName == zwlr_output_power_manager_v1_interface.name) {
    m_outputPowerManager =
        bindAs<zwlr_output_power_manager_v1>(registry, name, zwlr_output_power_manager_v1_interface, 1);
    return true;
  }

  if (interfaceName == zwlr_screencopy_manager_v1_interface.name) {
    m_screencopyManager = bindAs<zwlr_screencopy_manager_v1>(
        registry, name, zwlr_screencopy_manager_v1_interface, std::min(version, kScreencopyManagerVersion)
    );
    return true;
  }

  if (interfaceName == ext_image_copy_capture_manager_v1_interface.name) {
    m_imageCopyCaptureManager = bindAs<ext_image_copy_capture_manager_v1>(
        registry, name, ext_image_copy_capture_manager_v1_interface, std::min(version, kImageCopyCaptureManagerVersion)
    );
    return true;
  }

  if (interfaceName == ext_output_image_capture_source_manager_v1_interface.name) {
    m_outputImageCaptureSourceManager = bindAs<ext_output_image_capture_source_manager_v1>(
        registry, name, ext_output_image_capture_source_manager_v1_interface,
        std::min(version, kOutputImageCaptureSourceManagerVersion)
    );
    return true;
  }

  if (interfaceName == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
    m_foreignToplevelImageCaptureSourceManager = bindAs<ext_foreign_toplevel_image_capture_source_manager_v1>(
        registry, name, ext_foreign_toplevel_image_capture_source_manager_v1_interface,
        std::min(version, kForeignToplevelImageCaptureSourceManagerVersion)
    );
    return true;
  }

  if (interfaceName == "dsk_output_manager_v1") {
    m_desktopOutputGlobalName = name;
    m_desktopOutputGlobalVersion = std::min(version, 1U);
    return true;
  }

  if (interfaceName == "dsk_settings_manager_v1") {
    m_desktopSettingsGlobalName = name;
    m_desktopSettingsGlobalVersion = std::min(version, 2U);
    return true;
  }

  return false;
}

void ShellWaylandConnection::onSeatBound() { bindClipboardService(); }

void ShellWaylandConnection::bindClipboardService() {
  if (m_clipboardService == nullptr || m_dataControlManager == nullptr || m_dataControlOps == nullptr
      || seat() == nullptr) {
    return;
  }
  m_clipboardService->bind(m_dataControlManager, m_dataControlOps, seat());
}

void ShellWaylandConnection::bindVirtualKeyboardService() {
  if (m_virtualKeyboardService == nullptr || m_virtualKeyboardManager == nullptr || seat() == nullptr) {
    return;
  }
  m_virtualKeyboardService->bind(m_virtualKeyboardManager, seat());
}

void ShellWaylandConnection::cleanupExtensions() {
  if (m_clipboardService != nullptr) {
    m_clipboardService->cleanup();
  }
  if (m_virtualKeyboardService != nullptr) {
    m_virtualKeyboardService->cleanup();
  }
  m_toplevelsHandler.cleanup();
  m_extForeignToplevels.cleanup();

  const auto destroy = [](auto*& proxy, auto destroyFn) {
    if (proxy != nullptr) {
      destroyFn(proxy);
      proxy = nullptr;
    }
  };
  destroy(m_sessionLockManager, ext_session_lock_manager_v1_destroy);
  destroy(m_gammaControlManager, zwlr_gamma_control_manager_v1_destroy);
  destroy(m_outputPowerManager, zwlr_output_power_manager_v1_destroy);
  destroy(m_desktopShell, dsk_shell_v1_destroy);
  destroy(m_screencopyManager, zwlr_screencopy_manager_v1_destroy);
  destroy(m_imageCopyCaptureManager, ext_image_copy_capture_manager_v1_destroy);
  destroy(m_outputImageCaptureSourceManager, ext_output_image_capture_source_manager_v1_destroy);
  destroy(m_foreignToplevelImageCaptureSourceManager, ext_foreign_toplevel_image_capture_source_manager_v1_destroy);
  destroy(m_virtualKeyboardManager, zwp_virtual_keyboard_manager_v1_destroy);
  if (m_dataControlManager != nullptr && m_dataControlOps != nullptr) {
    m_dataControlOps->destroyManager(m_dataControlManager);
    m_dataControlManager = nullptr;
    m_dataControlOps = nullptr;
  }

  m_hasExtWorkspaceGlobal = false;
  m_hasDwlIpcGlobal = false;
  m_hasForeignToplevelManagerGlobal = false;
  m_extWorkspaceManagerCallback = nullptr;
  m_kdeVirtualDesktopManagerCallback = nullptr;
  m_dwlIpcManagerCallback = nullptr;
}

void ShellWaylandConnection::logExtensionSummary() const {
  kLog.info(
      "shell globals ext-workspace={} kde-vd={} dwl-ipc={} session-lock={} gamma-control={}",
      hasExtWorkspaceManager() ? "yes" : "no", hasKdeVirtualDesktopManager() ? "yes" : "no",
      hasDwlIpcManager() ? "yes" : "no", hasSessionLockManager() ? "yes" : "no", hasGammaControl() ? "yes" : "no"
  );
}

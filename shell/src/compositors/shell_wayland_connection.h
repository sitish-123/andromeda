#pragma once

#include "compositors/ext_foreign_toplevels.h"
#include "compositors/wayland_toplevels.h"
#include "wayland/wayland_connection.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_session_lock_manager_v1;
struct ext_workspace_manager_v1;
struct dsk_shell_v1;
struct hyprland_toplevel_mapping_manager_v1;
struct org_kde_plasma_virtual_desktop_management;
struct zdwl_ipc_manager_v2;
struct zwlr_foreign_toplevel_handle_v1;
struct zwlr_gamma_control_manager_v1;
struct zwlr_output_power_manager_v1;
struct zwlr_screencopy_manager_v1;
struct zwp_virtual_keyboard_manager_v1;
class ClipboardService;
struct DataControlOps;
class VirtualKeyboardService;

// The shell's Wayland connection: the toolkit connection plus desktop-shell protocols (workspaces, toplevel
// lists, session lock, clipboard, capture, gamma, output power, dsk_shell_v1).
class ShellWaylandConnection : public WaylandConnection {
public:
  explicit ShellWaylandConnection(Purpose purpose = Purpose::Shell);
  ~ShellWaylandConnection() override;

  void setWorkspaceManagerCallbacks(
      std::function<void(ext_workspace_manager_v1*)> extWorkspace, std::function<void(zdwl_ipc_manager_v2*)> dwlIpc
  );
  void setKdeVirtualDesktopManagerCallback(std::function<void(org_kde_plasma_virtual_desktop_management*)> callback);
  void setToplevelChangeCallback(ChangeCallback callback);
  void setHyprlandToplevelMappingManagerCallback(
      std::function<void(hyprland_toplevel_mapping_manager_v1* manager)> callback
  );
  // dsk_shell_v1: keybind actions, lock-key state, and system bells from the compositor.
  void setShellActionCallback(std::function<void(const std::string& command)> callback);
  void setShellBellCallback(std::function<void(const std::string& appId)> callback);
  void setCompositorLockKeysCallback(ChangeCallback callback);
  [[nodiscard]] bool hasCompositorLockKeys() const noexcept { return m_hasCompositorLockKeys; }
  [[nodiscard]] WaylandSeat::LockKeysState compositorLockKeys() const noexcept { return m_compositorLockKeys; }
  void onShellAction(const std::string& command);
  void onShellBell(const std::string& appId);
  void onCompositorLockKeys(const WaylandSeat::LockKeysState& state);
  void setClipboardService(ClipboardService* clipboardService);
  void setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService);

  [[nodiscard]] bool hasExtWorkspaceManager() const noexcept { return m_hasExtWorkspaceGlobal; }
  [[nodiscard]] bool hasKdeVirtualDesktopManager() const noexcept { return m_hasKdeVirtualDesktopGlobal; }
  [[nodiscard]] bool hasDwlIpcManager() const noexcept { return m_hasDwlIpcGlobal; }
  [[nodiscard]] bool hasForeignToplevelManager() const noexcept { return m_hasForeignToplevelManagerGlobal; }
  [[nodiscard]] bool hasExtForeignToplevelList() const noexcept { return m_hasExtForeignToplevelListGlobal; }
  [[nodiscard]] bool hasSessionLockManager() const noexcept { return m_sessionLockManager != nullptr; }
  [[nodiscard]] bool hasGammaControl() const noexcept { return m_gammaControlManager != nullptr; }
  [[nodiscard]] bool hasScreencopy() const noexcept { return m_screencopyManager != nullptr; }
  [[nodiscard]] zwlr_screencopy_manager_v1* screencopyManager() const noexcept { return m_screencopyManager; }
  [[nodiscard]] ext_image_copy_capture_manager_v1* imageCopyCaptureManager() const noexcept {
    return m_imageCopyCaptureManager;
  }
  [[nodiscard]] ext_output_image_capture_source_manager_v1* outputImageCaptureSourceManager() const noexcept {
    return m_outputImageCaptureSourceManager;
  }
  [[nodiscard]] ext_foreign_toplevel_image_capture_source_manager_v1*
  foreignToplevelImageCaptureSourceManager() const noexcept {
    return m_foreignToplevelImageCaptureSourceManager;
  }
  [[nodiscard]] zwlr_gamma_control_manager_v1* gammaControlManager() const noexcept { return m_gammaControlManager; }
  // Powers every output on or off through wlr-output-power-management. False when the compositor lacks it.
  bool setOutputsPower(bool on);
  [[nodiscard]] zwp_virtual_keyboard_manager_v1* virtualKeyboardManager() const noexcept {
    return m_virtualKeyboardManager;
  }
  [[nodiscard]] ext_session_lock_manager_v1* sessionLockManager() const noexcept { return m_sessionLockManager; }
  /// Registry name and version of the desktop fork's dsk_output_manager_v1, 0 when the compositor lacks it.
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> desktopOutputGlobal() const noexcept {
    return {m_desktopOutputGlobalName, m_desktopOutputGlobalVersion};
  }
  /// Registry name and version of the desktop fork's dsk_settings_manager_v1, 0 when the compositor lacks it.
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> desktopSettingsGlobal() const noexcept {
    return {m_desktopSettingsGlobalName, m_desktopSettingsGlobalVersion};
  }

  [[nodiscard]] std::optional<ActiveToplevel> activeToplevel() const;
  [[nodiscard]] std::optional<ActiveToplevel>
  matchToplevelByTitleAndAppId(std::string_view title, std::string_view appId, wl_output* preferredOutput) const;
  [[nodiscard]] wl_output* activeToplevelOutput() const;
  [[nodiscard]] bool hasFullscreenToplevel() const;
  [[nodiscard]] std::vector<std::string> runningAppIds(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo> windowsWithoutAppId(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo>
  extWindowsForApp(const std::string& idLower, const std::string& wmClassLower) const;
  [[nodiscard]] std::vector<ToplevelInfo> extWindowsWithoutAppId() const;
  [[nodiscard]] ext_foreign_toplevel_handle_v1* extHandleForIdentifier(std::string_view identifier) const;
  [[nodiscard]] bool containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const;
  template <typename Fn> void visitExtToplevelHandles(Fn&& fn) const {
    m_extForeignToplevels.visitExtHandles(std::forward<Fn>(fn));
  }
  void activateToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  [[nodiscard]] bool minimizeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  [[nodiscard]] std::optional<bool> toplevelMinimized(zwlr_foreign_toplevel_handle_v1* handle) const;
  void closeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  template <typename Fn> void visitWlrToplevelHandles(Fn&& fn) const {
    m_toplevelsHandler.visitWlrHandles(std::forward<Fn>(fn));
  }
  template <typename Fn> void visitWlrToplevels(Fn&& fn) const {
    m_toplevelsHandler.visitWlrToplevels(std::forward<Fn>(fn));
  }

protected:
  bool bindExtensionGlobal(
      wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
  ) override;
  void onSeatBound() override;
  void cleanupExtensions() override;
  void logExtensionSummary() const override;

private:
  void bindClipboardService();
  void bindVirtualKeyboardService();

  ext_session_lock_manager_v1* m_sessionLockManager = nullptr;
  zwlr_gamma_control_manager_v1* m_gammaControlManager = nullptr;
  zwlr_output_power_manager_v1* m_outputPowerManager = nullptr;
  dsk_shell_v1* m_desktopShell = nullptr;
  std::function<void(const std::string&)> m_shellActionCallback;
  std::function<void(const std::string&)> m_shellBellCallback;
  ChangeCallback m_compositorLockKeysCallback;
  WaylandSeat::LockKeysState m_compositorLockKeys;
  bool m_hasCompositorLockKeys = false;
  zwlr_screencopy_manager_v1* m_screencopyManager = nullptr;
  ext_image_copy_capture_manager_v1* m_imageCopyCaptureManager = nullptr;
  ext_output_image_capture_source_manager_v1* m_outputImageCaptureSourceManager = nullptr;
  ext_foreign_toplevel_image_capture_source_manager_v1* m_foreignToplevelImageCaptureSourceManager = nullptr;
  std::uint32_t m_desktopOutputGlobalName = 0;
  std::uint32_t m_desktopOutputGlobalVersion = 0;
  std::uint32_t m_desktopSettingsGlobalName = 0;
  std::uint32_t m_desktopSettingsGlobalVersion = 0;
  void* m_dataControlManager = nullptr;
  const DataControlOps* m_dataControlOps = nullptr;
  zwp_virtual_keyboard_manager_v1* m_virtualKeyboardManager = nullptr;
  ClipboardService* m_clipboardService = nullptr;
  VirtualKeyboardService* m_virtualKeyboardService = nullptr;
  bool m_hasExtWorkspaceGlobal = false;
  bool m_hasKdeVirtualDesktopGlobal = false;
  bool m_hasDwlIpcGlobal = false;
  bool m_hasForeignToplevelManagerGlobal = false;
  bool m_hasExtForeignToplevelListGlobal = false;
  std::function<void(ext_workspace_manager_v1*)> m_extWorkspaceManagerCallback;
  std::function<void(org_kde_plasma_virtual_desktop_management*)> m_kdeVirtualDesktopManagerCallback;
  std::function<void(zdwl_ipc_manager_v2*)> m_dwlIpcManagerCallback;
  std::function<void(hyprland_toplevel_mapping_manager_v1*)> m_hyprlandToplevelMappingManagerCallback;

  WaylandToplevels m_toplevelsHandler;
  WaylandExtForeignToplevels m_extForeignToplevels;
};

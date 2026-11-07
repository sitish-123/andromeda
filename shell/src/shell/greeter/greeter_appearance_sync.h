#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

class CompositorPlatform;
class ConfigService;
class IpcService;
struct ShellGreeterSyncConfig;

namespace process {
  struct RunResult;
}

namespace greeter {

  namespace detail {

    enum class ApplyHelperProtocol : std::uint8_t {
      Unknown,
      Legacy,
      SecureSyncV1,
    };

    [[nodiscard]] ApplyHelperProtocol classifyApplyHelperProtocol(const process::RunResult& result);

  } // namespace detail

  enum class GreeterSyncLaunch {
    Failed,
    Busy,
    LaunchedConstrained,
    LaunchedLegacy,
    StagedOnly,
  };

  using SyncCompletion = std::function<void(bool success)>;

  // True when noctalia-greeter and the privileged apply helper are installed.
  [[nodiscard]] bool appearanceSyncAvailable(const ShellGreeterSyncConfig& greeterSync) noexcept;

  // Negotiates the installed helper protocol before staging. Current helpers receive
  // an appearance-only payload through pkexec and --sync; older helpers retain the
  // administrator-authenticated positional mode and its session payload. Returns Busy
  // without touching staging while another sync runs. Legacy mode may return StagedOnly
  // when no login session can host a Polkit prompt. onComplete runs (on the main loop's thread)
  // only after a successfully launched helper exits.
  [[nodiscard]] GreeterSyncLaunch syncAppearanceToGreeterAsync(
      const ConfigService& config, std::string_view resolvedThemeMode, SyncCompletion onComplete = {},
      const CompositorPlatform* platform = nullptr, bool logindOnSystemBus = false
  );

  // The lock clock's gradient ink (packed 0xRRGGBBAA, LockScreen::setClockGradient); synced as [lockscreen] clock_ink_*.
  void setLockClockGradient(std::uint32_t from, std::uint32_t to);

  // Runs on the main loop when a staged wallpaper's lock look (blur, dim) finished baking; the caller syncs again.
  void setLockWallpaperBakeReady(std::function<void()> callback);

  // No-op when appearanceSyncAvailable() is false (handler is not registered).
  void registerIpc(
      IpcService& ipc, const ConfigService& config, std::function<std::string_view()> resolvedThemeMode,
      const CompositorPlatform* platform, std::function<bool()> logindOnSystemBus = {}
  );

} // namespace greeter

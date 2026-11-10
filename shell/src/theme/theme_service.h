#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "theme/palette.h"
#include "ui/palette.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ConfigService;
class HttpClient;
class IpcService;

namespace noctalia::theme {

  class ThemeService {
  public:
    using ChangeCallback = std::function<void()>;
    // Carries the resolved [theme].mode: the app-facing mode templates and the GTK color
    // scheme run in. Shell-facing consumers read resolvedShellMode()/isLightMode().
    using ResolvedCallback = std::function<void(const GeneratedPalette&, std::string_view)>;

    ThemeService(ConfigService& config, HttpClient& httpClient);
    ~ThemeService();

    ThemeService(const ThemeService&) = delete;
    ThemeService& operator=(const ThemeService&) = delete;

    // Snaps the palette to the resolved theme (no fade). Used at startup.
    void apply();

    // Resolves the target theme and cross-fades to it.
    void onConfigReload();
    void onAutoSchemeChanged();
    void setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude);
    void toggleLightDark();
    void cycleMode();
    [[nodiscard]] ThemeMode configuredMode() const noexcept;
    // Noctalia's own resolved mode ([theme].shell_mode, or [theme].mode when it follows).
    [[nodiscard]] bool isLightMode() const noexcept;
    [[nodiscard]] std::string_view resolvedShellMode() const noexcept;
    // The resolved [theme].mode, which drives apps.
    [[nodiscard]] std::string_view resolvedMode() const noexcept;

    // The compositor's live appearance: accent "#rrggbb" (empty keeps the token accent) and increased contrast.
    void setLiveAppearance(std::string accent, bool increaseContrast);

    // Accent colors for GTK/Qt/KDE files in the resolved app mode: the target palette, not a mid-fade value.
    struct ToolkitColors {
      ::Color accent;
      ::Color accentText;
      ::Color onAccent;
    };
    [[nodiscard]] const ToolkitColors& toolkitColors() const noexcept { return m_toolkitColors; }

    void setChangeCallback(ChangeCallback callback);
    void setResolvedCallback(ResolvedCallback callback);

    void registerIpc(IpcService& ipc);


  private:
    ToolkitColors m_toolkitColors{};
    std::string m_liveAccent;
    bool m_liveHighContrast = false;

    void resolveAndSet(bool animate);
    void queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode);
    void flushResolvedCallback(bool defer);
    void startTransition(const Palette& target);
    void finishTransition(bool deferResolvedCallback);
    void tickTransition();
    void rescheduleAutoTimer();
    [[nodiscard]] bool hasAutoMode() const noexcept;

    ConfigService& m_config;
    HttpClient& m_httpClient;


    ChangeCallback m_changeCallback;
    ResolvedCallback m_resolvedCallback;
    // External template/hooks callbacks are delayed until the shell palette is
    // applied, and deferred callbacks must be able to drop stale resolves.
    std::optional<GeneratedPalette> m_pendingResolvedPalette;
    std::string m_pendingResolvedMode;
    std::uint64_t m_resolvedCallbackGeneration = 0;

    AnimationManager m_animations;
    Timer m_transitionTimer;
    Palette m_fromPalette{};
    Palette m_targetPalette{};
    AnimationManager::Id m_transitionAnimId = 0;
    bool m_transitionResolvedCallbackFlushed = false;
    bool m_isShellLightMode = false;
    bool m_isLightMode = false;
    std::optional<double> m_autoLatitude;
    std::optional<double> m_autoLongitude;
    Timer m_autoTimer;
  };

} // namespace noctalia::theme

#include "theme/theme_service.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "system/day_night_schedule.h"
#include "theme/builtin_palettes.h"
#include "theme/fixed_palette.h"
#include "theme/palette_transform.h"
#include "theme/shell_palette.h"
#include "ui/app_icon_colorization.h"
#include "util/string_utils.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr auto kLog = Logger("theme");

    constexpr float kTransitionDurationMs = 400.0F;
    // One 60 Hz frame: a faster tick repaints every surface more often than the screen can show it.
    constexpr std::chrono::milliseconds kTransitionTick{16};

    struct ResolvedTheme {
      GeneratedPalette generated;
      Palette palette;
      // Resolved [theme].mode (apps) and the mode Noctalia itself runs in.
      std::string mode;
      std::string shellMode;
    };

    ResolvedTheme resolveTokens(std::string_view mode, std::string_view shellMode) {
      return {
          .generated = expandBuiltinPalette(andromedaPalette()),
          .palette = tokenPalette(shellMode == "light"),
          .mode = std::string(mode),
          .shellMode = std::string(shellMode),
      };
    }

  } // namespace

  ThemeService::ThemeService(ConfigService& config, HttpClient& httpClient)
      : m_config(config), m_httpClient(httpClient) {}

  ThemeService::~ThemeService() = default;

  namespace {

    // Applies the compositor accent; high contrast makes secondary text read as primary text and strokes opaque.
    void applyLivePalette(::Palette& palette, std::string_view accent, bool highContrast) {
      ::Color color;
      if (!accent.empty() && tryParseHexColor(accent, color)) {
        palette.accent = color;
        palette.onAccent = onAccentOn(readableTextColorForBackground(color), color);
        palette.accentText = accentTextOn(color, palette.solidRegular);
      }
      if (highContrast) {
        palette.label2 = palette.label1;
        palette.stroke.a = 1.0F;
      }
    }

  } // namespace

  void ThemeService::setLiveAppearance(std::string accent, bool increaseContrast) {
    if (accent == m_liveAccent && increaseContrast == m_liveHighContrast) {
      return;
    }
    m_liveAccent = std::move(accent);
    m_liveHighContrast = increaseContrast;
    resolveAndSet(/*animate=*/true);
  }

  void ThemeService::apply() { resolveAndSet(/*animate=*/false); }

  void ThemeService::onConfigReload() { resolveAndSet(/*animate=*/true); }

  void ThemeService::onAutoSchemeChanged() {
    if (hasAutoMode()) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude) {
    if (latitude.has_value() && !std::isfinite(*latitude)) {
      latitude.reset();
    }
    if (longitude.has_value() && !std::isfinite(*longitude)) {
      longitude.reset();
    }
    if (m_autoLatitude == latitude && m_autoLongitude == longitude) {
      return;
    }
    m_autoLatitude = latitude;
    m_autoLongitude = longitude;
    if (hasAutoMode()) {
      resolveAndSet(/*animate=*/true);
    }
  }

  void ThemeService::toggleLightDark() {
    const auto next = m_isLightMode ? ThemeMode::Dark : ThemeMode::Light;
    // Persist via ConfigService → StateService. The resulting overrides-change
    // callback rebuilds the Config and fires the reload callbacks, which call
    // ThemeService::onConfigReload() to transition to the new palette.
    m_config.setThemeMode(next);
  }

  void ThemeService::cycleMode() {
    ThemeMode next = ThemeMode::Dark;
    switch (configuredMode()) {
    case ThemeMode::Dark:
      next = ThemeMode::Light;
      break;
    case ThemeMode::Light:
      next = ThemeMode::Auto;
      break;
    case ThemeMode::Auto:
      next = ThemeMode::Dark;
      break;
    }
    m_config.setThemeMode(next);
  }

  ThemeMode ThemeService::configuredMode() const noexcept { return m_config.config().theme.mode; }

  bool ThemeService::isLightMode() const noexcept { return m_isShellLightMode; }

  std::string_view ThemeService::resolvedShellMode() const noexcept { return m_isShellLightMode ? "light" : "dark"; }

  std::string_view ThemeService::resolvedMode() const noexcept { return m_isLightMode ? "light" : "dark"; }

  void ThemeService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  void ThemeService::setResolvedCallback(ResolvedCallback callback) { m_resolvedCallback = std::move(callback); }

  void ThemeService::queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode) {
    if (!m_resolvedCallback) {
      return;
    }
    m_pendingResolvedPalette = generated;
    m_pendingResolvedMode = std::string(mode);
    ++m_resolvedCallbackGeneration;
  }

  void ThemeService::flushResolvedCallback(bool defer) {
    if (!m_resolvedCallback || !m_pendingResolvedPalette.has_value()) {
      return;
    }

    GeneratedPalette generated = std::move(*m_pendingResolvedPalette);
    std::string mode = std::move(m_pendingResolvedMode);
    const std::uint64_t generation = m_resolvedCallbackGeneration;
    m_pendingResolvedPalette.reset();
    m_pendingResolvedMode.clear();

    if (defer) {
      DeferredCall::callLater([this, generation, generated = std::move(generated), mode = std::move(mode)] mutable {
        if (generation == m_resolvedCallbackGeneration && m_resolvedCallback) {
          m_resolvedCallback(generated, mode);
        }
      });
      return;
    }

    m_resolvedCallback(generated, mode);
  }

  void ThemeService::resolveAndSet(bool animate) {
    profiling::ScopedTimer t(kLog, "theme: resolveAndSet");
    const auto& cfg = m_config.config().theme;
    // [theme].mode is the app-facing mode; the shell either follows it or runs its own.
    // Both resolve `auto` against one schedule evaluation.
    std::optional<bool> scheduleNight;
    const auto modeName = [&](ThemeMode themeMode) -> std::string {
      if (themeMode != ThemeMode::Auto) {
        return themeMode == ThemeMode::Light ? "light" : "dark";
      }
      if (!scheduleNight.has_value()) {
        scheduleNight = day_night_schedule::evaluate(m_config.config().location, m_autoLatitude, m_autoLongitude).night;
      }
      return *scheduleNight ? "dark" : "light";
    };
    const std::string mode = modeName(cfg.mode);
    const std::string shellMode = modeName(shellThemeMode(cfg));

    std::optional<ResolvedTheme> resolved = resolveTokens(mode, shellMode);
    if (cfg.pureBlackDark) {
      applyPureBlackDark(resolved->generated);
    }
    const bool highContrast = m_config.config().accessibility.highContrast || m_liveHighContrast;
    if (highContrast) {
      applyHighContrast(resolved->generated);
    }
    const auto paletteFor = [&](bool light) {
      ::Palette p = tokenPalette(light);
      applyLivePalette(p, m_liveAccent, highContrast);
      return p;
    };
    resolved->palette = paletteFor(resolved->shellMode == "light");
    const ::Palette appPalette = resolved->mode == resolved->shellMode ? resolved->palette : paletteFor(resolved->mode == "light");
    m_toolkitColors = {.accent = appPalette.accent, .accentText = appPalette.accentText, .onAccent = appPalette.onAccent};

    queueResolvedCallback(resolved->generated, resolved->mode);
    m_isLightMode = resolved->mode == "light";
    m_isShellLightMode = resolved->shellMode == "light";

    if (animate) {
      setResolvedThemeLight(m_isShellLightMode);
      notifyShellAppIconColorizationChanged();
      startTransition(resolved->palette);
    } else {
      if (m_transitionAnimId == 0 && palette == resolved->palette) {
        flushResolvedCallback(/*defer=*/false);
        rescheduleAutoTimer();
        return;
      }
      if (m_transitionAnimId != 0) {
        m_animations.cancel(m_transitionAnimId);
        m_transitionAnimId = 0;
      }
      m_transitionTimer.stop();
      setResolvedThemeLight(m_isShellLightMode);
      notifyShellAppIconColorizationChanged();
      setPalette(resolved->palette);
      if (m_changeCallback) {
        m_changeCallback();
      }
      flushResolvedCallback(/*defer=*/false);
    }
    rescheduleAutoTimer();
  }

  void ThemeService::rescheduleAutoTimer() {
    m_autoTimer.stop();
    if (!hasAutoMode()) {
      return;
    }
    constexpr auto kAutoRecheckInterval = std::chrono::minutes(1);
    const auto nextBoundary =
        day_night_schedule::evaluate(m_config.config().location, m_autoLatitude, m_autoLongitude).untilBoundary;
    const auto delay =
        std::min(nextBoundary, std::chrono::duration_cast<std::chrono::milliseconds>(kAutoRecheckInterval));
    m_autoTimer.start(delay, [this] { onAutoSchemeChanged(); });
  }

  bool ThemeService::hasAutoMode() const noexcept {
    const auto& theme = m_config.config().theme;
    return theme.mode == ThemeMode::Auto || shellThemeMode(theme) == ThemeMode::Auto;
  }

  void ThemeService::startTransition(const Palette& target) {
    if (m_transitionAnimId == 0 && palette == target) {
      flushResolvedCallback(/*defer=*/false);
      return;
    }
    if (m_transitionAnimId != 0 && m_targetPalette == target) {
      flushResolvedCallback(/*defer=*/true);
      return;
    }
    // Capture the currently-displayed palette (possibly mid-fade) so a new
    // transition starts from wherever the previous one had reached.
    if (m_transitionAnimId != 0) {
      m_animations.cancel(m_transitionAnimId);
      m_transitionAnimId = 0;
    }
    m_fromPalette = palette;
    m_targetPalette = target;
    m_transitionResolvedCallbackFlushed = false;
    m_transitionAnimId = m_animations.animateSpring(
        0.0F, 1.0F, Style::spring(Tokens::springFade),
        [this](float t) {
          setPalette(lerpPalette(m_fromPalette, m_targetPalette, t));
          if (m_changeCallback) {
            m_changeCallback();
          }
          if (!m_transitionResolvedCallbackFlushed) {
            m_transitionResolvedCallbackFlushed = true;
            flushResolvedCallback(/*defer=*/true);
          }
        },
        [this] { finishTransition(/*deferResolvedCallback=*/false); }
    );
    if (m_transitionAnimId == 0) {
      m_transitionTimer.stop();
      return;
    }
    m_transitionTimer.startRepeating(kTransitionTick, [this] { tickTransition(); });
  }

  void ThemeService::finishTransition(bool deferResolvedCallback) {
    m_transitionAnimId = 0;
    m_transitionTimer.stop();
    setPalette(m_targetPalette);
    if (m_changeCallback) {
      m_changeCallback();
    }
    flushResolvedCallback(deferResolvedCallback);
  }

  void ThemeService::tickTransition() {
    if (!m_animations.hasActive()) {
      m_transitionTimer.stop();
      return;
    }
    m_animations.tick(static_cast<float>(kTransitionTick.count()));
  }

  void ThemeService::registerIpc(IpcService& ipc) {
    ipc.bind(noctalia::cli::msg::themeModeToggle, [this](const std::string&) -> std::string {
      toggleLightDark();
      return "ok\n";
    });
    ipc.bind(
        noctalia::cli::msg::themeModeGet,
        [this](const std::string&) -> std::string {
          std::string out(resolvedMode());
          out.push_back('\n');
          return out;
        },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    ipc.bind(noctalia::cli::msg::themeModeSet, [this](const std::string& args) -> std::string {
      const std::string token = StringUtils::trim(args);
      const auto mode = enumFromKey(kThemeModes, token);
      if (!mode.has_value()) {
        return "error: expected dark, light, or auto\n";
      }
      m_config.setThemeMode(*mode);
      return "ok\n";
    });
  }

} // namespace noctalia::theme

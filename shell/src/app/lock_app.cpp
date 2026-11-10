#include "app/lock_app.h"

#include "app/application.h"
#include "app/application_internal.h"
#include "app/deferred_call_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "compositors/compositor_platform.h"
#include "compositors/keyboard_layout_poll_source.h"
#include "compositors/shell_wayland_connection.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "dbus/andromeda/lock_notice_client.h"
#include "dbus/andromeda/lock_service.h"
#include "dbus/link/link_lock_client.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/network_display.h"
#include "dbus/network/network_lock_client.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/pixel_stream_service.h"
#include "net/http_client.h"
#include "render/animation/motion_service.h"
#include "render/core/shared_texture_cache.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_registry.h"
#include "scripting/script_api_context.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_surface.h"
#include "shell/lockscreen/lockscreen_widgets_controller.h"
#include "shell/lockscreen/lockscreen_widgets_host.h"
#include "shell/session/session_action_runner.h"
#include "system/lock_keys_poll_source.h"
#include "system/lock_keys_service.h"
#include "system/weather_service.h"
#include "theme/live_appearance_sync.h"
#include "theme/theme_service.h"
#include "ui/auth/auth_column.h"
#include "ui/controls/input.h"
#include "ui/style.h"
#include "wayland/key_repeat_poll_source.h"

#include <cmath>
#include <csignal>
#include <format>
#include <malloc.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

  constexpr Logger kLog("lock-mode");

  void onSignal(int signum) {
    if (signum == SIGTERM || signum == SIGINT) {
      Application::s_shutdownRequested = true;
    }
  }

  class LockApp {
  public:
    int run() {
      std::signal(SIGTERM, onSignal);
      std::signal(SIGINT, onSignal);
      initLogLevelFromEnvironment();
      kLog.info("lock process starting");

      applyStyle();
      i18n::Service::instance().init(m_config.config().shell.lang);
      m_theme.apply();
      m_config.addReloadCallback([this] {
        applyStyle();
        i18n::Service::instance().setLanguage(m_config.config().shell.lang);
        m_theme.onConfigReload();
        m_renderContext.setTextFontFamily(m_config.config().shell.fontFamily);
        m_renderContext.setTextScale(m_config.config().accessibility.textScale);
        if (m_logind != nullptr) {
          m_logind->setSessionLockIntegrationEnabled(m_config.isLockScreenEnabled());
          m_logind->setLockBeforeSuspendEnabled(m_config.shouldLockBeforeSuspend());
        }
        m_lock.onConfigChanged();
        if (m_config.lastChange().plugins && m_widgetsShown) {
          loadPlugins();
          m_widgetsHost.reloadPluginWidgets(m_lock);
        }
        syncWidgets();
      });

      if (!m_wayland.connect()) {
        kLog.error("cannot connect to the Wayland display");
        return 1;
      }
      m_platform.initialize();
      const bool sharedGl = m_config.config().shell.sharedGlContext;
      m_glShared.initialize(m_wayland.display(), sharedGl);
      m_textures.initialize(sharedGl ? &m_glShared : nullptr);
      m_renderContext.initialize(m_glShared);
      m_renderContext.setTextFontFamily(m_config.config().shell.fontFamily);
      m_renderContext.setTextScale(m_config.config().accessibility.textScale);
      Style::setRtl(i18n::Service::instance().rtl());
      m_renderContext.setTextBaseDirection(i18n::Service::instance().rtl());
      bindKeybinds();

      initBuses();
      m_lock.initialize(m_wayland, &m_renderContext, &m_config, &m_textures, m_systemBus.get(), &m_platform);
      m_lock.setLoginBoxServices(&m_sessionActions, m_mpris.get(), &m_weather, &m_http);
      m_lock.setPhoneServices(m_link.get());
      if (m_notices != nullptr) {
        m_lock.setNoticeProvider([this] { return m_notices->groups(); });
      }
      initWidgets();
      initHooks();
      initInput();
      initLiveAppearance();
      refreshStatus();
      initControl();

      MainLoop loop(m_wayland, [this] { return pollSources(); });
      loop.run();
      kLog.info("lock process stopping");
      return Application::s_displayLost ? 75 : 0;
    }

  private:
    void applyStyle() {
      const auto& shell = m_config.config().shell;
      MotionService::instance().setSpeed(shell.animation.speed);
      MotionService::instance().setEnabled(shell.animation.enabled);
      Style::setCornerRadiusScale(shell.cornerRadiusScale);
      Style::setInputBordersEnabled(shell.inputBorders);
      Style::setButtonBordersEnabled(shell.buttonBorders);
      Input::setPasswordMaskStyle(
          shell.passwordMaskStyle == PasswordMaskStyle::RandomIcons ? Input::PasswordMaskStyle::RandomIcons
                                                                    : Input::PasswordMaskStyle::CircleFilled
      );
      m_http.setOfflineMode(shell.offlineMode);
    }

    void bindKeybinds() {
      for (const KeybindAction action :
           {KeybindAction::Validate, KeybindAction::Cancel, KeybindAction::Left, KeybindAction::Right}) {
        KeybindMatcher::setMatcher(action, [this, action](std::uint32_t sym, std::uint32_t modifiers) {
          return m_config.matchesKeybind(action, sym, modifiers);
        });
      }
    }

    void initBuses() {
      try {
        m_systemBus = std::make_unique<SystemBus>();
        m_systemBusPoll = std::make_unique<SystemBusPollSource>(*m_systemBus);
        if (m_systemBus->nameHasOwner("org.freedesktop.login1")) {
          m_logind = std::make_unique<LogindService>(*m_systemBus);
        }
        m_upower = std::make_unique<UPowerService>(*m_systemBus);
        m_upower->setChangeCallback([this](const auto& /*change*/) { refreshStatus(); });
      } catch (const std::exception& e) {
        kLog.warn("system bus: {}", e.what());
      }
      if (m_systemBus != nullptr) {
        try {
          m_powerProfiles = std::make_unique<PowerProfilesService>(*m_systemBus);
          m_powerProfiles->setChangeCallback([this](const auto& /*state*/, auto /*origin*/) { syncStreamPause(); });
        } catch (const std::exception& e) {
          kLog.warn("power profiles: {}", e.what());
        }
        try {
          m_network = std::make_unique<NetworkLockClient>(*m_systemBus);
          m_network->setChangeCallback([this] { refreshStatus(); });
          m_network->setSignalGate([this] { return m_lock.isActive(); });
        } catch (const std::exception& e) {
          kLog.warn("network: {}", e.what());
        }
      }
      try {
        m_sessionBus = std::make_unique<SessionBus>();
        m_sessionBusPoll = std::make_unique<SessionBusPollSource>(*m_sessionBus);
        m_mpris = std::make_unique<MprisService>(*m_sessionBus, false);
      } catch (const std::exception& e) {
        kLog.warn("session bus: {}", e.what());
      }
      if (m_sessionBus != nullptr) {
        try {
          m_link = std::make_unique<LinkLockClient>(*m_sessionBus);
          m_link->setChangeCallback([this] {
            if (m_lock.isActive()) {
              m_lock.requestUpdate();
            }
          });
        } catch (const std::exception& e) {
          kLog.warn("link client: {}", e.what());
        }
        try {
          m_notices = std::make_unique<LockNoticeClient>(*m_sessionBus);
          m_notices->setChangeCallback([this] {
            if (m_lock.isActive()) {
              m_lock.requestUpdate();
            }
          });
        } catch (const std::exception& e) {
          kLog.warn("lock notices: {}", e.what());
        }
      }
    }

    void initWidgets() {
      m_widgetsHost.initialize(DesktopWidgetServices{
          .wayland = m_wayland,
          .config = &m_config,
          .renderContext = &m_renderContext,
          .runtime =
              {
                  .weather = &m_weather,
                  .mpris = m_mpris.get(),
                  .httpClient = &m_http,
                  .scriptDeps = {.scriptApi = &m_scriptApi, .configService = &m_config},
              },
          .textureCache = &m_textures,
      });
      syncStreamPause();
    }

    void loadPlugins() {
      scripting::applyPluginSourcesToRegistry(scripting::PluginRegistry::instance(), m_config.config().plugins);
    }

    // As LockscreenWidgetsController::applyVisibility: the host and its lock helper run only while the session is
    // locked (never on the surfaces of a pending lock), and with widgets off the host still plays a video or shader
    // wallpaper. Only show() resets the helper restart cap, so a visible host is rebuilt, never shown again.
    void syncWidgets() {
      if (!m_sessionLocked || !m_config.isLockScreenEnabled()) {
        m_widgetsTick.stop();
        m_widgetsHost.hide();
        m_widgetsShown = false;
        return;
      }
      m_scriptApi.setDarkMode(!m_theme.isLightMode());
      m_scriptApi.setTimeFormat(m_config.config().shell.timeFormat);
      m_scriptApi.setDateFormat(m_config.config().shell.dateFormat);
      auto snapshot = LockscreenWidgetsController::lockSnapshot(m_config.config().lockscreenWidgets, m_wayland);
      if (m_widgetsShown) {
        m_widgetsHost.rebuild(snapshot, m_lock);
      } else {
        loadPlugins();
        m_widgetsHost.show(snapshot, m_lock);
        m_widgetsShown = true;
      }
      scheduleWidgetsTick();
    }

    // The shell's TimeService tick: every second while a widget shows seconds, else on the minute.
    void scheduleWidgetsTick() {
      using namespace std::chrono;
      const milliseconds period = m_widgetsHost.wantsSecondTicks() ? milliseconds(seconds(1)) : minutes(1);
      const auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
      m_widgetsTick.start(period - now % period + milliseconds(5), [this] {
        m_widgetsHost.onSecondTick();
        scheduleWidgetsTick();
      });
    }

    void syncStreamPause() {
      const bool powerSaver = m_powerProfiles != nullptr && m_powerProfiles->activeProfile() == "power-saver";
      setLockClockPowerSaver(powerSaver);
      m_widgetsHost.setStreamsPaused(powerSaver, m_platform.outputPowerStates());
    }

    void initHooks() {
      m_lock.setSessionHooks(
          [this] {
            if (m_logind != nullptr) {
              m_logind->setSessionLockedHint(true);
            }
            m_sessionLocked = true;
            syncWidgets();
            emitState("locked");
          },
          [this] {
            m_sessionLocked = false;
            syncWidgets();
            m_lockBeforeSleep.onLockEnded();
            if (m_logind != nullptr) {
              m_logind->setSessionLockedHint(false);
            }
            if (m_control != nullptr) {
              if (const auto rejected = m_lock.takeRejectedWhileLocked(); !rejected.empty()) {
                m_control->failedAttempts(rejected);
              }
            }
            emitState("unlocked");
            releaseWhenIdle();
          },
          [this] {
            m_sessionLocked = false;
            syncWidgets();
            m_lockBeforeSleep.onLockEnded();
            emitState("aborted");
            releaseWhenIdle();
          },
          [this] { m_lockBeforeSleep.onLockSettled(); }
      );
      m_platform.setOutputPowerChangeCallback([this] {
        m_lock.onOutputPowerChanged();
        syncStreamPause();
      });

      if (m_logind == nullptr) {
        return;
      }
      m_logind->setSessionLockIntegrationEnabled(m_config.isLockScreenEnabled());
      m_logind->setLockBeforeSuspendEnabled(m_config.shouldLockBeforeSuspend());
      m_logind->setLockCallback([this] { (void)lock("logind"); });
      m_logind->setUnlockCallback([this] {
        if (m_lock.isActive()) {
          m_lock.unlock();
        }
      });
      m_logind->setPrepareForSleepCallback([this](bool sleeping) { onPrepareForSleep(sleeping); });
    }

    void onPrepareForSleep(bool sleeping) {
      if (!sleeping) {
        m_lockBeforeSleep.onPrepareForSleep(false);
        m_renderContext.invalidateGlyphTexturesNextFrame();
        if (m_lock.isActive()) {
          m_lock.forceRepaintAfterResume();
          m_lock.onSystemResumed();
        }
        m_weather.requestRefresh();
        return;
      }
      // A sleep-bound lock never offers the passwordless grace.
      m_lock.resetGracePeriod();
      m_lockBeforeSleep.onPrepareForSleep(true);
    }

    // The session hooks run before LockScreen destroys its surfaces, so the release waits for the next loop turn.
    void releaseWhenIdle() {
      DeferredCall::callLater([this] {
        if (m_lock.isActive()) {
          return;
        }
        m_renderContext.releaseUnusedCaches();
        trimAllocator();
      });
    }

    void initInput() {
      m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
        if (m_lock.isActive()) {
          m_lock.onPointerEvent(event);
        }
      });
      m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
        if (m_lock.isActive()) {
          m_lock.onKeyboardEvent(event);
        }
      });
      m_wayland.setLockKeysChangeCallback([this] {
        if (m_lock.isActive()) {
          m_lock.onLockKeysChanged();
        }
      });
      m_platform.setKeyboardLayoutChangeCallback([this] {
        if (m_lock.isActive()) {
          m_lock.onKeyboardLayoutChanged();
        }
      });
      m_wayland.setOutputChangeCallback([this] {
        m_lock.onOutputChange();
        syncWidgets();
      });
      // The previous lock process died while locked, or the compositor restarted into the lock: lock at once.
      m_wayland.setRelockCallback([this] { (void)lock("relock"); });
    }

    void initLiveAppearance() {
      m_liveAppearance.initialize(m_wayland, [this](const LiveAppearance& appearance) {
        Style::setGlass(appearance.glass, appearance.reduceTransparency, appearance.blurEnabled);
        MotionService::instance().setReduced(appearance.reduceMotion);
        m_theme.setLiveAppearance(appearance.accent, appearance.increaseContrast);
        m_lock.onThemeChanged();
      });
    }

    void refreshStatus() {
      AuthColumn::Status status;
      if (m_upower != nullptr && m_upower->state().isPresent) {
        const auto& battery = m_upower->state();
        status.batteryGlyph = batteryGlyphName(battery.percentage, battery.state);
        status.batteryText = std::format("{}%", static_cast<int>(std::lround(battery.percentage)));
        if (battery.state == BatteryState::Charging) {
          const std::int64_t minutes = battery.timeToFull / 60;
          status.chargingText = minutes <= 0
              ? i18n::tr("lockscreen.cards.charging", "percent", std::lround(battery.percentage))
              : minutes < 60
              ? i18n::tr("lockscreen.cards.charging-full-in", "time", std::format("{} min", minutes))
              : i18n::tr(
                    "lockscreen.cards.charging-full-in", "time", std::format("{} h {} min", minutes / 60, minutes % 60)
                );
        }
      }
      if (m_network != nullptr && m_network->hasState()) {
        status.networkGlyph = network_display::glyphForState(m_network->state());
        kLog.debug("network glyph {}", status.networkGlyph);
      }
      m_lock.setStatus(std::move(status));
    }

    bool lock(const std::string& reason) {
      const bool relock = reason == "relock";
      if (!relock && !m_config.isLockScreenEnabled()) {
        kLog.info("lock ({}) refused: the lock screen is off", reason);
        return false;
      }
      // A phone or sleep lock also reaches a held or queued lock: it revokes grace (phone: also face and fingerprint).
      const bool upgrade = reason == "phone" || reason == "sleep";
      if (!upgrade && m_lock.isLockHeldOrQueued()) {
        return true;
      }
      kLog.info("lock requested ({})", reason);
      if (m_network != nullptr) {
        m_network->refreshSignal();
      }
      if (reason == "phone") {
        return m_lock.lockFromPhone();
      }
      if (reason == "sleep") {
        return m_lock.lockForSleep();
      }
      return m_lock.lock(relock);
    }

    void initControl() {
      if (m_sessionBus == nullptr) {
        kLog.warn("no session bus: org.andromeda.Lock1 is not served");
        return;
      }
      try {
        m_control = std::make_unique<LockDBusService>(
            *m_sessionBus,
            LockDBusService::Handlers{
                .lock = [this](const std::string& reason) { return lock(reason); },
                .primeCapture = [this] { m_lock.primeDesktopCaptures(); },
                .clearCapture = [this] { m_lock.clearPrimedDesktopCaptures(); },
                .prepareOutputPowerOff =
                    [this](std::function<void(bool)> done) { m_lock.prepareForOutputPowerOff(std::move(done)); },
                .cancelOutputPowerOff = [this] { m_lock.cancelOutputPowerOff(); },
                .outputsWoke = [this] { m_lock.markOutputsWoke(); },
                .skipLockOnNextSleep = [this](bool skip) { m_lockBeforeSleep.setSkipNextLock(skip); },
                .active = [this] { return m_lock.isActive(); },
                .locked = [this] { return m_lock.isSessionLocked(); },
            }
        );
      } catch (const std::exception& e) {
        kLog.error("cannot serve org.andromeda.Lock1: {}", e.what());
      }
    }

    void emitState(const std::string& state) {
      if (m_control != nullptr) {
        m_control->stateChanged(state);
      }
    }

    std::vector<PollSource*> pollSources() {
      std::vector<PollSource*> sources;
      if (m_sessionBusPoll != nullptr) {
        sources.push_back(m_sessionBusPoll.get());
      }
      if (m_systemBusPoll != nullptr) {
        sources.push_back(m_systemBusPoll.get());
      }
      sources.push_back(&m_deferredPoll);
      sources.push_back(&m_configPoll);
      sources.push_back(&m_timerPoll);
      sources.push_back(&m_keyRepeatPoll);
      sources.push_back(&m_keyboardLayoutPoll);
      sources.push_back(&m_lockKeysPoll);
      sources.push_back(&m_pixelStreams);
      sources.push_back(&m_textures);
      return sources;
    }

    ShellWaylandConnection m_wayland;
    CompositorPlatform m_platform{m_wayland};
    ConfigService m_config;
    HttpClient m_http;
    noctalia::theme::ThemeService m_theme{m_config, m_http};
    LiveAppearanceSync m_liveAppearance;
    WeatherService m_weather{m_config, m_http};
    LockKeysService m_lockKeys{m_wayland};
    GlSharedContext m_glShared;
    SharedTextureCache m_textures;
    RenderContext m_renderContext;
    // Polls the lock helper's frame sockets; its clients live in the lock surfaces, so it outlives m_lock.
    PixelStreamService m_pixelStreams;

    // The lock's authenticators and players hold proxies on these buses, so the buses outlive m_lock.
    std::unique_ptr<SystemBus> m_systemBus;
    std::unique_ptr<SystemBusPollSource> m_systemBusPoll;
    std::unique_ptr<LogindService> m_logind;
    std::unique_ptr<UPowerService> m_upower;
    std::unique_ptr<PowerProfilesService> m_powerProfiles;
    std::unique_ptr<SessionBus> m_sessionBus;
    std::unique_ptr<SessionBusPollSource> m_sessionBusPoll;
    std::unique_ptr<MprisService> m_mpris;
    std::unique_ptr<LinkLockClient> m_link;
    std::unique_ptr<LockNoticeClient> m_notices;
    std::unique_ptr<NetworkLockClient> m_network;

    LockScreen m_lock;
    SessionActionRunner m_sessionActions{m_platform, m_lock};
    scripting::ScriptApiContext m_scriptApi;
    // Declared after m_lock: hide() on destruction detaches its nodes from the live lock surfaces.
    LockscreenWidgetsHost m_widgetsHost;
    Timer m_widgetsTick;
    std::unique_ptr<LockDBusService> m_control;

    DeferredCallPollSource m_deferredPoll;
    ConfigPollSource m_configPoll{m_config};
    TimerPollSource m_timerPoll;
    KeyRepeatPollSource m_keyRepeatPoll{m_wayland};
    KeyboardLayoutPollSource m_keyboardLayoutPoll{m_platform};
    LockKeysPollSource m_lockKeysPoll{m_lockKeys};

    bool m_sessionLocked = false;
    bool m_widgetsShown = false;
    LockBeforeSleep m_lockBeforeSleep{{
        .enabled = [this] { return m_config.shouldLockBeforeSuspend(); },
        .lockReadyForSleep = [this] { return m_lock.isReadyForSleep(); },
        .lockHeldOrQueued = [this] { return m_lock.isLockHeldOrQueued(); },
        .lockTearingDown = [this] { return m_lock.isTearingDown(); },
        .lock = [this] { return m_lock.lockForSleep(); },
        .prepareLockForSleep = [this] { m_lock.prepareForSleep(); },
        .acquireDelay = [this] { m_logind->acquireSleepDelayInhibit(); },
        .releaseDelay =
            [this] {
              if (m_logind != nullptr) {
                m_logind->releaseSleepDelayInhibit();
              }
            },
    }};
  };

} // namespace

int runLockMode(int /*argc*/, char* /*argv*/[]) {
#ifdef __GLIBC__
  // A fixed threshold keeps wallpaper decode buffers (8-33 MB) on their own mappings, unmapped when freed; glibc's
  // dynamic threshold would move them into arena heaps that stay dirty while the process waits unlocked.
  mallopt(M_MMAP_THRESHOLD, 4 << 20);
#endif
  try {
    LockApp app;
    return app.run();
  } catch (const std::exception& e) {
    kLog.error("lock process failed: {}", e.what());
    return 1;
  }
}

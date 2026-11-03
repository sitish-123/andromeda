#include "app/main_loop.h"
#include "application.h"
#include "application_internal.h"
#include "dbus/session_bus.h"
#include "dbus/system_bus.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <string_view>

ActivityMonitor* Application::ensureActivityMonitor() {
  if (m_activityMonitor == nullptr && m_bus != nullptr) {
    const char* root = std::getenv("NOCTALIA_CGROUP_ROOT");
    m_activityMonitor = std::make_unique<ActivityMonitor>(
        *m_bus, [this]() { return m_compositorPlatform.windowPids(); },
        root != nullptr && *root != '\0' ? root : "/sys/fs/cgroup"
    );
  }
  return m_activityMonitor.get();
}

void Application::initActivity() {
  if (ensureActivityMonitor() == nullptr) {
    return;
  }
  m_activityActions = std::make_unique<ActivityActions>(m_bus->connection(), m_systemBus != nullptr ? &m_systemBus->connection() : nullptr);
  m_backgroundWarningState = std::make_unique<BackgroundWarningState>(FileUtils::stateDir() + "/activity.json");
  m_activityWindow.initialize(m_wayland, &m_configService, &m_renderContext, m_activityMonitor.get(), m_activityActions.get());

  BackgroundActivityMonitor::Hooks hooks;
  hooks.trayPids = [this]() { return m_trayService != nullptr ? m_trayService->itemPids() : std::vector<int>{}; };
  hooks.enabledFeatures = [this]() {
    std::set<ShellFeature> features{ShellFeature::Notifications, ShellFeature::Clipboard, ShellFeature::NightLight};
    if (bluetooth() != nullptr) {
      features.insert(ShellFeature::Bluetooth);
    }
    if (network() != nullptr) {
      features.insert(ShellFeature::Network);
    }
    if (m_pipewireModule != nullptr) {
      features.insert(ShellFeature::Sound);
    }
    if (upower() != nullptr) {
      features.insert(ShellFeature::Battery);
    }
    if (m_linkService != nullptr) {
      features.insert(ShellFeature::Link);
    }
    if (m_polkitAgent != nullptr) {
      features.insert(ShellFeature::PolkitAgent);
    }
    if (m_udisksService != nullptr) {
      features.insert(ShellFeature::Drives);
    }
    const char* desktops = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktops == nullptr || !std::string_view(desktops).contains("KDE")) {
      features.insert(ShellFeature::Plasma);
    }
    return features;
  };
  hooks.enabled = [this]() { return m_configService.config().system.background.warn; };
  hooks.thresholds = [this]() {
    const auto& cfg = m_configService.config().system.background;
    BackgroundThresholds thresholds;
    thresholds.minUptime = std::chrono::minutes(cfg.minUptimeMinutes);
    thresholds.memoryBytes = static_cast<std::uint64_t>(cfg.memoryMb) * 1024 * 1024;
    return thresholds;
  };
  hooks.sampled = [this]() {
    m_activityDetails.snapshot(m_activityMonitor->activity().rows(), std::chrono::steady_clock::now());
  };
  hooks.showActivity = [this](const std::string& rowKey) { m_activityWindow.open(rowKey); };
  m_backgroundActivityMonitor = std::make_unique<BackgroundActivityMonitor>(
      *m_activityMonitor, *m_activityActions, *m_backgroundWarningState, m_notificationManager, std::move(hooks)
  );
  m_activityWindow.setBackgroundMonitor(m_backgroundActivityMonitor.get());
  m_activityWindow.setDetails(&m_activityDetails);
  m_activityWindow.setSystemMonitorProvider([this]() { return m_systemMonitor.get(); });
  m_activityWindow.setUPowerProvider([this]() { return upower(); });
  m_backgroundActivityMonitor->start();
}

#include "system/background_activity_monitor.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "system/activity_actions.h"
#include "system/activity_monitor.h"
#include "system/background_warning_state.h"
#include "system/cgroup_stats.h"
#include "system/desktop_entry.h"
#include "system/xdg_autostart.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <utility>

namespace {

  constexpr Logger kLog("background-activity");
  constexpr std::size_t kGroupAt = 3;

  std::string memoryText(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? std::format("{:.1f} GB", mb / 1024.0) : std::format("{:.0f} MB", mb);
  }

  std::string agoText(std::uint64_t seconds) {
    if (seconds >= 3600) {
      return std::format("{} h", seconds / 3600);
    }
    return std::format("{} min", std::max<std::uint64_t>(1, seconds / 60));
  }

  std::string featureText(ShellFeature feature) {
    switch (feature) {
    case ShellFeature::Bluetooth:
      return i18n::tr("notifications.internal.activity-feature-bluetooth");
    case ShellFeature::Network:
      return i18n::tr("notifications.internal.activity-feature-network");
    case ShellFeature::Sound:
      return i18n::tr("notifications.internal.activity-feature-sound");
    case ShellFeature::Battery:
      return i18n::tr("notifications.internal.activity-feature-battery");
    case ShellFeature::Notifications:
      return i18n::tr("notifications.internal.activity-feature-notifications");
    case ShellFeature::Link:
      return i18n::tr("notifications.internal.activity-feature-link");
    case ShellFeature::PolkitAgent:
      return i18n::tr("notifications.internal.activity-feature-polkit");
    case ShellFeature::Clipboard:
      return i18n::tr("notifications.internal.activity-feature-clipboard");
    case ShellFeature::NightLight:
      return i18n::tr("notifications.internal.activity-feature-night-light");
    case ShellFeature::Drives:
      return i18n::tr("notifications.internal.activity-feature-drives");
    case ShellFeature::Plasma:
    case ShellFeature::None:
      break;
    }
    return {};
  }

  std::uint64_t nowRealtimeUsec() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
    );
  }

  std::string autostartUserDir() { return autostartDirsFromEnv().user; }

  std::vector<AutostartEntry> autostartEntries() {
    const char* desktops = std::getenv("XDG_CURRENT_DESKTOP");
    return scanAutostart(autostartDirsFromEnv(), desktops != nullptr ? desktops : "");
  }

} // namespace

BackgroundActivityMonitor::BackgroundActivityMonitor(
    ActivityMonitor& activity, ActivityActions& actions, BackgroundWarningState& state, NotificationManager& notifications,
    Hooks hooks
)
    : m_activity(activity), m_actions(actions), m_state(state), m_notifications(notifications), m_hooks(std::move(hooks)) {
  m_notifications.addInternalActionCallback(
      [this](std::uint32_t id, const std::string& action, const std::string& /*token*/, const std::string& /*sender*/) {
        onAction(id, action);
      }
  );
}

void BackgroundActivityMonitor::start() {
  if (m_started) {
    return;
  }
  m_started = true;
  m_timer.start(std::chrono::duration_cast<std::chrono::milliseconds>(kFirstCheck), [this]() {
    checkNow();
    m_timer.startRepeating(std::chrono::duration_cast<std::chrono::milliseconds>(kInterval), [this]() { checkNow(); });
  });
}

void BackgroundActivityMonitor::checkNow(std::function<void(std::size_t)> done) {
  if (m_hooks.enabled && !m_hooks.enabled()) {
    if (done) {
      done(0);
    }
    return;
  }
  m_activity.sampleOnce([this, done = std::move(done)]() {
    auto findings = evaluate();
    const std::size_t count = findings.size();
    if (!findings.empty()) {
      notify(std::move(findings));
    }
    if (done) {
      done(count);
    }
  });
}

std::vector<BackgroundActivityMonitor::Finding> BackgroundActivityMonitor::evaluate() const {
  const auto& service = m_activity.activity();
  const auto& units = m_activity.userUnits().units();
  BackgroundContext context;
  context.nowUsec = nowRealtimeUsec();
  context.cpuCount = service.cpuCount();
  if (const char* home = std::getenv("HOME"); home != nullptr) {
    context.homeDir = home;
  }
  if (m_hooks.trayPids) {
    for (const int pid : m_hooks.trayPids()) {
      if (std::string unit = unitForPid(pid); !unit.empty()) {
        context.trayUnits.insert(std::move(unit));
      }
    }
  }
  if (m_hooks.enabledFeatures) {
    context.enabledFeatures = m_hooks.enabledFeatures();
  }
  context.hasTerminal = [&service, &units](const ActivityRow& row) {
    return std::ranges::any_of(row.units, [&](const std::string& name) {
      const auto it = std::ranges::find(units, name, &SystemdUnit::name);
      return it != units.end() && cgroupHasTerminal(service.cgroupRoot(), it->controlGroup);
    });
  };

  const auto autostart = autostartEntries();
  const auto& entries = desktopEntries();
  std::vector<Finding> findings;
  for (const auto& row : service.rows()) {
    if (m_state.ignored(row.key) || row.units.empty()) {
      continue;
    }
    const auto unit = std::ranges::find(units, row.units.front(), &SystemdUnit::name);
    if (unit == units.end()) {
      continue;
    }
    const BackgroundVerdict verdict = classifyBackground(row, unit->fragmentPath, unit->unitFilePreset, context);
    if (verdict.finding == BackgroundVerdict::Finding::None || !verdict.notify) {
      continue;
    }
    Finding finding;
    finding.rowKey = row.key;
    finding.unitKey = unit->fragmentPath.empty() ? unit->name : unit->fragmentPath;
    finding.mtime = unit->fragmentPath.empty() ? 0 : fileMtime(unit->fragmentPath);
    if (m_state.alreadyNotified(finding.unitKey, finding.mtime)) {
      continue;
    }
    const auto autostartEntry = std::ranges::find(autostart, unit->name, &AutostartEntry::unitName);
    const auto desktop = std::ranges::find_if(entries, [&row](const DesktopEntry& e) {
      return !row.appId.empty() && desktopEntryAppId(e.id) == row.appId;
    });
    if (desktop != entries.end() && !desktop->name.empty()) {
      finding.name = desktop->name;
    } else if (autostartEntry != autostart.end() && !autostartEntry->name.empty()) {
      finding.name = autostartEntry->name;
    } else if (!unit->description.empty()) {
      finding.name = unit->description;
    } else {
      finding.name = row.key;
    }
    finding.verdict = verdict;
    finding.memoryBytes = row.memoryBytes;
    finding.atLogin = unit->unitFileState == "enabled" || autostartEntry != autostart.end();
    for (const auto& name : row.units) {
      if (const auto u = std::ranges::find(units, name, &SystemdUnit::name); u != units.end()) {
        finding.units.push_back(*u);
      }
    }
    findings.push_back(std::move(finding));
  }
  return findings;
}

void BackgroundActivityMonitor::notify(std::vector<Finding> findings) {
  const auto post = [this](std::string summary, std::string body, std::vector<std::string> actions) {
    NotificationRequest request;
    request.appName = i18n::tr("notifications.internal.activity");
    request.summary = std::move(summary);
    request.body = std::move(body);
    request.origin = NotificationOrigin::Internal;
    request.persistInHistory = true;
    request.icon = std::string("noctalia-glyph:activity");
    request.actions = std::move(actions);
    return m_notifications.addOrReplace(std::move(request));
  };

  for (const auto& finding : findings) {
    m_state.markNotified(finding.unitKey, finding.mtime);
    kLog.info("background finding: {} ({}, {})", finding.rowKey, finding.verdict.finding == BackgroundVerdict::Finding::Duplicate ? "duplicate" : "uses resources", finding.unitKey);
  }
  if (findings.size() >= kGroupAt) {
    std::string names;
    std::uint64_t memory = 0;
    for (std::size_t i = 0; i < findings.size(); ++i) {
      memory += findings[i].memoryBytes;
      if (i < kGroupAt) {
        names += (i == 0 ? "" : i + 1 == std::min(findings.size(), kGroupAt) ? " and " : ", ") + findings[i].name;
      }
    }
    const std::uint32_t id = post(
        i18n::tr("notifications.internal.activity-grouped-title", "count", std::to_string(findings.size())),
        i18n::tr("notifications.internal.activity-grouped-body", "names", names, "memory", memoryText(memory)),
        {"default", i18n::tr("notifications.internal.activity-show"), "show", i18n::tr("notifications.internal.activity-show"),
         "ignore-all", i18n::tr("notifications.internal.activity-ignore-all")}
    );
    if (id != 0) {
      m_posted[id] = std::move(findings);
    }
    return;
  }
  for (auto& finding : findings) {
    std::string body;
    std::vector<std::string> actions;
    if (finding.verdict.finding == BackgroundVerdict::Finding::Duplicate) {
      body = finding.verdict.duplicates == ShellFeature::Plasma
                 ? i18n::tr("notifications.internal.activity-duplicate-plasma-body")
                 : i18n::tr("notifications.internal.activity-duplicate-body", "feature", featureText(finding.verdict.duplicates));
      actions = {"stop-disable", i18n::tr("notifications.internal.activity-stop-dont-start"), "keep",
                 i18n::tr("notifications.internal.activity-keep")};
    } else {
      body = i18n::tr(
          finding.atLogin ? "notifications.internal.activity-background-body-login" : "notifications.internal.activity-background-body",
          "memory", memoryText(finding.memoryBytes), "ago", agoText(finding.verdict.uptimeSec)
      );
      actions = {"stop", i18n::tr("notifications.internal.activity-stop")};
      if (finding.atLogin) {
        actions.insert(actions.end(), {"disable", i18n::tr("notifications.internal.activity-dont-start")});
      }
      actions.insert(actions.end(), {"show", i18n::tr("notifications.internal.activity-show")});
    }
    const std::uint32_t id = post(
        i18n::tr("notifications.internal.activity-background-title", "name", finding.name), std::move(body), std::move(actions)
    );
    if (id != 0) {
      m_posted[id] = {std::move(finding)};
    }
  }
}

void BackgroundActivityMonitor::onAction(std::uint32_t id, const std::string& action) {
  const auto it = m_posted.find(id);
  if (it == m_posted.end()) {
    return;
  }
  const std::vector<Finding> findings = std::move(it->second);
  m_posted.erase(it);
  const auto logResult = [](const std::string& what, const std::string& unit) {
    return [what, unit](const ActivityActionResult& result) {
      if (!result.ok) {
        kLog.warn("{} {} failed: {}", what, unit, result.error);
      }
    };
  };
  for (const auto& finding : findings) {
    if (action == "show" || action == "default") {
      if (m_hooks.showActivity) {
        m_hooks.showActivity(finding.rowKey);
      }
      return; // one window for the whole group
    }
    if (action == "keep" || action == "ignore-all") {
      m_state.setIgnored(finding.rowKey, true);
      continue;
    }
    for (const auto& unit : finding.units) {
      if (action == "disable" || action == "stop-disable") {
        m_actions.disableAtLogin(unit, autostartEntries(), autostartUserDir(), logResult("disable", unit.name));
      }
      if (action == "stop" || action == "stop-disable") {
        m_actions.stop(unit, logResult("stop", unit.name));
      }
    }
  }
}

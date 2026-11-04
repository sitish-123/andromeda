#include "system/background_activity.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fnmatch.h>
#include <string>
#include <utility>

namespace {

  constexpr std::array kNeverFlag{
      "andromeda*",  "noctalia*",   "pipewire*",      "wireplumber*", "*xdg-*portal*", "*keyring*", "gcr-ssh-agent*",
      "dbus.service", "dbus-broker.service", "at-spi*",     "dconf*",         "gvfs*",        "ssh-agent*",    "gpg-agent*", "systemd-*",
      "greetd*",     "polkit*",     "NetworkManager*", "iwd*",        "bluetooth*",    "upower*",   "power-profiles-daemon*",
      "init.scope",  "session-*.scope", "user@*.service", "xdg-*",
  };

  // Matched against the app ID or the unit name without suffix and instance, lower case.
  constexpr std::array<std::pair<const char*, ShellFeature>, 25> kDuplicates{{
      {"blueman", ShellFeature::Bluetooth},
      {"blueman-applet", ShellFeature::Bluetooth},
      {"nm-applet", ShellFeature::Network},
      {"nm-tray", ShellFeature::Network},
      {"pasystray", ShellFeature::Sound},
      {"xfce4-power-manager", ShellFeature::Battery},
      {"cbatticon", ShellFeature::Battery},
      {"dunst", ShellFeature::Notifications},
      {"mako", ShellFeature::Notifications},
      {"swaync", ShellFeature::Notifications},
      {"xfce4-notifyd", ShellFeature::Notifications},
      {"kdeconnectd", ShellFeature::Link},
      {"org.kde.kdeconnect.daemon", ShellFeature::Link},
      {"polkit-gnome-authentication-agent-1", ShellFeature::PolkitAgent},
      {"lxpolkit", ShellFeature::PolkitAgent},
      {"parcellite", ShellFeature::Clipboard},
      {"copyq", ShellFeature::Clipboard},
      {"com.github.hluk.copyq", ShellFeature::Clipboard},
      {"clipman", ShellFeature::Clipboard},
      {"redshift", ShellFeature::NightLight},
      {"gammastep", ShellFeature::NightLight},
      {"udiskie", ShellFeature::Drives},
      {"org.kde.kded6", ShellFeature::Plasma},
      {"plasma-kactivitymanagerd", ShellFeature::Plasma},
      {"kactivitymanagerd", ShellFeature::Plasma},
  }};

  std::string matchKey(std::string_view text) {
    for (const char* suffix : {".service", ".scope", ".desktop"}) {
      if (text.ends_with(suffix)) {
        text.remove_suffix(std::string_view(suffix).size());
      }
    }
    if (const std::size_t at = text.find('@'); at != std::string_view::npos) {
      text = text.substr(0, at);
    }
    std::string key(text);
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
  }

} // namespace

ShellFeature duplicateFeatureFor(std::string_view appIdOrUnit) {
  const std::string key = matchKey(appIdOrUnit);
  const auto it = std::ranges::find_if(kDuplicates, [&key](const auto& entry) { return key == entry.first; });
  return it == kDuplicates.end() ? ShellFeature::None : it->second;
}

bool neverFlag(std::string_view unitName) {
  const std::string name(unitName);
  return std::ranges::any_of(kNeverFlag, [&name](const char* pattern) { return ::fnmatch(pattern, name.c_str(), 0) == 0; });
}

BackgroundVerdict classifyBackground(
    const ActivityRow& row, std::string_view fragmentPath, std::string_view unitFilePreset, const BackgroundContext& context
) {
  BackgroundVerdict verdict;
  if (row.startedUsec != 0 && context.nowUsec > row.startedUsec) {
    verdict.uptimeSec = (context.nowUsec - row.startedUsec) / 1'000'000;
    verdict.cpuAveragePercent = static_cast<double>(row.cpuUsageUsec)
                                / (static_cast<double>(context.nowUsec - row.startedUsec) * std::max(1, context.cpuCount)) * 100.0;
  }
  const bool inTray = std::ranges::any_of(row.units, [&context](const std::string& u) { return context.trayUnits.contains(u); });
  if (row.hasWindow || inTray || row.slice == UnitSlice::Session) {
    return verdict;
  }
  if (std::ranges::any_of(row.units, [](const std::string& u) { return neverFlag(u); })) {
    return verdict;
  }

  const ShellFeature duplicate = duplicateFeatureFor(row.appId.empty() ? row.key : row.appId);
  const bool isDuplicate = duplicate != ShellFeature::None && context.enabledFeatures.contains(duplicate);
  const auto& t = context.thresholds;
  const bool costly = verdict.uptimeSec >= static_cast<std::uint64_t>(std::chrono::seconds(t.minUptime).count())
                      && (row.memoryBytes >= t.memoryBytes || verdict.cpuAveragePercent >= t.cpuAveragePercent);
  if (!isDuplicate && !costly) {
    verdict.background = true;
    return verdict;
  }
  // A terminal session (a shell in a scope, `tmux`) is attended even without a window.
  if (context.hasTerminal && context.hasTerminal(row)) {
    return verdict;
  }
  verdict.background = true;
  verdict.finding = isDuplicate ? BackgroundVerdict::Finding::Duplicate : BackgroundVerdict::Finding::UsesResources;
  verdict.duplicates = isDuplicate ? duplicate : ShellFeature::None;

  // XDG autostart entries are the user's choice; their generated units live in generator.late as app-*@autostart.service.
  const bool userInstalled = (!context.homeDir.empty() && fragmentPath.starts_with(context.homeDir + "/.config/"))
                             || fragmentPath.starts_with("/etc/") || (fragmentPath.contains("/generator.late/app-") && fragmentPath.ends_with("@autostart.service"))
                             || unitFilePreset == "disabled";
  // Units from the vendor's /usr that the vendor enables list in Activity but never notify.
  verdict.notify = userInstalled;
  return verdict;
}

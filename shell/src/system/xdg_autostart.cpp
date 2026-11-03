#include "system/xdg_autostart.h"

#include "dbus/systemd/systemd_unit_name.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  std::map<std::string, std::string> readDesktopEntryGroup(const fs::path& path) {
    std::map<std::string, std::string> keys;
    std::ifstream in(path);
    std::string line;
    bool inGroup = false;
    while (std::getline(in, line)) {
      if (line.empty() || line.front() == '#') {
        continue;
      }
      if (line.front() == '[') {
        inGroup = line == "[Desktop Entry]";
        continue;
      }
      const std::size_t eq = line.find('=');
      if (!inGroup || eq == std::string::npos) {
        continue;
      }
      std::string key = line.substr(0, eq);
      while (!key.empty() && key.back() == ' ') {
        key.pop_back();
      }
      std::size_t valueStart = eq + 1;
      while (valueStart < line.size() && line[valueStart] == ' ') {
        ++valueStart;
      }
      keys.try_emplace(std::move(key), line.substr(valueStart));
    }
    return keys;
  }

  bool isTrue(const std::map<std::string, std::string>& keys, const std::string& key) {
    const auto it = keys.find(key);
    return it != keys.end() && it->second == "true";
  }

  std::set<std::string, std::less<>> splitList(std::string_view text, char separator) {
    std::set<std::string, std::less<>> items;
    while (!text.empty()) {
      const std::size_t end = text.find(separator);
      const std::string_view item = text.substr(0, end);
      if (!item.empty()) {
        items.emplace(item);
      }
      if (end == std::string_view::npos) {
        break;
      }
      text.remove_prefix(end + 1);
    }
    return items;
  }

  bool executableExists(const std::string& program) {
    if (program.contains('/')) {
      return ::access(program.c_str(), X_OK) == 0;
    }
    const char* path = std::getenv("PATH");
    return std::ranges::any_of(splitList(path != nullptr ? path : "/usr/local/bin:/usr/bin", ':'), [&](const auto& dir) {
      return ::access(std::format("{}/{}", dir, program).c_str(), X_OK) == 0;
    });
  }

  bool shownIn(const std::map<std::string, std::string>& keys, std::string_view currentDesktops) {
    const auto desktops = splitList(currentDesktops, ':');
    const auto intersects = [&desktops](const std::string& list) {
      return std::ranges::any_of(splitList(list, ';'), [&desktops](const auto& d) { return desktops.contains(d); });
    };
    if (const auto it = keys.find("OnlyShowIn"); it != keys.end() && !intersects(it->second)) {
      return false;
    }
    if (const auto it = keys.find("NotShowIn"); it != keys.end() && intersects(it->second)) {
      return false;
    }
    return true;
  }

} // namespace

AutostartDirs autostartDirsFromEnv() {
  AutostartDirs dirs;
  const char* configHome = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  if (configHome != nullptr && *configHome != '\0') {
    dirs.user = std::string(configHome) + "/autostart";
  } else if (home != nullptr) {
    dirs.user = std::string(home) + "/.config/autostart";
  }
  const char* configDirs = std::getenv("XDG_CONFIG_DIRS");
  std::string_view list = configDirs != nullptr && *configDirs != '\0' ? configDirs : "/etc/xdg";
  while (!list.empty()) {
    const std::size_t end = list.find(':');
    if (const std::string_view dir = list.substr(0, end); !dir.empty()) {
      dirs.system.push_back(std::string(dir) + "/autostart");
    }
    if (end == std::string_view::npos) {
      break;
    }
    list.remove_prefix(end + 1);
  }
  return dirs;
}

std::vector<AutostartEntry> scanAutostart(const AutostartDirs& dirs, std::string_view currentDesktops) {
  std::vector<AutostartEntry> entries;
  std::set<std::string, std::less<>> seen;
  std::vector<std::pair<std::string, bool>> order;
  if (!dirs.user.empty()) {
    order.emplace_back(dirs.user, true);
  }
  for (const auto& dir : dirs.system) {
    order.emplace_back(dir, false);
  }
  for (const auto& [dir, user] : order) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& file : fs::directory_iterator(dir, ec)) {
      if (file.path().extension() == ".desktop") {
        files.push_back(file.path());
      }
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
      std::string id = file.stem().string();
      if (!seen.insert(id).second) {
        continue;
      }
      const auto keys = readDesktopEntryGroup(file);
      const auto value = [&keys](const std::string& key) {
        const auto it = keys.find(key);
        return it == keys.end() ? std::string{} : it->second;
      };
      AutostartEntry entry;
      entry.path = file.string();
      entry.name = value("Name");
      entry.exec = value("Exec");
      entry.icon = value("Icon");
      entry.unitName = "app-" + escapeUnitName(id) + "@autostart.service";
      entry.userFile = user;
      const std::string tryExec = value("TryExec");
      const auto autostartEnabled = keys.find("X-GNOME-Autostart-enabled");
      entry.startsHere = !isTrue(keys, "Hidden")
                         && !isTrue(keys, "X-systemd-skip")
                         && (autostartEnabled == keys.end() || autostartEnabled->second != "false")
                         && (tryExec.empty() || executableExists(tryExec))
                         && shownIn(keys, currentDesktops);
      entry.id = std::move(id);
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

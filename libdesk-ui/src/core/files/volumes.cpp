#include "core/files/volumes.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/statvfs.h>

namespace fs = std::filesystem;

namespace {
  // /proc/self/mounts writes space, tab, newline and backslash as octal escapes ("\040").
  std::string unescape(const std::string& field) {
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
      if (field[i] == '\\' && i + 3 < field.size() && std::isdigit(static_cast<unsigned char>(field[i + 1])) != 0) {
        out.push_back(static_cast<char>(std::stoi(field.substr(i + 1, 3), nullptr, 8)));
        i += 3;
      } else {
        out.push_back(field[i]);
      }
    }
    return out;
  }

  bool under(const std::string& path, std::string_view prefix) {
    return path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/';
  }

  bool browsable(const std::string& mount) {
    return mount == "/" || mount == "/home" || under(mount, "/run/media") || under(mount, "/media") ||
           under(mount, "/mnt");
  }
} // namespace

std::vector<Volume> listVolumes() {
  std::ifstream in("/proc/self/mounts");
  std::map<std::string, std::string> byDevice; // one row per device: btrfs subvolumes mount it many times
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string device;
    std::string mount;
    if (!(fields >> device >> mount) || !device.starts_with("/dev/") || device.starts_with("/dev/loop")) {
      continue;
    }
    mount = unescape(mount);
    if (!browsable(mount)) {
      continue;
    }
    auto [it, added] = byDevice.try_emplace(device, mount);
    if (!added && mount.size() < it->second.size()) {
      it->second = mount; // "/" wins over "/home" on the same drive
    }
  }

  std::vector<Volume> out;
  for (const auto& [device, mount] : byDevice) {
    struct statvfs st {};
    if (::statvfs(mount.c_str(), &st) != 0 || st.f_blocks == 0) {
      continue;
    }
    const bool removable = under(mount, "/run/media") || under(mount, "/media");
    out.push_back({
        .name = mount == "/" ? std::string("This PC") : fs::path(mount).filename().string(),
        .mountPoint = mount,
        .totalBytes = static_cast<std::uint64_t>(st.f_blocks) * st.f_frsize,
        .freeBytes = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize,
        .removable = removable,
    });
  }
  std::ranges::stable_sort(out, [](const Volume& a, const Volume& b) {
    if (a.mountPoint == "/" || b.mountPoint == "/") {
      return a.mountPoint == "/" && b.mountPoint != "/";
    }
    return a.removable < b.removable;
  });
  return out;
}

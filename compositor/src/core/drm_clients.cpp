#include "core/drm_clients.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

namespace andromeda {

  namespace {
    bool isDrm(const std::filesystem::path& fd) {
      std::error_code ec;
      const std::filesystem::path target = std::filesystem::read_symlink(fd, ec);
      return !ec && target.native().starts_with("/dev/dri/");
    }
  } // namespace

  std::vector<DrmClient> drmClients(const std::vector<pid_t>& pids) {
    std::vector<DrmClient> out;
    std::set<std::string> seen;
    for (const pid_t pid : pids) {
      const std::string proc = "/proc/" + std::to_string(pid);
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(proc + "/fd", ec)) {
        if (!isDrm(entry.path())) {
          continue;
        }
        std::ifstream info(proc + "/fdinfo/" + entry.path().filename().native());
        std::string pdev;
        std::string id;
        std::uint64_t busy = 0;
        for (std::string line; std::getline(info, line);) {
          std::istringstream fields(line);
          std::string name;
          std::string value;
          std::string unit;
          fields >> name >> value >> unit;
          if (name == "drm-pdev:") {
            pdev = value;
          } else if (name == "drm-client-id:") {
            id = value;
          } else if (name.starts_with("drm-engine-") && !name.starts_with("drm-engine-capacity-") && unit == "ns") {
            busy += std::strtoull(value.c_str(), nullptr, 10);
          }
        }
        if (id.empty() || !seen.insert(pdev + "/" + id).second) {
          continue;
        }
        out.push_back({.key = pdev + "/" + id, .busyNs = busy, .pid = pid});
      }
    }
    return out;
  }

  std::vector<pid_t> cgroupPids(const std::string& dir) {
    std::vector<pid_t> pids;
    const auto read = [&pids](const std::filesystem::path& cgroup) {
      std::ifstream in(cgroup / "cgroup.procs");
      for (pid_t pid = 0; in >> pid;) {
        pids.push_back(pid);
      }
    };
    read(dir);
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_directory(ec)) {
        read(it->path());
      }
    }
    return pids;
  }

  std::string userManagerCgroup() {
    std::ifstream in("/proc/self/cgroup");
    std::string line;
    std::getline(in, line);
    // cgroup v2: "0::/user.slice/user-1000.slice/user@1000.service/session.slice/andromeda.service"
    const std::size_t at = line.find("/user@");
    const std::size_t end = at == std::string::npos ? at : line.find(".service", at);
    const std::size_t path = line.find("::");
    if (end == std::string::npos || path == std::string::npos) {
      return {};
    }
    return "/sys/fs/cgroup" + line.substr(path + 2, end + 8 - (path + 2));
  }

} // namespace andromeda

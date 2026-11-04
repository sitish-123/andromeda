#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace andromeda {

  // One open DRM client (docs/gpu/drm-usage-stats): `key` = "<drm-pdev>/<drm-client-id>", `busyNs` = its
  // drm-engine-* times summed.
  struct DrmClient {
    std::string key;
    std::uint64_t busyNs = 0;
    pid_t pid = 0;
  };

  // The DRM clients the processes `pids` hold, each client once (a client shared by dup or fork counts for the first
  // pid that holds it). Reads /proc/<pid>/fd and fdinfo only: about 1 ms for 800 fds.
  std::vector<DrmClient> drmClients(const std::vector<pid_t>& pids);

  // The processes of a cgroup directory and of every cgroup below it.
  std::vector<pid_t> cgroupPids(const std::string& dir);

  // The user's systemd manager cgroup (".../user@<uid>.service") the compositor runs under; empty when it runs
  // outside one.
  std::string userManagerCgroup();

} // namespace andromeda

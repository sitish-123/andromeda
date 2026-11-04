#pragma once

#include "core/shared_worker.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A Docker-API engine on a unix socket: Docker or Podman, rootful or rootless.
struct ContainerEngine {
  std::string socket;
  std::string name;  // from GET /version: "Docker Engine", "Podman Engine"
  std::string error; // set when the socket refuses us (permission) or the reply is not the Docker API
  bool rootless = false; // the socket belongs to this user: its units run in the user manager
  bool idle = false;     // only the socket unit listens; listing would start the daemon, so it was not asked
  std::vector<std::string> units; // the units the engine packages ship for this socket, service first
};

struct ContainerInfo {
  std::string id;
  std::string name;
  std::string image;
  std::string state;  // running, paused, exited, created, restarting, removing, dead
  std::string status; // the engine's own text, e.g. "Up 3 hours"
  std::string engineSocket;
  std::string cgroup; // relative to the cgroup2 root; empty when nothing runs
  std::int64_t createdSec = 0;
  double cpuPercent = 0.0; // share of all CPUs, like the Apps column
  std::uint64_t memoryBytes = 0;
};

// Lists containers of every engine socket found and runs Stop / Pause / Unpause. All socket and cgroup I/O runs on
// its own worker thread: an engine stop can take 10 s and must not hold up the shared worker's /proc scans.
class ContainerService {
public:
  enum class Action : std::uint8_t { Stop, Pause, Unpause };

  // Unix sockets of $DOCKER_HOST and $CONTAINER_HOST, then the rootless and rootful Docker and Podman defaults.
  // Only paths that exist, each socket once.
  [[nodiscard]] static std::vector<std::string> defaultSockets();

  ContainerService(std::string cgroupRoot, int cpuCount, std::vector<std::string> sockets = defaultSockets());
  ~ContainerService();

  ContainerService(const ContainerService&) = delete;
  ContainerService& operator=(const ContainerService&) = delete;

  // What the socket path and owner tell without any I/O: kind, name, user or system manager, unit names.
  [[nodiscard]] static ContainerEngine engineFor(const std::string& socket);

  // One list per engine except the idle sockets, then done() on the main loop. A call while one runs waits and
  // runs once after it, with the last arguments.
  void refresh(std::set<std::string> idleSockets, std::function<void()> done);
  // done(error) on the main loop; error is empty on success.
  void act(const ContainerInfo& container, Action action, std::function<void(std::string)> done);

  [[nodiscard]] bool available() const noexcept { return !m_sockets.empty(); }
  [[nodiscard]] const std::vector<ContainerEngine>& engines() const noexcept { return m_engines; }
  [[nodiscard]] const std::vector<ContainerInfo>& containers() const noexcept { return m_containers; }
  [[nodiscard]] const ContainerInfo* find(const std::string& id) const;
  [[nodiscard]] int runningCount() const;
  // Engines and containers as JSON, for `msg activity-dump`.
  [[nodiscard]] std::string dumpJson() const;

private:
  struct CpuSample {
    std::uint64_t usageUsec = 0;
    std::chrono::steady_clock::time_point at;
  };
  struct Snapshot {
    std::vector<ContainerEngine> engines;
    std::vector<ContainerInfo> containers;
    std::map<std::string, CpuSample> cpu;
    std::map<std::string, std::string> cgroups; // id + start time -> cgroup, so a container is inspected once per start
  };

  // Lets the destructor wake a request blocked on a slow engine, so shutdown does not wait for it.
  struct Io {
    std::atomic<bool> stopping = false;
    std::mutex mutex;
    std::set<int> fds;
  };
  struct Queued {
    std::set<std::string> idle;
    std::function<void()> done;
  };

  std::string m_cgroupRoot;
  int m_cpuCount;
  std::vector<std::string> m_sockets;
  std::vector<ContainerEngine> m_engines;
  std::vector<ContainerInfo> m_containers;
  std::map<std::string, CpuSample> m_cpu;
  std::map<std::string, std::string> m_cgroups;
  bool m_refreshing = false;
  std::optional<Queued> m_queued;
  std::shared_ptr<Io> m_io = std::make_shared<Io>();
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  SharedWorker m_worker; // last member: it joins its thread before the state above goes away
};

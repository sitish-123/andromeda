#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct ActivityRow;
struct SystemdUnit;

// PIDs in a unit's cgroup and its child cgroups.
[[nodiscard]] std::vector<int> cgroupPids(const std::string& cgroupRoot, const std::string& controlGroup);

// utime + stime of one process in clock ticks (/proc/<pid>/stat), 0 when it has exited.
[[nodiscard]] std::uint64_t readProcessCpuTicks(int pid);

struct ProcessGroupUsage {
  std::string command; // /proc/<pid>/comm
  int count = 0;
  std::uint64_t residentBytes = 0;
  double cpuPercent = 0.0; // share of all CPUs since the previous update
};

// 0..4 bars from CPU and GPU shares (percent of the whole machine): the Energy column and the "High energy" chip.
[[nodiscard]] int energyBars(double cpuPercent, double gpuPercent);

// Per-row extras that only the open Activity window needs: GPU share, history, process groups of expanded rows.
// update() runs once per ActivityMonitor sample; snapshot() runs from the 30 min background check, window or not.
// The /proc and cgroup reads of a sample run on the SharedWorker; GPU shares and groups apply when they finish.
class ActivityDetails {
public:
  static constexpr std::size_t kShortSamples = 600; // 10 min at the 1 s sample interval
  static constexpr std::size_t kLongSamples = 48;   // 24 h at the 30 min check interval
  static constexpr auto kMinInterval = std::chrono::milliseconds(900);
  static constexpr auto kSnapshotInterval = std::chrono::minutes(25); // a manual check between two must not add a point

  struct RowHistory {
    std::deque<float> memoryMb; // short: one per sample while the window is open
    std::deque<float> cpuPercent;
    std::deque<float> longMemoryMb; // long: one per background check
  };

  // Returns false and changes nothing when called again within kMinInterval (a unit-list refresh, not a sample).
  // `probed` runs on the main loop when the worker's GPU shares and groups for this sample apply.
  bool update(
      const std::vector<ActivityRow>& rows, const std::vector<SystemdUnit>& units, const std::string& cgroupRoot,
      int cpuCount, std::chrono::steady_clock::time_point now, std::function<void()> probed
  );
  void snapshot(const std::vector<ActivityRow>& rows, std::chrono::steady_clock::time_point now);
  // Clears the short history and GPU baselines (window closed: the next open starts fresh).
  void resetShort();

  void setExpanded(const std::string& rowKey, bool expanded);
  [[nodiscard]] bool expanded(const std::string& rowKey) const { return m_expanded.contains(rowKey); }

  [[nodiscard]] double gpuPercent(const std::string& rowKey) const;
  [[nodiscard]] const RowHistory* history(const std::string& rowKey) const;
  [[nodiscard]] const std::vector<ProcessGroupUsage>* groups(const std::string& rowKey) const;
  [[nodiscard]] const std::map<std::string, RowHistory>& histories() const noexcept { return m_history; }

  struct DrmFds {
    std::uint64_t startTime = 0; // /proc/<pid>/stat field 22: a reused pid gets a new scan
    std::vector<int> fds;        // fds that link to /dev/dri/*
    std::chrono::steady_clock::time_point scannedAt;
  };
  // State that only the worker job reads and writes; it moves into the job and back with the result.
  struct ProbeState {
    std::chrono::steady_clock::time_point at; // the previous probe; rates divide by now - at
    // Busy ns by engine per DRM client ("<drm-pdev>/<drm-client-id>"): a client counts from the probe that first saw
    // it, so one found late does not add its whole lifetime to one sample.
    std::map<std::string, std::map<std::string, std::uint64_t>> clientBusyNs;
    std::map<int, std::uint64_t> pidTicks; // CPU ticks of expanded rows' processes at the previous probe
    std::map<int, DrmFds> drmFds;
  };
  struct ProbeRow {
    std::string key;
    std::vector<std::string> controlGroups;
    bool expanded = false;
  };
  struct ProbeResult {
    ProbeState state;
    std::map<std::string, double> gpuPercent;
    std::map<std::string, std::vector<ProcessGroupUsage>> groups;
  };
  // The worker job: touches no ActivityDetails member.
  [[nodiscard]] static ProbeResult probe(
      ProbeState state, const std::vector<ProbeRow>& rows, const std::string& cgroupRoot, int cpuCount,
      std::chrono::steady_clock::time_point now
  );

private:
  std::map<std::string, RowHistory> m_history;
  std::map<std::string, double> m_gpuPercent;
  std::set<std::string> m_expanded;
  std::map<std::string, std::vector<ProcessGroupUsage>> m_groups;
  ProbeState m_probeState;
  bool m_probing = false;
  std::uint64_t m_generation = 0; // resetShort() bumps it: a job from before the reset is dropped
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  std::chrono::steady_clock::time_point m_lastUpdate;
  std::chrono::steady_clock::time_point m_lastSnapshot;
};

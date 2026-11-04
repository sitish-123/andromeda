#include "system/activity_details.h"

#include "core/shared_worker.h"
#include "dbus/systemd/systemd_units_service.h"
#include "system/activity_service.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  std::uint64_t toU64(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
      text.remove_prefix(1);
    }
    std::uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
  }

  fs::path cgroupDir(const std::string& cgroupRoot, const std::string& controlGroup) {
    return fs::path(cgroupRoot) / controlGroup.substr(!controlGroup.empty() && controlGroup.front() == '/' ? 1 : 0);
  }

  template <typename F> void forEachLine(std::string_view text, F&& f) {
    while (!text.empty()) {
      const std::size_t end = text.find('\n');
      f(text.substr(0, end));
      if (end == std::string_view::npos) {
        break;
      }
      text.remove_prefix(end + 1);
    }
  }

  std::string comm(int pid) {
    std::string command = readFile(std::format("/proc/{}/comm", pid));
    if (!command.empty() && command.back() == '\n') {
      command.pop_back();
    }
    return command;
  }

  std::uint64_t residentBytes(int pid) {
    static const auto pageSize = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    const std::string statm = readFile(std::format("/proc/{}/statm", pid));
    const std::size_t space = statm.find(' ');
    return space == std::string::npos ? 0 : toU64(std::string_view(statm).substr(space + 1)) * pageSize;
  }

  template <typename T> void push(std::deque<T>& ring, T value, std::size_t cap) {
    ring.push_back(value);
    while (ring.size() > cap) {
      ring.pop_front();
    }
  }

} // namespace

std::vector<int>
cgroupPids(const std::string& cgroupRoot, const std::string& controlGroup, std::vector<std::uint64_t>* ids) {
  std::vector<int> pids;
  if (controlGroup.empty()) {
    return pids;
  }
  const fs::path dir = cgroupDir(cgroupRoot, controlGroup);
  std::vector<fs::path> cgroups{dir};
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_directory(ec)) {
      cgroups.push_back(it->path());
    }
  }
  for (const auto& cgroup : cgroups) {
    if (struct stat st{}; ids != nullptr && ::stat(cgroup.c_str(), &st) == 0) {
      ids->push_back(st.st_ino);
    }
    forEachLine(readFile(cgroup / "cgroup.procs"), [&](std::string_view line) {
      if (const auto pid = static_cast<int>(toU64(line)); pid > 0) {
        pids.push_back(pid);
      }
    });
  }
  return pids;
}

namespace {

  constexpr auto kDrmRescan = std::chrono::seconds(10); // a running process can open the GPU late

  std::uint64_t startTime(int pid) {
    const std::string stat = readFile(std::format("/proc/{}/stat", pid));
    // comm may contain spaces: fields restart after the last ')' with field 3.
    const std::size_t close = stat.rfind(')');
    if (close == std::string::npos || close + 2 > stat.size()) {
      return 0;
    }
    std::string_view rest = std::string_view(stat).substr(close + 2);
    for (int field = 3; field < 22; ++field) {
      const std::size_t space = rest.find(' ');
      if (space == std::string_view::npos) {
        return 0;
      }
      rest.remove_prefix(space + 1);
    }
    return toU64(rest.substr(0, rest.find(' ')));
  }

  // DRM engine busy ns by engine per client ("<drm-pdev>/<drm-client-id>") of the processes, from the drm-engine-*
  // lines in /proc/<pid>/fdinfo (DRM client-usage stats). A client open on several fds or processes appears once.
  // Reads fdinfo of the cached DRM fds only: a full fdinfo scan of every process costs about 100 ms per sample.
  std::map<std::string, std::map<std::string, std::uint64_t>> readGpuClients(
      const std::vector<int>& pids, std::map<int, ActivityDetails::DrmFds>& drmFds,
      std::chrono::steady_clock::time_point now
  ) {
    std::map<std::string, std::map<std::string, std::uint64_t>> clients;
    for (const int pid : pids) {
      auto& cached = drmFds[pid];
      const std::uint64_t started = startTime(pid);
      if (cached.startTime != started
          || cached.scannedAt == std::chrono::steady_clock::time_point{}
          || now - cached.scannedAt >= kDrmRescan) {
        cached = {.startTime = started, .fds = {}, .scannedAt = now};
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(std::format("/proc/{}/fd", pid), ec)) {
          std::error_code linkEc;
          if (fs::read_symlink(entry.path(), linkEc).native().starts_with("/dev/dri/")) {
            cached.fds.push_back(static_cast<int>(toU64(entry.path().filename().native())));
          }
        }
      }
      for (const int fd : cached.fds) {
        const std::string info = readFile(std::format("/proc/{}/fdinfo/{}", pid, fd));
        std::string client;
        std::string device;
        std::map<std::string, std::uint64_t> busy;
        forEachLine(info, [&](std::string_view line) {
          if (line.starts_with("drm-client-id:")) {
            client = std::string(line.substr(14));
          } else if (line.starts_with("drm-pdev:")) {
            device = std::string(line.substr(9));
          } else if (line.starts_with("drm-engine-") && line.ends_with(" ns")) {
            if (const std::size_t colon = line.find(':'); colon != std::string_view::npos) {
              busy[std::string(line.substr(11, colon - 11))] += toU64(line.substr(colon + 1));
            }
          }
        });
        if (busy.empty() || client.empty()) {
          continue;
        }
        // Client ids are unique per device only.
        clients.try_emplace(device + "/" + client, std::move(busy));
      }
    }
    return clients;
  }

} // namespace

std::optional<std::map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>>> readTcpBytesByCgroup() {
  std::map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>> result;
  const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
  if (fd < 0) {
    return std::nullopt;
  }
  bool complete = true;
  for (const std::uint8_t family : {std::uint8_t{AF_INET}, std::uint8_t{AF_INET6}}) {
    struct {
      nlmsghdr header;
      inet_diag_req_v2 request;
    } message{};
    message.header.nlmsg_len = sizeof(message);
    message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    message.request.sdiag_family = family;
    message.request.sdiag_protocol = IPPROTO_TCP;
    message.request.idiag_states = ~0U;
    message.request.idiag_ext = 1U << (INET_DIAG_INFO - 1);
    if (::send(fd, &message, sizeof(message), 0) < 0) {
      complete = false;
      break;
    }
    alignas(nlmsghdr) char buffer[32768];
    for (bool done = false; !done && complete;) {
      const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        complete = false;
        break;
      }
      auto left = static_cast<unsigned int>(received);
      for (auto* header = reinterpret_cast<nlmsghdr*>(buffer); NLMSG_OK(header, left);
           header = NLMSG_NEXT(header, left)) {
        if (header->nlmsg_type == NLMSG_DONE || header->nlmsg_type == NLMSG_ERROR) {
          complete = header->nlmsg_type == NLMSG_DONE;
          done = true;
          break;
        }
        const auto* diag = static_cast<const inet_diag_msg*>(NLMSG_DATA(header));
        auto attrLeft = static_cast<int>(header->nlmsg_len - NLMSG_LENGTH(sizeof(*diag)));
        std::uint64_t cgroup = 0;
        std::uint64_t bytes = 0;
        for (auto* attr = reinterpret_cast<const rtattr*>(diag + 1); RTA_OK(attr, attrLeft);
             attr = RTA_NEXT(attr, attrLeft)) {
          if (attr->rta_type == INET_DIAG_CGROUP_ID && RTA_PAYLOAD(attr) >= sizeof(std::uint64_t)) {
            std::memcpy(&cgroup, RTA_DATA(attr), sizeof(cgroup));
          } else if (attr->rta_type == INET_DIAG_INFO) {
            tcp_info info{};
            std::memcpy(&info, RTA_DATA(attr), std::min<std::size_t>(RTA_PAYLOAD(attr), sizeof(info)));
            bytes = info.tcpi_bytes_received + info.tcpi_bytes_acked;
          }
        }
        if (cgroup != 0 && diag->idiag_inode != 0) {
          result[cgroup][diag->idiag_inode] = bytes;
        }
      }
    }
  }
  ::close(fd);
  if (!complete) {
    return std::nullopt;
  }
  return result;
}

std::uint64_t readProcessCpuTicks(int pid) {
  const std::string stat = readFile(std::format("/proc/{}/stat", pid));
  // comm may contain spaces: fields restart after the last ')'. utime and stime are fields 14 and 15.
  const std::size_t close = stat.rfind(')');
  if (close == std::string::npos) {
    return 0;
  }
  std::string_view rest = std::string_view(stat).substr(close + 2);
  std::uint64_t ticks = 0;
  for (int field = 3; field <= 15 && !rest.empty(); ++field) {
    const std::size_t space = rest.find(' ');
    if (field >= 14) {
      ticks += toU64(rest.substr(0, space));
    }
    if (space == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(space + 1);
  }
  return ticks;
}

int energyBars(double cpuPercent, double gpuPercent) {
  // GPU work costs about twice the power of the same CPU share on laptop parts; 4 bars from 20 % weighted.
  const double load = cpuPercent + 2.0 * gpuPercent;
  if (load >= 20.0) {
    return 4;
  }
  if (load >= 8.0) {
    return 3;
  }
  if (load >= 2.0) {
    return 2;
  }
  return load >= 0.3 ? 1 : 0;
}

bool ActivityDetails::update(
    const std::vector<ActivityRow>& rows, const std::vector<SystemdUnit>& units, const std::string& cgroupRoot,
    int cpuCount, std::chrono::steady_clock::time_point now, std::function<void()> probed
) {
  if (m_lastUpdate != std::chrono::steady_clock::time_point{} && now - m_lastUpdate < kMinInterval) {
    return false;
  }
  m_lastUpdate = now;
  for (const auto& row : rows) {
    auto& history = m_history[row.key];
    push(history.memoryMb, static_cast<float>(static_cast<double>(row.memoryBytes) / (1024.0 * 1024.0)), kShortSamples);
    push(history.cpuPercent, static_cast<float>(row.cpuPercent), kShortSamples);
  }
  if (m_probing) {
    return true; // the previous job still runs: this sample gets no GPU shares, the next one does
  }
  std::vector<ProbeRow> probeRows;
  probeRows.reserve(rows.size());
  for (const auto& row : rows) {
    ProbeRow& probeRow =
        probeRows.emplace_back(ProbeRow{.key = row.key, .controlGroups = {}, .expanded = m_expanded.contains(row.key)});
    for (const auto& name : row.units) {
      if (const auto it = std::ranges::find(units, name, &SystemdUnit::name);
          it != units.end() && !it->controlGroup.empty()) {
        probeRow.controlGroups.push_back(it->controlGroup);
      }
    }
  }
  m_probing = true;
  SharedWorker::instance().post<ProbeResult>(
      m_alive,
      [state = std::move(m_probeState), probeRows = std::move(probeRows), cgroupRoot, cpuCount, now]() mutable {
        return probe(std::move(state), probeRows, cgroupRoot, cpuCount, now);
      },
      [this, generation = m_generation, probed = std::move(probed)](ProbeResult result) {
        if (generation != m_generation) {
          return; // resetShort() ran meanwhile; it already cleared m_probing
        }
        m_probing = false;
        m_probeState = std::move(result.state);
        m_gpuPercent = std::move(result.gpuPercent);
        m_networkBytesPerSec = std::move(result.networkBytesPerSec);
        m_groups = std::move(result.groups);
        if (probed) {
          probed();
        }
      }
  );
  m_probeState = {};
  return true;
}

ActivityDetails::ProbeResult ActivityDetails::probe(
    ProbeState state, const std::vector<ProbeRow>& rows, const std::string& cgroupRoot, int cpuCount,
    std::chrono::steady_clock::time_point now
) {
  static const auto ticksPerSecond = static_cast<double>(::sysconf(_SC_CLK_TCK));
  const double seconds =
      state.at == std::chrono::steady_clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - state.at).count();
  ProbeResult result;
  result.state.at = now;
  std::set<int> seenPids;
  const auto tcp = readTcpBytesByCgroup();
  for (const auto& row : rows) {
    std::vector<int> pids;
    std::vector<std::uint64_t> cgroupIds;
    for (const auto& cg : row.controlGroups) {
      auto unitPids = cgroupPids(cgroupRoot, cg, &cgroupIds);
      pids.insert(pids.end(), unitPids.begin(), unitPids.end());
    }
    seenPids.insert(pids.begin(), pids.end());
    if (tcp.has_value() && seconds > 0.0) {
      std::uint64_t netDelta = 0;
      for (const std::uint64_t id : cgroupIds) {
        const auto sockets = tcp->find(id);
        if (sockets == tcp->end()) {
          continue;
        }
        for (const auto& [inode, bytes] : sockets->second) {
          const auto before = state.socketBytes.find(inode);
          if (before != state.socketBytes.end()) {
            netDelta += bytes >= before->second ? bytes - before->second : 0;
          } else if (state.socketsComplete) {
            netDelta += bytes; // not in the previous full dump: it opened since then
          }
        }
      }
      result.networkBytesPerSec[row.key] = static_cast<double>(netDelta) / seconds;
    }
    std::map<std::string, std::uint64_t> deltaNs; // by engine, summed over the row's clients
    for (auto& [client, busy] : readGpuClients(pids, state.drmFds, now)) {
      if (const auto before = state.clientBusyNs.find(client); before != state.clientBusyNs.end()) {
        for (const auto& [engine, ns] : busy) {
          const auto prev = before->second.find(engine);
          if (prev != before->second.end() && ns >= prev->second) {
            deltaNs[engine] += ns - prev->second;
          }
        }
      }
      result.state.clientBusyNs.try_emplace(client, std::move(busy));
    }
    if (seconds > 0.0) {
      // The busiest engine is the share: render and video engines run in parallel, so a sum can pass 100 %.
      double busiest = 0.0;
      for (const auto& [engine, ns] : deltaNs) {
        busiest = std::max(busiest, static_cast<double>(ns) / (seconds * 1e9) * 100.0);
      }
      result.gpuPercent[row.key] = std::min(100.0, busiest);
    }

    if (!row.expanded) {
      continue;
    }
    std::map<std::string, ProcessGroupUsage> byCommand;
    for (const int pid : pids) {
      std::string command = comm(pid);
      if (command.empty()) {
        continue; // exited between the reads
      }
      const std::uint64_t ticks = readProcessCpuTicks(pid);
      result.state.pidTicks[pid] = ticks;
      auto& group = byCommand[command];
      group.command = command;
      ++group.count;
      group.residentBytes += residentBytes(pid);
      if (const auto before = state.pidTicks.find(pid);
          before != state.pidTicks.end() && seconds > 0.0 && ticks >= before->second) {
        group.cpuPercent +=
            static_cast<double>(ticks - before->second) / ticksPerSecond / (seconds * std::max(1, cpuCount)) * 100.0;
      }
    }
    auto& list = result.groups[row.key];
    for (auto& [command, group] : byCommand) {
      list.push_back(std::move(group));
    }
    std::ranges::sort(list, std::greater{}, &ProcessGroupUsage::residentBytes);
  }
  if (tcp.has_value()) {
    for (const auto& [cgroup, sockets] : *tcp) {
      result.state.socketBytes.insert(sockets.begin(), sockets.end());
    }
    result.state.socketsComplete = true;
  } else {
    result.state.socketBytes = std::move(state.socketBytes);
  }
  std::erase_if(state.drmFds, [&seenPids](const auto& entry) { return !seenPids.contains(entry.first); });
  result.state.drmFds = std::move(state.drmFds);
  return result;
}

void ActivityDetails::snapshot(const std::vector<ActivityRow>& rows, std::chrono::steady_clock::time_point now) {
  if (m_lastSnapshot != std::chrono::steady_clock::time_point{} && now - m_lastSnapshot < kSnapshotInterval) {
    return;
  }
  m_lastSnapshot = now;
  for (auto& [key, history] : m_history) {
    if (!history.longMemoryMb.empty() && std::ranges::find(rows, key, &ActivityRow::key) == rows.end()) {
      push(history.longMemoryMb, 0.0F, kLongSamples); // gone: its line drops to zero, then ages out
    }
  }
  for (const auto& row : rows) {
    push(
        m_history[row.key].longMemoryMb, static_cast<float>(static_cast<double>(row.memoryBytes) / (1024.0 * 1024.0)),
        kLongSamples
    );
  }
  // A row gone for 24 h has only zeros left.
  std::erase_if(m_history, [](const auto& entry) {
    const auto& ring = entry.second.longMemoryMb;
    return entry.second.memoryMb.empty() && std::ranges::all_of(ring, [](float v) { return v == 0.0F; });
  });
}

void ActivityDetails::resetShort() {
  for (auto& [key, history] : m_history) {
    history.memoryMb.clear();
    history.cpuPercent.clear();
  }
  std::erase_if(m_history, [](const auto& entry) { return entry.second.longMemoryMb.empty(); });
  m_probeState = {};
  m_probing = false;
  ++m_generation;
  m_gpuPercent.clear();
  m_networkBytesPerSec.clear();
  m_groups.clear();
  m_lastUpdate = {};
}

void ActivityDetails::setExpanded(const std::string& rowKey, bool expanded) {
  if (expanded) {
    m_expanded.insert(rowKey);
  } else {
    m_expanded.erase(rowKey);
  }
}

std::optional<double> ActivityDetails::networkBytesPerSec(const std::string& rowKey) const {
  const auto it = m_networkBytesPerSec.find(rowKey);
  return it != m_networkBytesPerSec.end() ? std::optional{it->second} : std::nullopt;
}

double ActivityDetails::gpuPercent(const std::string& rowKey) const {
  const auto it = m_gpuPercent.find(rowKey);
  return it != m_gpuPercent.end() ? it->second : 0.0;
}

const ActivityDetails::RowHistory* ActivityDetails::history(const std::string& rowKey) const {
  const auto it = m_history.find(rowKey);
  return it != m_history.end() ? &it->second : nullptr;
}

const std::vector<ProcessGroupUsage>* ActivityDetails::groups(const std::string& rowKey) const {
  const auto it = m_groups.find(rowKey);
  return it != m_groups.end() ? &it->second : nullptr;
}

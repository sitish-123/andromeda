#include "system/container_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace {

  constexpr int kListTimeoutSec = 3;
  constexpr int kStopGraceSec = 10;                      // the engine's own default before SIGKILL
  constexpr int kStopTimeoutSec = kStopGraceSec + 10;

  struct HttpReply {
    int status = 0;
    std::string body;
    std::string error;
  };

  std::string decodeChunked(std::string_view body) {
    std::string out;
    while (!body.empty()) {
      const auto eol = body.find("\r\n");
      if (eol == std::string_view::npos) {
        break;
      }
      const std::size_t size = std::strtoull(std::string(body.substr(0, eol)).c_str(), nullptr, 16);
      if (size == 0 || eol + 2 + size > body.size()) {
        break;
      }
      out.append(body.substr(eol + 2, size));
      body.remove_prefix(std::min(body.size(), eol + 2 + size + 2));
    }
    return out;
  }

  // One HTTP/1.0 request on a unix socket: the server closes the connection after the reply, so we read to EOF.
  // timeoutSec bounds the whole request; io->stopping or a shutdown() from the owner ends it early.
  template <typename Io>
  HttpReply request(Io& io, const std::string& socketPath, std::string_view method, const std::string& path, int timeoutSec) {
    HttpReply reply;
    sockaddr_un addr{};
    if (socketPath.size() >= sizeof(addr.sun_path)) {
      reply.error = "socket path too long";
      return reply;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      reply.error = std::strerror(errno);
      return reply;
    }
    {
      std::scoped_lock lock(io.mutex);
      if (io.stopping) {
        ::close(fd);
        reply.error = "stopping";
        return reply;
      }
      io.fds.insert(fd);
    }
    const auto finish = [&] {
      std::scoped_lock lock(io.mutex);
      io.fds.erase(fd);
      ::close(fd);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    const timeval timeout{.tv_sec = timeoutSec, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      reply.error = errno == EACCES ? "permission" : std::strerror(errno);
      finish();
      return reply;
    }
    const std::string head = std::format("{} {} HTTP/1.0\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n", method, path);
    if (::send(fd, head.data(), head.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(head.size())) {
      reply.error = std::strerror(errno);
      finish();
      return reply;
    }
    std::string raw;
    char buffer[16384];
    while (true) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      pollfd waiter{.fd = fd, .events = POLLIN, .revents = 0};
      if (left.count() <= 0 || ::poll(&waiter, 1, static_cast<int>(left.count())) == 0) {
        reply.error = "timeout";
        break;
      }
      if (io.stopping) {
        reply.error = "stopping";
        break;
      }
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (n > 0) {
        raw.append(buffer, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (n < 0) {
        reply.error = std::strerror(errno);
      }
      break;
    }
    finish();
    const auto split = raw.find("\r\n\r\n");
    if (!reply.error.empty() || !raw.starts_with("HTTP/") || split == std::string::npos) {
      if (reply.error.empty()) {
        reply.error = "not an HTTP reply";
      }
      return reply;
    }
    const auto space = raw.find(' ');
    reply.status = static_cast<int>(std::strtol(raw.c_str() + space + 1, nullptr, 10));
    std::string headers = raw.substr(0, split);
    std::ranges::transform(headers, headers.begin(), [](unsigned char c) { return std::tolower(c); });
    reply.body = raw.substr(split + 4);
    if (headers.contains("transfer-encoding: chunked")) {
      reply.body = decodeChunked(reply.body);
    }
    return reply;
  }

  std::string engineError(const HttpReply& reply) {
    if (!reply.error.empty()) {
      return reply.error;
    }
    const auto body = nlohmann::json::parse(reply.body, nullptr, false);
    return body.is_object() && body.contains("message") && body["message"].is_string()
        ? body["message"].get<std::string>()
        : std::format("HTTP {}", reply.status);
  }

  std::string unixPath(const char* env) {
    const char* value = std::getenv(env);
    constexpr std::string_view kScheme = "unix://";
    return value != nullptr && std::string_view(value).starts_with(kScheme) ? std::string(value + kScheme.size()) : "";
  }

  std::string readLine(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
  }

  // The cgroup2 path of a process ("0::/system.slice/docker-<id>.scope" -> "/system.slice/docker-<id>.scope").
  std::string cgroupOf(int pid) {
    std::ifstream in(std::format("/proc/{}/cgroup", pid));
    for (std::string line; std::getline(in, line);) {
      if (line.starts_with("0::")) {
        return line.substr(3);
      }
    }
    return {};
  }

  std::uint64_t cpuUsageUsec(const std::string& dir) {
    std::ifstream in(dir + "/cpu.stat");
    std::string key;
    std::uint64_t value = 0;
    while (in >> key >> value) {
      if (key == "usage_usec") {
        return value;
      }
    }
    return 0;
  }

} // namespace

std::vector<std::string> ContainerService::defaultSockets() {
  std::vector<std::string> candidates{unixPath("DOCKER_HOST"), unixPath("CONTAINER_HOST")};
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
    candidates.push_back(std::string(runtime) + "/docker.sock");
    candidates.push_back(std::string(runtime) + "/podman/podman.sock");
  }
  candidates.emplace_back("/run/docker.sock");
  candidates.emplace_back("/run/podman/podman.sock");
  std::vector<std::string> found;
  std::vector<fs::path> seen;
  for (const auto& path : candidates) {
    std::error_code ec;
    if (path.empty() || !fs::is_socket(path, ec)) {
      continue;
    }
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (std::ranges::find(seen, canonical) == seen.end()) {
      seen.push_back(canonical);
      found.push_back(path);
    }
  }
  return found;
}

ContainerEngine ContainerService::engineFor(const std::string& socket) {
  ContainerEngine engine;
  engine.socket = socket;
  const bool podman = fs::path(socket).filename().string().contains("podman");
  engine.name = podman ? "Podman Engine" : "Docker Engine";
  struct stat info{};
  engine.rootless = ::stat(socket.c_str(), &info) == 0 && info.st_uid == ::getuid() && ::getuid() != 0;
  // The unit names Docker and Podman ship; rootless Docker installs only the service.
  engine.units = podman ? std::vector<std::string>{"podman.service", "podman.socket"}
      : engine.rootless ? std::vector<std::string>{"docker.service"}
                        : std::vector<std::string>{"docker.service", "docker.socket"};
  return engine;
}

ContainerService::ContainerService(std::string cgroupRoot, int cpuCount, std::vector<std::string> sockets)
    : m_cgroupRoot(std::move(cgroupRoot)), m_cpuCount(std::max(1, cpuCount)), m_sockets(std::move(sockets)) {
  for (const auto& socket : m_sockets) {
    m_engines.push_back(engineFor(socket));
  }
}

ContainerService::~ContainerService() {
  std::scoped_lock lock(m_io->mutex);
  m_io->stopping = true;
  for (const int fd : m_io->fds) {
    ::shutdown(fd, SHUT_RDWR);
  }
}

const ContainerInfo* ContainerService::find(const std::string& id) const {
  const auto it = std::ranges::find(m_containers, id, &ContainerInfo::id);
  return it != m_containers.end() ? &*it : nullptr;
}

int ContainerService::runningCount() const {
  return static_cast<int>(std::ranges::count(m_containers, std::string("running"), &ContainerInfo::state));
}

void ContainerService::refresh(std::set<std::string> idleSockets, std::function<void()> done) {
  if (m_sockets.empty()) {
    return;
  }
  if (m_refreshing) {
    m_queued = Queued{.idle = std::move(idleSockets), .done = std::move(done)};
    return;
  }
  m_refreshing = true;
  m_worker.post<Snapshot>(
      m_alive,
      [io = m_io, root = m_cgroupRoot, cpus = m_cpuCount, engines = m_engines, cpu = m_cpu, cgroups = m_cgroups,
       idle = std::move(idleSockets)] {
        Snapshot next;
        for (ContainerEngine engine : engines) {
          engine.error.clear();
          engine.idle = idle.contains(engine.socket);
          if (engine.idle || io->stopping) {
            next.engines.push_back(std::move(engine));
            continue;
          }
          const std::string& socket = engine.socket;
          const HttpReply version = request(*io, socket, "GET", "/version", kListTimeoutSec);
          const auto body = nlohmann::json::parse(version.body, nullptr, false);
          if (version.status != 200 || !body.is_object()) {
            engine.error = engineError(version);
            next.engines.push_back(std::move(engine));
            continue;
          }
          bool podman = false;
          for (const auto& component : body.value("Components", nlohmann::json::array())) {
            podman = podman || (component.is_object() && component.value("Name", "").contains("Podman"));
          }
          if (podman && !engine.name.starts_with("Podman")) { // podman-docker serves Podman on docker.sock
            engine.name = "Podman Engine";
            engine.units = {"podman.service", "podman.socket"};
          }
          const HttpReply list = request(*io, socket, "GET", "/containers/json?all=1", kListTimeoutSec);
          const auto items = nlohmann::json::parse(list.body, nullptr, false);
          if (list.status != 200 || !items.is_array()) {
            engine.error = engineError(list);
            next.engines.push_back(std::move(engine));
            continue;
          }
          for (const auto& item : items) {
            if (io->stopping) {
              break;
            }
            ContainerInfo info;
            info.id = item.value("Id", "");
            const auto names = item.value("Names", nlohmann::json::array());
            info.name = !names.empty() && names[0].is_string() ? names[0].get<std::string>() : info.id.substr(0, 12);
            if (info.name.starts_with('/')) {
              info.name.erase(0, 1);
            }
            info.image = item.value("Image", "");
            info.state = item.value("State", "");
            info.status = item.value("Status", "");
            info.createdSec = item.value("Created", std::int64_t{0});
            info.engineSocket = socket;
            if (info.state == "running" || info.state == "paused") {
              const std::string key = info.id + "|" + std::to_string(info.createdSec);
              if (const auto known = cgroups.find(key); known != cgroups.end()) {
                info.cgroup = known->second;
              } else {
                const HttpReply inspect = request(*io, socket, "GET", "/containers/" + info.id + "/json", kListTimeoutSec);
                const auto detail = nlohmann::json::parse(inspect.body, nullptr, false);
                const int pid = detail.is_object() && detail.contains("State") && detail["State"].is_object()
                    ? detail["State"].value("Pid", 0)
                    : 0;
                info.cgroup = pid > 0 ? cgroupOf(pid) : "";
              }
              if (!info.cgroup.empty()) {
                next.cgroups[key] = info.cgroup;
                const std::string dir = root + info.cgroup;
                info.memoryBytes = std::strtoull(readLine(dir + "/memory.current").c_str(), nullptr, 10);
                const CpuSample sample{.usageUsec = cpuUsageUsec(dir), .at = std::chrono::steady_clock::now()};
                if (const auto last = cpu.find(info.id); last != cpu.end() && sample.usageUsec >= last->second.usageUsec) {
                  const double seconds = std::chrono::duration<double>(sample.at - last->second.at).count();
                  info.cpuPercent = seconds > 0.0
                      ? static_cast<double>(sample.usageUsec - last->second.usageUsec) / (seconds * 1e6 * cpus) * 100.0
                      : 0.0;
                }
                next.cpu[info.id] = sample;
              }
            }
            next.containers.push_back(std::move(info));
          }
          next.engines.push_back(std::move(engine));
        }
        return next;
      },
      [this, done = std::move(done)](Snapshot next) {
        m_refreshing = false;
        m_engines = std::move(next.engines);
        m_containers = std::move(next.containers);
        m_cpu = std::move(next.cpu);
        m_cgroups = std::move(next.cgroups);
        if (done) {
          done();
        }
        if (m_queued.has_value()) {
          Queued queued = std::move(*m_queued);
          m_queued.reset();
          refresh(std::move(queued.idle), std::move(queued.done));
        }
      }
  );
}

void ContainerService::act(const ContainerInfo& container, Action action, std::function<void(std::string)> done) {
  const char* verb = action == Action::Stop ? "stop" : action == Action::Pause ? "pause" : "unpause";
  std::string path = std::format("/containers/{}/{}", container.id, verb);
  if (action == Action::Stop) {
    path += std::format("?t={}", kStopGraceSec);
  }
  m_worker.post<std::string>(
      m_alive,
      [io = m_io, socket = container.engineSocket, path = std::move(path), action] {
        const HttpReply reply =
            request(*io, socket, "POST", path, action == Action::Stop ? kStopTimeoutSec : kListTimeoutSec);
        // 304: already stopped / not paused; the state the user asked for holds.
        return reply.error.empty() && (reply.status == 204 || reply.status == 304 || reply.status == 200)
            ? std::string()
            : engineError(reply);
      },
      std::move(done)
  );
}

std::string ContainerService::dumpJson() const {
  nlohmann::json out{{"engines", nlohmann::json::array()}, {"containers", nlohmann::json::array()}};
  for (const auto& engine : m_engines) {
    out["engines"].push_back(
        {{"socket", engine.socket},
         {"name", engine.name},
         {"error", engine.error},
         {"rootless", engine.rootless},
         {"idle", engine.idle},
         {"units", engine.units}}
    );
  }
  for (const auto& c : m_containers) {
    out["containers"].push_back(
        {{"id", c.id},
         {"name", c.name},
         {"image", c.image},
         {"state", c.state},
         {"engine", c.engineSocket},
         {"cgroup", c.cgroup},
         {"cpu_percent", c.cpuPercent},
         {"memory_bytes", c.memoryBytes}}
    );
  }
  return out.dump();
}

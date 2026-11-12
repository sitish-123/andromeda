#include "compositors/andromeda/andromeda_runtime.h"
#include "compositors/andromeda/andromeda_workspace_backend.h"
#include "test_check.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using compositors::andromeda::AndromedaRuntime;

namespace {
  // A stand-in compositor socket: takes the shell's event-stream connection and writes event lines to it.
  class Server {
  public:
    Server() {
      char directory[] = "/tmp/noctalia-slide-test-XXXXXX";
      const auto* created = mkdtemp(directory);
      TEST_CHECK(created != nullptr);
      m_directory = created;
      m_path = m_directory + "/andromeda.sock";
      TEST_CHECK(setenv("ANDROMEDA_SOCKET", m_path.c_str(), 1) == 0);
      m_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      TEST_CHECK(m_fd >= 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
      TEST_CHECK(bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(listen(m_fd, 4) == 0);
    }
    ~Server() {
      closeClient();
      close(m_fd);
      unlink(m_path.c_str());
      rmdir(m_directory.c_str());
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void accept() {
      pollfd ready{.fd = m_fd, .events = POLLIN, .revents = 0};
      TEST_CHECK(poll(&ready, 1, 3000) == 1);
      m_client = accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
      TEST_CHECK(m_client >= 0);
    }
    // The next request line the shell wrote.
    std::string request() {
      while (!m_input.contains('\n')) {
        pollfd ready{.fd = m_client, .events = POLLIN, .revents = 0};
        TEST_CHECK(poll(&ready, 1, 3000) == 1);
        char data[4096];
        const auto size = recv(m_client, data, sizeof(data), 0);
        TEST_CHECK(size > 0);
        m_input.append(data, static_cast<std::size_t>(size));
      }
      const auto end = m_input.find('\n');
      std::string line = m_input.substr(0, end);
      m_input.erase(0, end + 1);
      return line;
    }
    void send(AndromedaRuntime& runtime, const nlohmann::json& line) {
      const std::string text = line.dump() + "\n";
      TEST_CHECK(::send(m_client, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size()));
      runtime.dispatchPoll(POLLIN);
    }
    void closeClient() {
      if (m_client >= 0) {
        close(m_client);
        m_client = -1;
      }
    }

  private:
    std::string m_directory;
    std::string m_path;
    std::string m_input;
    int m_fd = -1;
    int m_client = -1;
  };

  nlohmann::json slide(const std::string& output, const std::string& phase, double progress) {
    return {{"event", "workspace_slide"},
            {"data",
             {{"output", output},
              {"phase", phase},
              {"from", output + ":1"},
              {"to", output + ":2"},
              {"progress", progress},
              {"time_ms", 1000.0}}}};
  }

  nlohmann::json outputs(const std::vector<std::string>& names) {
    nlohmann::json data = nlohmann::json::array();
    for (const auto& name : names) {
      data.push_back({{"name", name}, {"powered", true}});
    }
    return {{"event", "outputs"}, {"data", data}};
  }
} // namespace

int main() {
  Server server;
  AndromedaRuntime runtime;
  AndromedaWorkspaceBackend backend(runtime);
  int changes = 0;
  backend.setSlideChangeCallback([&changes] { ++changes; });
  server.accept();

  // workspace_slide goes in a request of its own, so a compositor that does not know it rejects only that request.
  const auto events = nlohmann::json::parse(server.request());
  TEST_CHECK(events.value("cmd", "") == "subscribe");
  const auto named = [&events](const char* name) {
    return std::ranges::find(events.at("events"), nlohmann::json(name)) != events.at("events").end();
  };
  TEST_CHECK(!named("workspace_slide") && named("windows"));
  const auto slides = nlohmann::json::parse(server.request());
  TEST_CHECK(slides.at("events") == nlohmann::json::array({"workspace_slide"}));

  // The rejection keeps the stream: the other events still arrive.
  server.send(runtime, {{"err", "unknown subscription event: workspace_slide"}});
  TEST_CHECK(runtime.pollFd() >= 0);
  server.send(runtime, {{"event", "overview"}, {"data", {{"open", true}}}});
  TEST_CHECK(backend.hasOverviewState() && backend.isOverviewOpen());

  // A line reports the slide on its own output only.
  server.send(runtime, slide("DP-1", "tracking", 0.25));
  const auto tracking = backend.workspaceSlide("DP-1");
  TEST_CHECK(tracking.has_value());
  TEST_CHECK(tracking->phase == WorkspaceSlide::Phase::Tracking);
  TEST_CHECK(tracking->from == "DP-1:1" && tracking->to == "DP-1:2");
  TEST_CHECK(tracking->progress == 0.25 && tracking->timeMs == 1000.0);
  TEST_CHECK(!backend.workspaceSlide("HDMI-A-1").has_value());
  TEST_CHECK(changes == 1);
  server.send(runtime, slide("DP-1", "settling", 0.5));
  TEST_CHECK(backend.workspaceSlide("DP-1")->phase == WorkspaceSlide::Phase::Settling);

  // Another output's idle line leaves this slide alone; its own idle line ends it.
  server.send(runtime, slide("HDMI-A-1", "idle", 0.0));
  TEST_CHECK(backend.workspaceSlide("DP-1").has_value());
  server.send(runtime, slide("DP-1", "idle", 0.0));
  TEST_CHECK(!backend.workspaceSlide("DP-1").has_value());

  // An output that leaves mid-slide takes its slide with it, idle line or not.
  server.send(runtime, slide("DP-1", "settling", 0.5));
  server.send(runtime, slide("HDMI-A-1", "settling", 0.5));
  changes = 0;
  server.send(runtime, outputs({"HDMI-A-1"}));
  TEST_CHECK(!backend.workspaceSlide("DP-1").has_value());
  TEST_CHECK(backend.workspaceSlide("HDMI-A-1").has_value());
  TEST_CHECK(changes == 1);

  // A lost stream ends every slide.
  changes = 0;
  server.closeClient();
  runtime.dispatchPoll(POLLIN | POLLHUP);
  TEST_CHECK(!backend.workspaceSlide("HDMI-A-1").has_value());
  TEST_CHECK(changes == 1);

  backend.setSlideChangeCallback({});
  return 0;
}

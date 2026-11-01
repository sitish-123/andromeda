#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <sys/types.h>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // `andromeda --greeter`: the login screen session (docs/architecture-v2.md, greeter mode). The compositor runs one
  // client, the greeter, and exits when it does.
  [[nodiscard]] bool greeterMode();
  void setGreeterMode(bool enabled);

  // Globals no client of the greeter session sees: capture, clipboard managers, input injection, window lists, and output
  // control. dsk_settings_manager_v1 stays visible so the greeter reads appearance; its writes fail.
  [[nodiscard]] bool greeterHidesGlobal(std::string_view interface);

  // The greeter process. Its exit, seen through a pidfd (the SIGCHLD reaper may collect the status first), calls
  // `onExit` from the event loop.
  class GreeterChild {
  public:
    GreeterChild(wl_event_loop* loop, const char* command, std::function<void()> onExit);
    ~GreeterChild();

    GreeterChild(const GreeterChild&) = delete;
    GreeterChild& operator=(const GreeterChild&) = delete;

    [[nodiscard]] bool running() const { return m_source != nullptr; }

  private:
    static int handleExit(int fd, uint32_t mask, void* data);

    pid_t m_pid = -1;
    int m_pidfd = -1;
    wl_event_source* m_source = nullptr;
    std::function<void()> m_onExit;
  };

} // namespace andromeda

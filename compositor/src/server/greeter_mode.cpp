#include "server/greeter_mode.h"

#include "core/log.h"
#include "core/process.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("greeter");
    bool g_greeterMode = false;

    constexpr std::array<std::string_view, 15> kHiddenGlobals = {
        "zwlr_screencopy_manager_v1",
        "ext_image_copy_capture_manager_v1",
        "ext_output_image_capture_source_manager_v1",
        "ext_foreign_toplevel_image_capture_source_manager_v1",
        "zwlr_data_control_manager_v1",
        "ext_data_control_manager_v1",
        "zwp_virtual_keyboard_manager_v1",
        "zwlr_virtual_pointer_manager_v1",
        "andromeda_input_capture_manager_v1",
        "zwlr_foreign_toplevel_manager_v1",
        "ext_foreign_toplevel_list_v1",
        "dsk_output_manager_v1",
        "zwlr_output_manager_v1",
        "zwlr_gamma_control_manager_v1",
        "zwlr_output_power_manager_v1",
    };
  } // namespace

  bool greeterMode() { return g_greeterMode; }
  void setGreeterMode(bool enabled) { g_greeterMode = enabled; }

  bool greeterHidesGlobal(std::string_view interface) {
    return g_greeterMode && std::ranges::find(kHiddenGlobals, interface) != kHiddenGlobals.end();
  }

  GreeterChild::GreeterChild(wl_event_loop* loop, const char* command, std::function<void()> onExit)
      : m_onExit(std::move(onExit)) {
    m_pid = fork();
    if (m_pid == 0) {
      resetChildSignalState();
      execl("/bin/sh", "/bin/sh", "-c", command, static_cast<char*>(nullptr));
      _exit(127);
    }
    if (m_pid < 0) {
      kLog.error("greeter: fork failed");
      return;
    }
    m_pidfd = static_cast<int>(syscall(SYS_pidfd_open, m_pid, 0));
    if (m_pidfd < 0) {
      kLog.error("greeter: pidfd_open failed; the session will not end with the greeter");
      return;
    }
    m_source = wl_event_loop_add_fd(loop, m_pidfd, WL_EVENT_READABLE, handleExit, this);
    kLog.info("greeter: started pid={} command='{}'", m_pid, command);
  }

  GreeterChild::~GreeterChild() {
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
    }
    if (m_pidfd >= 0) {
      close(m_pidfd);
    }
  }

  int GreeterChild::handleExit(int /*fd*/, uint32_t /*mask*/, void* data) {
    auto* self = static_cast<GreeterChild*>(data);
    wl_event_source_remove(self->m_source);
    self->m_source = nullptr;
    kLog.info("greeter: pid={} exited; ending the greeter session", self->m_pid);
    self->m_onExit();
    return 0;
  }

} // namespace andromeda

#include "core/cpu_boost.h"

#include "core/application_scope.h"
#include "core/log.h"

#include <format>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <wayland-server-core.h>

#if ANDROMEDA_HAS_SD_BUS
#include <systemd/sd-bus.h>
#endif

namespace andromeda {

  namespace {
    constexpr Logger kLog("cpu-boost");

    // A sched_ext scheduler replaces the kernel's for normal tasks and may ignore cgroup cpu.weight (scx_lavd does).
    void logSchedExt() {
      std::ifstream state("/sys/kernel/sched_ext/state");
      std::string value;
      if (!std::getline(state, value) || value != "enabled") {
        return;
      }
      std::ifstream ops("/sys/kernel/sched_ext/root/ops");
      std::string name;
      std::getline(ops, name);
      kLog.warn("sched_ext scheduler {} runs: app CPU weights may have no effect", name.empty() ? "?" : name);
    }

    // cgroup v2 cpu.weight: default 100; the launching app gets 10x and the focused app 4x a busy terminal's share.
    constexpr unsigned kDefaultWeight = 100;
    constexpr unsigned kFocusWeight = 400;

    std::string unitOfPid(pid_t pid) {
      std::ifstream stream(std::format("/proc/{}/cgroup", pid));
      if (!stream) {
        return {};
      }
      const std::string unit =
          systemdUnitFromCgroup(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
      return unit.starts_with("app-") ? unit : std::string();
    }

    // systemd unit-name escaping, as the shell does for app-<desktop id>@<uuid>.service.
    std::string escapeUnitName(std::string_view name) {
      std::string out;
      for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if ((std::isalnum(c) != 0 || c == ':' || c == '_' || (c == '.' && i > 0))) {
          out += static_cast<char>(c);
        } else {
          out += std::format("\\x{:02x}", c);
        }
      }
      return out;
    }
  } // namespace

#if ANDROMEDA_HAS_SD_BUS
  CpuBoost::CpuBoost(wl_event_loop* loop) {
    logSchedExt();
    if (loop == nullptr || sd_bus_open_user(&m_bus) < 0) {
      m_bus = nullptr;
      return;
    }
    m_source = wl_event_loop_add_fd(loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    updateEvents();
  }

  CpuBoost::~CpuBoost() {
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
    }
    if (m_bus != nullptr) {
      sd_bus_flush_close_unref(m_bus);
    }
  }

  int CpuBoost::onEvent(int /*fd*/, unsigned /*mask*/, void* data) {
    static_cast<CpuBoost*>(data)->dispatch();
    return 0;
  }

  void CpuBoost::dispatch() {
    while (sd_bus_process(m_bus, nullptr) > 0) {
    }
    updateEvents();
  }

  void CpuBoost::updateEvents() {
    if (m_source == nullptr) {
      return;
    }
    const int events = sd_bus_get_events(m_bus);
    wl_event_source_fd_update(
        m_source, WL_EVENT_READABLE | ((events > 0 && (events & POLLOUT) != 0) ? static_cast<uint32_t>(WL_EVENT_WRITABLE) : 0U)
    );
  }

  void CpuBoost::setWeight(const std::string& unit, unsigned weight) {
    if (m_bus == nullptr || unit.empty()) {
      return;
    }
    sd_bus_message* message = nullptr;
    if (sd_bus_message_new_method_call(
            m_bus, &message, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "SetUnitProperties"
        )
            >= 0
        && sd_bus_message_append(message, "sb", unit.c_str(), 1) >= 0
        && sd_bus_message_append(message, "a(sv)", 1, "CPUWeight", "t", static_cast<std::uint64_t>(weight)) >= 0) {
      (void)sd_bus_call_async(m_bus, nullptr, message, nullptr, nullptr, 0);
    }
    sd_bus_message_unref(message);
    updateEvents();
  }

  void CpuBoost::expireLaunch(std::string_view appId) {
    if (m_bus == nullptr || appId.empty()) {
      return;
    }
    sd_bus_message* message = nullptr;
    const std::string pattern = "app-" + escapeUnitName(appId) + "@*.service";
    if (sd_bus_message_new_method_call(
            m_bus, &message, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "ListUnitsByPatterns"
        )
            >= 0
        && sd_bus_message_append(message, "as", 0) >= 0
        && sd_bus_message_append(message, "as", 1, pattern.c_str()) >= 0) {
      (void)sd_bus_call_async(
          m_bus, nullptr, message,
          [](sd_bus_message* reply, void* data, sd_bus_error* /*error*/) -> int {
            auto* self = static_cast<CpuBoost*>(data);
            if (sd_bus_message_is_method_error(reply, nullptr) != 0
                || sd_bus_message_enter_container(reply, 'a', "(ssssssouso)") < 0) {
              return 0;
            }
            const char* name = nullptr;
            while (sd_bus_message_read(reply, "(ssssssouso)", &name, nullptr, nullptr, nullptr, nullptr, nullptr,
                                       nullptr, nullptr, nullptr, nullptr)
                   > 0) {
              if (name != nullptr && name != self->m_focused) {
                self->setWeight(name, kDefaultWeight);
              }
            }
            return 0;
          },
          this, 0
      );
    }
    sd_bus_message_unref(message);
    updateEvents();
  }
#else
  CpuBoost::CpuBoost(wl_event_loop* /*loop*/) {}
  CpuBoost::~CpuBoost() = default;
  int CpuBoost::onEvent(int /*fd*/, unsigned /*mask*/, void* /*data*/) { return 0; }
  void CpuBoost::dispatch() {}
  void CpuBoost::updateEvents() {}
  void CpuBoost::setWeight(const std::string& /*unit*/, unsigned /*weight*/) {}
  void CpuBoost::expireLaunch(std::string_view /*appId*/) {}
#endif

  void CpuBoost::focus(pid_t pid) {
    const std::string unit = pid > 0 ? unitOfPid(pid) : std::string();
    if (unit.empty() || unit == m_focused) {
      return;
    }
    setWeight(m_focused, kDefaultWeight);
    setWeight(unit, kFocusWeight);
    m_focused = unit;
  }

} // namespace andromeda

#include "server/sleep_watch.h"

#include "core/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <systemd/sd-bus.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("sleep");
    constexpr const char* kName = "org.freedesktop.login1";
    constexpr const char* kPath = "/org/freedesktop/login1";
    constexpr const char* kInterface = "org.freedesktop.login1.Manager";
  } // namespace

  SleepWatch::SleepWatch(wl_event_loop* loop, std::function<void(bool)> sleeping) : m_sleeping(std::move(sleeping)) {
    if (loop == nullptr || sd_bus_open_system(&m_bus) < 0) {
      m_bus = nullptr;
      kLog.warn("no system bus: the watchdog stays armed across sleep");
      return;
    }
    m_source = wl_event_loop_add_fd(loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    if (m_source == nullptr
        || sd_bus_match_signal_async(m_bus, nullptr, kName, kPath, kInterface, "PrepareForSleep", onPrepareForSleep,
                                     nullptr, this)
               < 0) {
      kLog.warn("cannot follow logind PrepareForSleep");
      return;
    }
    inhibit();
    dispatch();
  }

  SleepWatch::~SleepWatch() {
    release();
    m_inhibitSlot = sd_bus_slot_unref(m_inhibitSlot);
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
    }
    if (m_bus != nullptr) {
      sd_bus_flush_close_unref(m_bus);
    }
  }

  void SleepWatch::inhibit() {
    m_inhibitSlot = sd_bus_slot_unref(m_inhibitSlot);
    if (sd_bus_call_method_async(m_bus, &m_inhibitSlot, kName, kPath, kInterface, "Inhibit", onInhibitReply, this,
                                 "ssss", "sleep", "andromeda", "Pause the compositor watchdog", "delay")
        < 0) {
      kLog.warn("cannot request a sleep delay lock");
    }
  }

  void SleepWatch::release() {
    if (m_lockFd >= 0) {
      close(m_lockFd);
      m_lockFd = -1;
    }
  }

  int SleepWatch::onInhibitReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<SleepWatch*>(data);
    self->m_inhibitSlot = sd_bus_slot_unref(self->m_inhibitSlot);
    int fd = -1;
    if (sd_bus_message_is_method_error(reply, nullptr) || sd_bus_message_read(reply, "h", &fd) < 0) {
      kLog.warn("logind refused a sleep delay lock");
      return 0;
    }
    self->release();
    self->m_lockFd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    return 0;
  }

  int SleepWatch::onPrepareForSleep(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<SleepWatch*>(data);
    int start = 0;
    if (sd_bus_message_read(message, "b", &start) < 0) {
      return 0;
    }
    self->m_sleeping(start != 0);
    if (start != 0) {
      self->release();
    } else {
      self->inhibit();
    }
    return 0;
  }

  int SleepWatch::onEvent(int /*fd*/, std::uint32_t /*mask*/, void* data) {
    static_cast<SleepWatch*>(data)->dispatch();
    return 0;
  }

  void SleepWatch::dispatch() {
    while (sd_bus_process(m_bus, nullptr) > 0) {
    }
    sd_bus_flush(m_bus);
  }

} // namespace andromeda

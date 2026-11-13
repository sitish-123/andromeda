#pragma once

#include <cstdint>
#include <functional>

struct sd_bus;
struct sd_bus_error;
struct sd_bus_message;
struct sd_bus_slot;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Follows logind PrepareForSleep and holds a sleep delay lock, so `sleeping(true)` runs before
  // systemd-sleep freezes the session. Without a system bus it does nothing.
  class SleepWatch {
  public:
    SleepWatch(wl_event_loop* loop, std::function<void(bool sleeping)> sleeping);
    ~SleepWatch();

    SleepWatch(const SleepWatch&) = delete;
    SleepWatch& operator=(const SleepWatch&) = delete;

  private:
    static int onEvent(int fd, std::uint32_t mask, void* data);
    static int onPrepareForSleep(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onInhibitReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    void inhibit();
    void release();
    void dispatch();

    std::function<void(bool)> m_sleeping;
    sd_bus* m_bus = nullptr;
    sd_bus_slot* m_inhibitSlot = nullptr;
    wl_event_source* m_source = nullptr;
    int m_lockFd = -1;
  };

} // namespace andromeda

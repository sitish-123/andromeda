#pragma once

#include <cstdint>
#include <functional>

struct sd_bus;
struct sd_bus_error;
struct sd_bus_message;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Reads org.andromeda.System1.Power.PackageEnergyJ (CPU package joules since that daemon started; RAPL is
  // root-only) over the system bus without blocking the event loop. The bus opens on the first request.
  class PackageEnergy {
  public:
    explicit PackageEnergy(wl_event_loop* loop);
    ~PackageEnergy();

    PackageEnergy(const PackageEnergy&) = delete;
    PackageEnergy& operator=(const PackageEnergy&) = delete;

    // `done` gets the joules, or NaN when the daemon or RAPL is missing. A request while one is pending is dropped.
    void request(std::function<void(double)> done);

  private:
    static int onEvent(int fd, std::uint32_t mask, void* data);
    static int onTimer(void* data);
    static int onReply(::sd_bus_message* reply, void* data, ::sd_bus_error* error);
    bool open();
    void close();
    void dispatch();
    void updateEvents();

    wl_event_loop* m_loop = nullptr;
    sd_bus* m_bus = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr;
    std::function<void(double)> m_done;
  };

} // namespace andromeda

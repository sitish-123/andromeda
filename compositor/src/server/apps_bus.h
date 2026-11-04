#pragma once

#include <cstdint>
#include <map>
#include <string>

struct sd_bus;
struct sd_bus_message;
struct sd_bus_error;
struct sd_bus_slot;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // org.andromeda.Compositor1 /org/andromeda/Compositor1/Apps on the session bus (docs/system-health.md §1, §5): app
  // stats and tier and freeze holds for the andromeda-system session part. Holds live in qos::registry(); the QoS
  // controller applies them. cpu.stat and memory.current are read every 30 s, only while a hold exists or a client
  // called List() in the last 2 minutes.
  class AppsBus {
  public:
    explicit AppsBus(wl_event_loop* loop);
    ~AppsBus();
    AppsBus(const AppsBus&) = delete;
    AppsBus& operator=(const AppsBus&) = delete;

  private:
    struct Sample {
      std::uint64_t usageUs = 0;
      std::uint64_t atUs = 0;
      double cpuPct = 0.0; // of one core over the last interval
      std::uint64_t rssBytes = 0;
    };

    static int onEvent(int fd, unsigned mask, void* data);
    static int onTimer(void* data);
    static int onSample(void* data);
    static int onHoldTier(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onHoldFreeze(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onRelease(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onList(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* error);

    void dispatch();
    void updateEvents();
    void disconnect();
    void watch();
    void sample();
    int appendApps(sd_bus_message* message);
    void changed();

    sd_bus* m_bus = nullptr;
    sd_bus_slot* m_object = nullptr;
    sd_bus_slot* m_ownerMatch = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr;
    wl_event_source* m_sampleTimer = nullptr;
    bool m_sampling = false;
    std::uint64_t m_watchedUntilUs = 0;
    std::map<std::string, Sample> m_samples; // by unit
  };

} // namespace andromeda

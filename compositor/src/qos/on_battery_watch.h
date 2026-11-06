#pragma once

#include <cstdint>
#include <functional>
#include <optional>

struct sd_bus;
struct sd_bus_error;
struct sd_bus_message;
struct sd_bus_slot;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda::qos {

  // Follows org.andromeda.System1.Power.OnBattery over the system bus without blocking the event loop. Reading
  // /sys/class/power_supply/BAT*/status runs ACPI _BST through the EC and can take 40 ms on the main thread.
  class OnBatteryWatch {
  public:
    // `changed` runs after the value changes, never from inside the constructor.
    OnBatteryWatch(wl_event_loop* loop, std::function<void()> changed);
    ~OnBatteryWatch();

    OnBatteryWatch(const OnBatteryWatch&) = delete;
    OnBatteryWatch& operator=(const OnBatteryWatch&) = delete;

    // Empty until andromeda-system answered, and while it is not on the bus.
    [[nodiscard]] std::optional<bool> onBattery() const { return m_onBattery; }

  private:
    static int onEvent(int fd, std::uint32_t mask, void* data);
    static int onTimer(void* data);
    static int onGetReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    static int onPropertiesChanged(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* error);
    static int onRetry(void* data);
    bool open();
    void retryLater();
    void requestValue();
    void set(std::optional<bool> value);
    void close();
    void dispatch();
    void updateEvents();

    wl_event_loop* m_loop = nullptr;
    std::function<void()> m_changed;
    sd_bus* m_bus = nullptr;
    sd_bus_slot* m_getSlot = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr;
    wl_event_source* m_retry = nullptr; // outlives close(): it opens the bus again
    int m_retryMs = 2000;
    std::optional<bool> m_onBattery;
  };

} // namespace andromeda::qos

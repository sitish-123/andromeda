#include "qos/on_battery_watch.h"

#include "core/log.h"

#include <poll.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include <systemd/sd-bus.h>

namespace andromeda::qos {

  namespace {

    constexpr Logger kLog("qos");
    constexpr const char* kName = "org.andromeda.System1";
    constexpr const char* kPath = "/org/andromeda/System1/Power";
    constexpr const char* kInterface = "org.andromeda.System1.Power";
    constexpr int kRetryFirstMs = 2000;
    constexpr int kRetryMaxMs = 60000;

    std::uint64_t monotonicUs() {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000;
    }

    // Reads a "b" variant into `out`; false on any other type.
    bool readBoolVariant(sd_bus_message* message, bool& out) {
      int value = 0;
      if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "b") <= 0
          || sd_bus_message_read(message, "b", &value) < 0) {
        return false;
      }
      sd_bus_message_exit_container(message);
      out = value != 0;
      return true;
    }

  } // namespace

  OnBatteryWatch::OnBatteryWatch(wl_event_loop* loop, std::function<void()> changed)
      : m_loop(loop), m_changed(std::move(changed)) {
    if (m_loop == nullptr) {
      return;
    }
    m_retry = wl_event_loop_add_timer(m_loop, onRetry, this);
    if (!open()) {
      retryLater();
    }
  }

  bool OnBatteryWatch::open() {
    if (sd_bus_open_system(&m_bus) < 0) {
      m_bus = nullptr;
      return false;
    }
    m_source = wl_event_loop_add_fd(m_loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    m_timer = wl_event_loop_add_timer(m_loop, onTimer, this);
    if (m_source == nullptr || m_timer == nullptr
        || sd_bus_match_signal_async(
               m_bus, nullptr, kName, kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
               onPropertiesChanged, nullptr, this
           ) < 0
        || sd_bus_add_match_async(
               m_bus, nullptr,
               "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.andromeda.System1'",
               onOwnerChanged, nullptr, this
           ) < 0) {
      close();
      return false;
    }
    m_retryMs = kRetryFirstMs;
    requestValue();
    return true;
  }

  // A lost system bus (dbus-broker restart) is opened again, 2 s after the loss, then up to every 60 s.
  void OnBatteryWatch::retryLater() {
    if (m_retry != nullptr) {
      wl_event_source_timer_update(m_retry, m_retryMs);
      m_retryMs = std::min(m_retryMs * 2, kRetryMaxMs);
    }
  }

  int OnBatteryWatch::onRetry(void* data) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    if (!self->open()) {
      self->retryLater();
    }
    return 0;
  }

  OnBatteryWatch::~OnBatteryWatch() {
    close();
    if (m_retry != nullptr) {
      wl_event_source_remove(m_retry);
    }
  }

  void OnBatteryWatch::close() {
    m_getSlot = sd_bus_slot_unref(m_getSlot);
    for (wl_event_source** source : {&m_source, &m_timer}) {
      if (*source != nullptr) {
        wl_event_source_remove(*source);
        *source = nullptr;
      }
    }
    if (m_bus != nullptr) {
      sd_bus_flush_close_unref(m_bus);
      m_bus = nullptr;
    }
  }

  void OnBatteryWatch::requestValue() {
    m_getSlot = sd_bus_slot_unref(m_getSlot);
    if (sd_bus_call_method_async(
            m_bus, &m_getSlot, kName, kPath, "org.freedesktop.DBus.Properties", "Get", onGetReply, this, "ss",
            kInterface, "OnBattery"
        )
        < 0) {
      m_getSlot = nullptr;
    }
    updateEvents();
  }

  void OnBatteryWatch::set(std::optional<bool> value) {
    if (value == m_onBattery) {
      return;
    }
    m_onBattery = value;
    kLog.info("power source: {}", !value ? "unknown (andromeda-system not on the bus)" : *value ? "battery" : "mains");
    if (m_changed) {
      m_changed();
    }
  }

  int OnBatteryWatch::onGetReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    self->m_getSlot = sd_bus_slot_unref(self->m_getSlot);
    bool value = false;
    if (sd_bus_message_is_method_error(reply, nullptr) == 0 && readBoolVariant(reply, value)) {
      self->set(value);
    }
    return 0;
  }

  // PropertiesChanged: (s interface, a{sv} changed, as invalidated).
  int OnBatteryWatch::onPropertiesChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    const char* interface = nullptr;
    if (sd_bus_message_read(message, "s", &interface) < 0 || std::strcmp(interface, kInterface) != 0
        || sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
      return 0;
    }
    while (sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
      const char* name = nullptr;
      bool value = false;
      if (sd_bus_message_read(message, "s", &name) < 0) {
        return 0;
      }
      if (std::strcmp(name, "OnBattery") == 0 && readBoolVariant(message, value)) {
        self->set(value);
      } else if (sd_bus_message_skip(message, "v") < 0) {
        return 0;
      }
      sd_bus_message_exit_container(message);
    }
    return 0;
  }

  // NameOwnerChanged: (s name, s old owner, s new owner). A restarted daemon may report a new value.
  int OnBatteryWatch::onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<OnBatteryWatch*>(data);
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 || std::strcmp(name, kName) != 0) {
      return 0;
    }
    if (*newOwner == '\0') {
      self->set(std::nullopt);
    } else {
      self->requestValue();
    }
    return 0;
  }

  int OnBatteryWatch::onEvent(int /*fd*/, std::uint32_t /*mask*/, void* data) {
    static_cast<OnBatteryWatch*>(data)->dispatch();
    return 0;
  }

  int OnBatteryWatch::onTimer(void* data) {
    static_cast<OnBatteryWatch*>(data)->dispatch();
    return 0;
  }

  void OnBatteryWatch::dispatch() {
    if (m_bus == nullptr) {
      return;
    }
    int result = 0;
    while ((result = sd_bus_process(m_bus, nullptr)) > 0) {
    }
    if (result < 0) {
      close();
      set(std::nullopt);
      retryLater();
      return;
    }
    updateEvents();
  }

  void OnBatteryWatch::updateEvents() {
    if (m_bus == nullptr) {
      return;
    }
    const int events = sd_bus_get_events(m_bus);
    wl_event_source_fd_update(
        m_source,
        WL_EVENT_READABLE | ((events > 0 && (events & POLLOUT) != 0) ? static_cast<uint32_t>(WL_EVENT_WRITABLE) : 0U)
    );
    // sd_bus_get_timeout: absolute CLOCK_MONOTONIC µs of the next timeout, UINT64_MAX for none.
    std::uint64_t deadline = UINT64_MAX;
    int delayMs = 0;
    if (sd_bus_get_timeout(m_bus, &deadline) >= 0 && deadline != UINT64_MAX) {
      const std::uint64_t now = monotonicUs();
      delayMs = deadline <= now ? 1 : static_cast<int>((deadline - now + 999) / 1000);
    }
    wl_event_source_timer_update(m_timer, delayMs);
  }

} // namespace andromeda::qos

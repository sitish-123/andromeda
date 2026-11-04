#include "server/apps_bus.h"

#include "core/log.h"
#include "qos/holds.h"

#include <poll.h>
#include <sys/auxv.h>
#include <wayland-server-core.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <systemd/sd-bus.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("apps-bus");
    constexpr const char* kName = "org.andromeda.Compositor1";
    constexpr const char* kPath = "/org/andromeda/Compositor1/Apps";
    constexpr const char* kInterface = "org.andromeda.Compositor1.Apps";
    constexpr const char* kAppRow = "(sbbsbdt)";
    constexpr int kSampleMs = 30'000;
    constexpr std::uint64_t kWatchUs = 120'000'000;

    std::uint64_t monotonicUs() {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000;
    }

    // sd_bus_open_user reads its environment with secure_getenv, which is empty in a secure-exec process (the
    // packaged cap_sys_nice binary); main() restored DBUS_SESSION_BUS_ADDRESS into environ for getenv.
    int openUserBus(sd_bus** bus) {
      const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
      if (getauxval(AT_SECURE) == 0 || address == nullptr) {
        return sd_bus_open_user(bus);
      }
      sd_bus* opened = nullptr;
      int r = sd_bus_new(&opened);
      if (r >= 0) {
        r = sd_bus_set_address(opened, address);
      }
      if (r >= 0) {
        r = sd_bus_set_bus_client(opened, 1);
      }
      if (r >= 0) {
        r = sd_bus_start(opened);
      }
      if (r < 0) {
        sd_bus_unref(opened);
        return r;
      }
      *bus = opened;
      return 0;
    }

    const char* tierName(qos::Tier tier) {
      switch (tier) {
      case qos::Tier::Focus:
        return "T0";
      case qos::Tier::Visible:
        return "T1";
      case qos::Tier::Hidden:
        return "T2";
      case qos::Tier::HiddenLong:
        return "T3";
      }
      return "?";
    }

    std::optional<qos::Tier> tierFromName(std::string_view name) {
      if (name == "T1") {
        return qos::Tier::Visible;
      }
      if (name == "T2") {
        return qos::Tier::Hidden;
      }
      if (name == "T3") {
        return qos::Tier::HiddenLong;
      }
      return std::nullopt;
    }

    std::optional<std::uint64_t> readUsageUs(const std::string& dir) {
      std::ifstream in(dir + "/cpu.stat");
      std::string key;
      std::uint64_t value = 0;
      while (in >> key >> value) {
        if (key == "usage_usec") {
          return value;
        }
      }
      return std::nullopt;
    }

    std::uint64_t readRss(const std::string& dir) {
      std::ifstream in(dir + "/memory.current");
      std::uint64_t bytes = 0;
      in >> bytes;
      return bytes;
    }
  } // namespace

  AppsBus::AppsBus(wl_event_loop* loop) {
    static const sd_bus_vtable kVtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("HoldTier", "sss", "s", onHoldTier, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("HoldFreeze", "ss", "s", onHoldFreeze, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Release", "s", "", onRelease, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("List", "", "a(sbbsbdt)", onList, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_SIGNAL("AppStats", "a(sbbsbdt)", 0),
        SD_BUS_VTABLE_END,
    };
    if (loop == nullptr || openUserBus(&m_bus) < 0) {
      kLog.info("no session bus: org.andromeda.Compositor1.Apps is off");
      m_bus = nullptr;
      return;
    }
    const auto named = [](sd_bus_message* reply, void* /*data*/, sd_bus_error* /*error*/) -> int {
      if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
        kLog.info("{} is taken: Compositor1.Apps is off in this compositor", kName);
      }
      return 0;
    };
    if (sd_bus_add_object_vtable(m_bus, &m_object, kPath, kInterface, kVtable, this) < 0
        || sd_bus_match_signal_async(m_bus, &m_ownerMatch, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "NameOwnerChanged", onOwnerChanged, nullptr, this)
            < 0
        || sd_bus_request_name_async(m_bus, nullptr, kName, 0, named, nullptr) < 0) {
      kLog.warn("Compositor1.Apps: could not set up the bus object");
      disconnect();
      return;
    }
    m_source = wl_event_loop_add_fd(loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    m_timer = wl_event_loop_add_timer(loop, onTimer, this);
    m_sampleTimer = wl_event_loop_add_timer(loop, onSample, this);
    if (m_source == nullptr || m_timer == nullptr || m_sampleTimer == nullptr) {
      disconnect();
      return;
    }
    updateEvents();
  }

  AppsBus::~AppsBus() { disconnect(); }

  void AppsBus::disconnect() {
    for (wl_event_source** source : {&m_source, &m_timer, &m_sampleTimer}) {
      if (*source != nullptr) {
        wl_event_source_remove(*source);
        *source = nullptr;
      }
    }
    m_object = sd_bus_slot_unref(m_object);
    m_ownerMatch = sd_bus_slot_unref(m_ownerMatch);
    if (m_bus != nullptr) {
      m_bus = sd_bus_flush_close_unref(m_bus);
    }
  }

  int AppsBus::onEvent(int /*fd*/, unsigned mask, void* data) {
    auto* self = static_cast<AppsBus*>(data);
    self->dispatch();
    if (self->m_bus != nullptr && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
      kLog.warn("session bus closed: Compositor1.Apps stops");
      self->disconnect();
    }
    return 0;
  }

  int AppsBus::onTimer(void* data) {
    static_cast<AppsBus*>(data)->dispatch();
    return 0;
  }

  void AppsBus::dispatch() {
    if (m_bus == nullptr) {
      return;
    }
    int result = 0;
    while ((result = sd_bus_process(m_bus, nullptr)) > 0) {
    }
    if (result < 0) {
      kLog.warn("session bus failed: Compositor1.Apps stops");
      disconnect();
      return;
    }
    updateEvents();
  }

  void AppsBus::updateEvents() {
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

  void AppsBus::changed() {
    if (const auto& callback = qos::registry().changed) {
      callback();
    }
  }

  void AppsBus::watch() {
    m_watchedUntilUs = monotonicUs() + kWatchUs;
    if (!m_sampling) {
      m_sampling = true;
      sample();
      wl_event_source_timer_update(m_sampleTimer, kSampleMs);
    }
  }

  int AppsBus::onSample(void* data) {
    auto* self = static_cast<AppsBus*>(data);
    if (self->m_bus == nullptr) {
      return 0;
    }
    if (qos::registry().holds.empty() && monotonicUs() > self->m_watchedUntilUs) {
      self->m_sampling = false;
      self->m_samples.clear();
      return 0;
    }
    self->sample();
    sd_bus_message* signal = nullptr;
    if (sd_bus_message_new_signal(self->m_bus, &signal, kPath, kInterface, "AppStats") >= 0
        && self->appendApps(signal) >= 0) {
      (void)sd_bus_send(self->m_bus, signal, nullptr);
    }
    sd_bus_message_unref(signal);
    self->updateEvents();
    wl_event_source_timer_update(self->m_sampleTimer, kSampleMs);
    return 0;
  }

  void AppsBus::sample() {
    const auto& apps = qos::registry().apps;
    std::erase_if(m_samples, [&](const auto& entry) { return !apps.contains(entry.first); });
    const std::uint64_t now = monotonicUs();
    for (const auto& [unit, app] : apps) {
      const std::optional<std::uint64_t> usage = readUsageUs(app.dir);
      if (!usage) {
        continue;
      }
      Sample& last = m_samples[unit];
      if (last.atUs != 0 && now > last.atUs && *usage >= last.usageUs) {
        last.cpuPct = 100.0 * static_cast<double>(*usage - last.usageUs) / static_cast<double>(now - last.atUs);
      }
      last.usageUs = *usage;
      last.atUs = now;
      last.rssBytes = readRss(app.dir);
    }
  }

  int AppsBus::appendApps(sd_bus_message* message) {
    int r = sd_bus_message_open_container(message, 'a', kAppRow);
    for (const auto& [unit, app] : qos::registry().apps) {
      const auto it = m_samples.find(unit);
      const double cpu = it != m_samples.end() ? it->second.cpuPct : 0.0;
      const std::uint64_t rss = it != m_samples.end() ? it->second.rssBytes : 0;
      if (r >= 0) {
        r = sd_bus_message_append(message, kAppRow, unit.c_str(), static_cast<int>(app.visible),
                                  static_cast<int>(app.focused), tierName(app.tier), static_cast<int>(app.frozen), cpu,
                                  rss);
      }
    }
    return r < 0 ? r : sd_bus_message_close_container(message);
  }

  int AppsBus::onHoldTier(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* unit = nullptr;
    const char* tierText = nullptr;
    const char* reason = nullptr;
    if (const int r = sd_bus_message_read(message, "sss", &unit, &tierText, &reason); r < 0) {
      return r;
    }
    const std::optional<qos::Tier> tier = tierFromName(tierText);
    if (!tier) {
      return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "tier must be T1, T2 or T3");
    }
    qos::Registry& registry = qos::registry();
    const std::optional<std::string> id =
        registry.holds.holdTier(sd_bus_message_get_sender(message), unit, *tier, reason, registry.apps.contains(unit));
    if (!id) {
      return sd_bus_error_setf(error, "org.andromeda.Compositor1.Error.UnknownApp", "%s is not an app scope", unit);
    }
    self->watch();
    self->changed();
    return sd_bus_reply_method_return(message, "s", id->c_str());
  }

  int AppsBus::onHoldFreeze(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* unit = nullptr;
    const char* reason = nullptr;
    if (const int r = sd_bus_message_read(message, "ss", &unit, &reason); r < 0) {
      return r;
    }
    qos::Registry& registry = qos::registry();
    const std::optional<std::string> id =
        registry.holds.holdFreeze(sd_bus_message_get_sender(message), unit, reason, registry.apps.contains(unit));
    if (!id) {
      return sd_bus_error_setf(error, "org.andromeda.Compositor1.Error.UnknownApp", "%s is not an app scope", unit);
    }
    self->watch();
    self->changed();
    return sd_bus_reply_method_return(message, "s", id->c_str());
  }

  int AppsBus::onRelease(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* id = nullptr;
    if (const int r = sd_bus_message_read(message, "s", &id); r < 0) {
      return r;
    }
    if (!qos::registry().holds.release(id, sd_bus_message_get_sender(message))) {
      return sd_bus_error_setf(error, "org.andromeda.Compositor1.Error.NoHold", "no hold %s of this caller", id);
    }
    self->changed();
    return sd_bus_reply_method_return(message, "");
  }

  int AppsBus::onList(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<AppsBus*>(data);
    self->watch();
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(message, &reply);
    if (r >= 0) {
      r = self->appendApps(reply);
    }
    if (r >= 0) {
      r = sd_bus_send(nullptr, reply, nullptr);
    }
    sd_bus_message_unref(reply);
    return r;
  }

  int AppsBus::onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<AppsBus*>(data);
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0) {
      return 0;
    }
    if (name[0] == ':' && newOwner[0] == '\0' && qos::registry().holds.releaseOwner(name)) {
      self->changed();
    }
    return 0;
  }

} // namespace andromeda

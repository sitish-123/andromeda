#include "core/systemd_units.h"

#include "core/log.h"

#include <poll.h>
#include <sys/auxv.h>
#include <wayland-server-core.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <utility>

#if ANDROMEDA_HAS_SD_BUS
#include <systemd/sd-bus.h>
#endif

namespace andromeda {

#if ANDROMEDA_HAS_SD_BUS
  namespace {
    constexpr Logger kLog("systemd-units");
    constexpr const char* kService = "org.freedesktop.systemd1";
    constexpr const char* kPath = "/org/freedesktop/systemd1";
    constexpr const char* kManager = "org.freedesktop.systemd1.Manager";

    // The first failure is a warning (a missing cgroup delegation fails every call); the rest are debug lines.
    void logFailure(const std::string& what, sd_bus_message* reply) {
      static bool warned = false;
      const sd_bus_error* error = sd_bus_message_get_error(reply);
      const char* message = error != nullptr && error->message != nullptr ? error->message : "?";
      if (!std::exchange(warned, true)) {
        kLog.warn("{} failed: {}", what, message);
      } else {
        kLog.debug("{} failed: {}", what, message);
      }
    }

    // AllowedCPUs is a byte mask: bit n set = CPU n.
    std::vector<std::uint8_t> cpuMask(const std::vector<int>& cpus) {
      std::vector<std::uint8_t> mask;
      for (const int cpu : cpus) {
        const auto byte = static_cast<std::size_t>(cpu) / 8;
        if (mask.size() <= byte) {
          mask.resize(byte + 1, 0);
        }
        mask[byte] |= static_cast<std::uint8_t>(1U << (static_cast<unsigned>(cpu) % 8U));
      }
      return mask;
    }
    // sd_bus_open_user reads DBUS_SESSION_BUS_ADDRESS and XDG_RUNTIME_DIR with secure_getenv, which returns nothing
    // in a secure-exec process (the packaged cap_sys_nice binary); main() restored them into environ for getenv.
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
  } // namespace

  SystemdUnits::SystemdUnits(wl_event_loop* loop) {
    if (loop == nullptr || openUserBus(&m_bus) < 0) {
      kLog.info("no systemd user manager: app QoS and launch boost are off");
      m_bus = nullptr;
      return;
    }
    m_source = wl_event_loop_add_fd(loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    m_timer = wl_event_loop_add_timer(loop, onTimer, this);
    if (m_source == nullptr || m_timer == nullptr) {
      disconnect();
      return;
    }
    updateEvents();
  }

  SystemdUnits::~SystemdUnits() {
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
    }
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
    if (m_bus != nullptr) {
      sd_bus_flush_close_unref(m_bus);
    }
  }

  // The event loop polls its own copy of the bus fd: after a disconnect it reports a hangup on every turn.
  void SystemdUnits::disconnect() {
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
      m_source = nullptr;
    }
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
      m_timer = nullptr;
    }
    m_bus = sd_bus_close_unref(m_bus);
  }

  int SystemdUnits::onEvent(int /*fd*/, unsigned mask, void* data) {
    auto* self = static_cast<SystemdUnits*>(data);
    self->dispatch();
    // dispatch first: replies already buffered still reach their callers.
    if (self->m_bus != nullptr && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
      kLog.warn("user bus closed: QoS and the launch boost stop");
      self->disconnect();
    }
    return 0;
  }

  int SystemdUnits::onTimer(void* data) {
    static_cast<SystemdUnits*>(data)->dispatch();
    return 0;
  }

  void SystemdUnits::dispatch() {
    int result = 0;
    while ((result = sd_bus_process(m_bus, nullptr)) > 0) {
    }
    if (result < 0) {
      kLog.warn("user bus failed: QoS and the launch boost stop");
      disconnect();
      return;
    }
    updateEvents();
  }

  void SystemdUnits::updateEvents() {
    const int events = sd_bus_get_events(m_bus);
    wl_event_source_fd_update(
        m_source,
        WL_EVENT_READABLE | ((events > 0 && (events & POLLOUT) != 0) ? static_cast<uint32_t>(WL_EVENT_WRITABLE) : 0U)
    );
    // sd_bus_get_timeout: absolute CLOCK_MONOTONIC µs of the next call timeout, UINT64_MAX for none.
    std::uint64_t deadline = UINT64_MAX;
    int delayMs = 0;
    if (sd_bus_get_timeout(m_bus, &deadline) >= 0 && deadline != UINT64_MAX) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const auto nowUs = static_cast<std::uint64_t>(now.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000;
      delayMs = deadline <= nowUs ? 1 : static_cast<int>((deadline - nowUs + 999) / 1000);
    }
    wl_event_source_timer_update(m_timer, delayMs);
  }

  bool SystemdUnits::call(sd_bus_message* message, Reply reply) {
    auto* owned = new Reply(std::move(reply));
    sd_bus_slot* slot = nullptr;
    const auto onReply = [](sd_bus_message* answer, void* data, sd_bus_error* /*error*/) -> int {
      (*static_cast<Reply*>(data))(answer);
      return 0;
    };
    if (sd_bus_call_async(m_bus, &slot, message, onReply, owned, 0) < 0) {
      delete owned;
      return false;
    }
    // The bus owns the slot from here: it frees the callback after the reply, or when the bus closes.
    sd_bus_slot_set_destroy_callback(slot, [](void* data) { delete static_cast<Reply*>(data); });
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);
    updateEvents();
    return true;
  }

  void SystemdUnits::setProperties(const std::string& unit, const Properties& properties) {
    if (m_bus == nullptr || unit.empty()) {
      return;
    }
    sd_bus_message* message = nullptr;
    bool built = sd_bus_message_new_method_call(m_bus, &message, kService, kPath, kManager, "SetUnitProperties") >= 0
        && sd_bus_message_append(message, "sb", unit.c_str(), 1) >= 0
        && sd_bus_message_open_container(message, 'a', "(sv)") >= 0;
    if (built && properties.cpuWeight) {
      built = sd_bus_message_append(message, "(sv)", "CPUWeight", "t", *properties.cpuWeight) >= 0;
    }
    if (built && properties.ioWeight) {
      built = sd_bus_message_append(message, "(sv)", "IOWeight", "t", *properties.ioWeight) >= 0;
    }
    if (built && properties.memoryLow) {
      built = sd_bus_message_append(message, "(sv)", "MemoryLow", "t", *properties.memoryLow) >= 0;
    }
    if (built && properties.allowedCpus) {
      const std::vector<std::uint8_t> mask = cpuMask(*properties.allowedCpus);
      static constexpr std::uint8_t kNone = 0;
      built = sd_bus_message_open_container(message, 'r', "sv") >= 0
          && sd_bus_message_append(message, "s", "AllowedCPUs") >= 0
          && sd_bus_message_open_container(message, 'v', "ay") >= 0
          && sd_bus_message_append_array(message, 'y', mask.empty() ? &kNone : mask.data(), mask.size()) >= 0
          && sd_bus_message_close_container(message) >= 0
          && sd_bus_message_close_container(message) >= 0;
    }
    if (built && sd_bus_message_close_container(message) >= 0) {
      call(message, [unit](sd_bus_message* reply) {
        if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
          logFailure("SetUnitProperties " + unit, reply);
        }
      });
    }
    sd_bus_message_unref(message);
  }

  bool SystemdUnits::setFrozen(const std::string& unit, bool frozen, std::function<void(bool ok)> done) {
    if (m_bus == nullptr || unit.empty()) {
      return false;
    }
    bool sent = false;
    const char* method = frozen ? "FreezeUnit" : "ThawUnit";
    sd_bus_message* message = nullptr;
    if (sd_bus_message_new_method_call(m_bus, &message, kService, kPath, kManager, method) >= 0
        && sd_bus_message_append(message, "s", unit.c_str()) >= 0) {
      sent = call(message, [unit, method, done = std::move(done)](sd_bus_message* reply) {
        const bool ok = sd_bus_message_is_method_error(reply, nullptr) == 0;
        if (!ok) {
          logFailure(std::string(method) + " " + unit, reply);
        }
        if (done) {
          done(ok);
        }
      });
    }
    sd_bus_message_unref(message);
    return sent;
  }

  bool SystemdUnits::startScope(
      const std::string& unit, int pid, std::function<void(bool ok)> done, const ScopeOptions& options
  ) {
    if (m_bus == nullptr || unit.empty()) {
      return false;
    }
    sd_bus_message* message = nullptr;
    bool built = sd_bus_message_new_method_call(m_bus, &message, kService, kPath, kManager, "StartTransientUnit") >= 0
        && sd_bus_message_append(message, "ss", unit.c_str(), "fail") >= 0
        && sd_bus_message_open_container(message, 'a', "(sv)") >= 0
        && sd_bus_message_append(
               message, "(sv)(sv)(sv)", "PIDs", "au", 1, static_cast<std::uint32_t>(pid), "Slice", "s", "app.slice",
               "CollectMode", "s", "inactive-or-failed"
           ) >= 0;
    if (built && !options.description.empty()) {
      built = sd_bus_message_append(message, "(sv)", "Description", "s", options.description.c_str()) >= 0;
    }
    for (const auto& [name, units] : {std::pair{"PartOf", &options.partOf}, std::pair{"After", &options.after}}) {
      if (!built || units->empty()) {
        continue;
      }
      built = sd_bus_message_open_container(message, 'r', "sv") >= 0 && sd_bus_message_append(message, "s", name) >= 0
          && sd_bus_message_open_container(message, 'v', "as") >= 0
          && sd_bus_message_open_container(message, 'a', "s") >= 0;
      for (const std::string& dependency : *units) {
        built = built && sd_bus_message_append(message, "s", dependency.c_str()) >= 0;
      }
      built = built && sd_bus_message_close_container(message) >= 0 && sd_bus_message_close_container(message) >= 0
          && sd_bus_message_close_container(message) >= 0;
    }
    if (built && options.cpuWeight) {
      built = sd_bus_message_append(message, "(sv)", "CPUWeight", "t", *options.cpuWeight) >= 0;
    }
    built = built && sd_bus_message_close_container(message) >= 0 && sd_bus_message_append(message, "a(sa(sv))", 0) >= 0;
    if (built) {
      built = call(message, [unit, done = std::move(done)](sd_bus_message* reply) {
        const bool ok = sd_bus_message_is_method_error(reply, nullptr) == 0;
        if (!ok) {
          logFailure("StartTransientUnit " + unit, reply);
        }
        done(ok);
      });
    }
    sd_bus_message_unref(message);
    return built;
  }

  void SystemdUnits::listUnits(const std::vector<std::string>& patterns, std::function<void(const std::string&)> found) {
    if (m_bus == nullptr || patterns.empty()) {
      return;
    }
    sd_bus_message* message = nullptr;
    bool built = sd_bus_message_new_method_call(m_bus, &message, kService, kPath, kManager, "ListUnitsByPatterns") >= 0
        && sd_bus_message_append(message, "as", 0) >= 0
        && sd_bus_message_open_container(message, 'a', "s") >= 0;
    for (const std::string& pattern : patterns) {
      built = built && sd_bus_message_append(message, "s", pattern.c_str()) >= 0;
    }
    if (built && sd_bus_message_close_container(message) >= 0) {
      call(message, [found = std::move(found)](sd_bus_message* reply) {
        if (sd_bus_message_is_method_error(reply, nullptr) != 0
            || sd_bus_message_enter_container(reply, 'a', "(ssssssouso)") < 0) {
          return;
        }
        const char* name = nullptr;
        while (sd_bus_message_read(
                   reply, "(ssssssouso)", &name, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr, nullptr
               )
               > 0) {
          if (name != nullptr) {
            found(name);
          }
        }
      });
    }
    sd_bus_message_unref(message);
  }
#else
  SystemdUnits::SystemdUnits(wl_event_loop* /*loop*/) {}
  SystemdUnits::~SystemdUnits() = default;
  int SystemdUnits::onEvent(int /*fd*/, unsigned /*mask*/, void* /*data*/) { return 0; }
  void SystemdUnits::dispatch() {}
  void SystemdUnits::updateEvents() {}
  bool SystemdUnits::call(sd_bus_message* /*message*/, Reply /*reply*/) { return false; }
  void SystemdUnits::setProperties(const std::string& /*unit*/, const Properties& /*properties*/) {}
  int SystemdUnits::onTimer(void* /*data*/) { return 0; }
  void SystemdUnits::disconnect() {}
  bool SystemdUnits::setFrozen(const std::string& /*unit*/, bool /*frozen*/, std::function<void(bool)> /*done*/) {
    return false;
  }
  bool SystemdUnits::startScope(const std::string& /*unit*/, int /*pid*/, std::function<void(bool)> /*done*/, const ScopeOptions& /*options*/) {
    return false;
  }
  void SystemdUnits::listUnits(const std::vector<std::string>& /*patterns*/, std::function<void(const std::string&)>) {}
#endif

} // namespace andromeda

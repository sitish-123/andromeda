#include "core/package_energy.h"

#include <poll.h>
#include <wayland-server-core.h>

#include <cmath>
#include <cstdint>
#include <ctime>

#include <systemd/sd-bus.h>

namespace andromeda {

  namespace {

    std::uint64_t monotonicUs() {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000;
    }

    constexpr std::uint64_t kCallTimeoutUs = 5'000'000;

  } // namespace

  PackageEnergy::PackageEnergy(wl_event_loop* loop) : m_loop(loop) {}

  PackageEnergy::~PackageEnergy() { close(); }

  bool PackageEnergy::open() {
    if (m_bus != nullptr) {
      return true;
    }
    if (m_loop == nullptr || sd_bus_open_system(&m_bus) < 0) {
      m_bus = nullptr;
      return false;
    }
    m_source = wl_event_loop_add_fd(m_loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    m_timer = wl_event_loop_add_timer(m_loop, onTimer, this);
    if (m_source == nullptr || m_timer == nullptr) {
      close();
      return false;
    }
    return true;
  }

  void PackageEnergy::close() {
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

  void PackageEnergy::request(std::function<void(double)> done) {
    if (m_done) {
      return;
    }
    if (!open()) {
      done(NAN);
      return;
    }
    m_done = std::move(done);
    const int result = sd_bus_call_method_async(
        m_bus, nullptr, "org.andromeda.System1", "/org/andromeda/System1/Power", "org.andromeda.System1.Power",
        "PackageEnergyJ", onReply, this, ""
    );
    if (result < 0) {
      auto callback = std::move(m_done);
      m_done = nullptr;
      close();
      callback(NAN);
      return;
    }
    updateEvents();
  }

  int PackageEnergy::onReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<PackageEnergy*>(data);
    double joules = NAN;
    if (sd_bus_message_is_method_error(reply, nullptr) == 0 && sd_bus_message_read(reply, "d", &joules) < 0) {
      joules = NAN;
    }
    if (auto done = std::move(self->m_done); done) {
      self->m_done = nullptr;
      done(joules);
    }
    return 0;
  }

  int PackageEnergy::onEvent(int /*fd*/, std::uint32_t /*mask*/, void* data) {
    static_cast<PackageEnergy*>(data)->dispatch();
    return 0;
  }

  int PackageEnergy::onTimer(void* data) {
    static_cast<PackageEnergy*>(data)->dispatch();
    return 0;
  }

  void PackageEnergy::dispatch() {
    if (m_bus == nullptr) {
      return;
    }
    int result = 0;
    while ((result = sd_bus_process(m_bus, nullptr)) > 0) {
    }
    if (result < 0) {
      close();
      if (auto done = std::move(m_done); done) {
        m_done = nullptr;
        done(NAN);
      }
      return;
    }
    updateEvents();
  }

  void PackageEnergy::updateEvents() {
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
    } else if (m_done) {
      delayMs = static_cast<int>(kCallTimeoutUs / 1000);
    }
    wl_event_source_timer_update(m_timer, delayMs);
  }

} // namespace andromeda

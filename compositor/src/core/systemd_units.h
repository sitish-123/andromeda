#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct sd_bus;
struct sd_bus_message;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // The compositor's one client of the systemd user manager. Calls are asynchronous on the compositor's event loop,
  // so the compositor never waits on systemd. Without sd-bus or a user bus every call is a no-op.
  class SystemdUnits {
  public:
    // Runtime unit properties; an unset field is left as it is.
    struct Properties {
      std::optional<std::uint64_t> cpuWeight;
      std::optional<std::uint64_t> ioWeight;
      std::optional<std::vector<int>> allowedCpus; // empty = every CPU
    };

    explicit SystemdUnits(wl_event_loop* loop);
    ~SystemdUnits();
    SystemdUnits(const SystemdUnits&) = delete;
    SystemdUnits& operator=(const SystemdUnits&) = delete;

    [[nodiscard]] bool active() const { return m_bus != nullptr; }
    void setProperties(const std::string& unit, const Properties& properties);
    // `done` gets true once systemd reports the unit frozen or thawed; it never runs after the bus is gone.
    // Returns false when the call was not sent; `done` then never runs.
    bool setFrozen(const std::string& unit, bool frozen, std::function<void(bool ok)> done = {});
    // Moves `pid` into a new transient scope `unit` under app.slice. `done` gets true once systemd started it.
    void startScope(const std::string& unit, int pid, std::function<void(bool ok)> done);
    // Calls `found` once for each loaded unit whose name matches one of the patterns.
    void listUnits(const std::vector<std::string>& patterns, std::function<void(const std::string&)> found);

  private:
    using Reply = std::function<void(sd_bus_message*)>;
    bool call(sd_bus_message* message, Reply reply);
    void dispatch();
    void updateEvents();
    void disconnect();
    static int onEvent(int fd, unsigned mask, void* data);
    static int onTimer(void* data);

    sd_bus* m_bus = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr; // fails calls that get no reply in time
  };

} // namespace andromeda

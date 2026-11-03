// Drives SystemdUnitsService against mock_systemd1.py on DBUS_SESSION_BUS_ADDRESS: lists the running units with
// their details, then starts and stops a unit through the mock and waits for the live list to follow.
// Prints one line per check and exits non-zero on any failure.
#include "dbus/session_bus.h"
#include "dbus/systemd/systemd_units_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>

namespace {
  int g_failures = 0;
  void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
  }

  void pumpUntil(SessionBus& bus, const std::function<bool()>& done) {
    for (int i = 0; i < 300 && !done(); ++i) {
      bus.processPendingEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  const SystemdUnit* find(const SystemdUnitsService& service, const std::string& name) {
    const auto& units = service.units();
    const auto it = std::ranges::find(units, name, &SystemdUnit::name);
    return it == units.end() ? nullptr : &*it;
  }
} // namespace

int main() {
  SessionBus bus;
  int changes = 0;
  SystemdUnitsService service(bus.connection(), SystemdUnitsService::Manager::User, [&changes]() { ++changes; });

  service.refresh();
  pumpUntil(bus, [&]() { return changes > 0; });
  check(service.available(), "user manager answered (" + service.lastError() + ")");
  check(changes == 1, "one change for the whole list, got " + std::to_string(changes));
  check(service.units().size() == 6, "6 running services and scopes, got " + std::to_string(service.units().size()));
  check(find(service, "stopped.service") == nullptr, "inactive unit is not listed");
  check(find(service, "default.target") == nullptr, "targets are not listed");

  const SystemdUnit* opencode = find(service, "opencode.service");
  check(opencode != nullptr, "opencode.service listed");
  if (opencode != nullptr) {
    check(opencode->slice == "app.slice", "opencode slice " + opencode->slice);
    check(opencode->controlGroup.ends_with("/app.slice/opencode.service"), "opencode cgroup " + opencode->controlGroup);
    check(opencode->fragmentPath.ends_with("/.config/systemd/user/opencode.service"), "opencode fragment " + opencode->fragmentPath);
    check(opencode->unitFileState == "enabled" && opencode->unitFilePreset == "disabled", "opencode enabled, preset disabled");
    check(opencode->activeEnterUsec > 0, "opencode start time read");
    check(opencode->appId.empty() && !opencode->systemManager, "opencode is a plain user service");
  }
  const SystemdUnit* zen = find(service, "app-zen\\x2dbrowser@0f1e2d3c.service");
  check(zen != nullptr && zen->appId == "zen-browser", "app unit maps to zen-browser");
  const SystemdUnit* kded = find(service, "dbus-:1.2-org.kde.kded6@0.service");
  check(kded != nullptr && kded->appId == "org.kde.kded6", "D-Bus activated unit maps to org.kde.kded6");
  const SystemdUnit* pipewire = find(service, "pipewire.service");
  check(pipewire != nullptr && pipewire->slice == "session.slice", "pipewire in session.slice");

  service.setLive(true);
  auto control = sdbus::createProxy(
      bus.connection(), sdbus::ServiceName{"org.freedesktop.systemd1"}, sdbus::ObjectPath{"/org/freedesktop/systemd1"}
  );
  control->callMethod("Start").onInterface("org.andromeda.MockSystemd").withArguments(std::string("sleep-test.service"), std::string("background.slice"));
  pumpUntil(bus, [&]() {
    const SystemdUnit* unit = find(service, "sleep-test.service");
    return unit != nullptr && !unit->slice.empty();
  });
  const SystemdUnit* started = find(service, "sleep-test.service");
  check(started != nullptr && started->slice == "background.slice", "live: started unit appears with its slice");

  control->callMethod("Stop").onInterface("org.andromeda.MockSystemd").withArguments(std::string("sleep-test.service"));
  pumpUntil(bus, [&]() { return find(service, "sleep-test.service") == nullptr; });
  check(find(service, "sleep-test.service") == nullptr, "live: stopped unit disappears");
  check(service.units().size() == 6, "live: back to 6 units");

  service.setLive(false);
  // Unsubscribe is asynchronous: give it a few turns to reach the mock before exit.
  int turns = 0;
  pumpUntil(bus, [&turns]() { return ++turns > 20; });
  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

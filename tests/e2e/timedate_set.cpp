// Drives TimeDateService against the mock org.freedesktop.timedate1 on DBUS_SYSTEM_BUS_ADDRESS: sets a timezone
// and waits for it to come back as the effective value. Prints one line per step and exits non-zero on mismatch.
#include "dbus/system_bus.h"
#include "dbus/timedate/timedate_service.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {
  int g_failures = 0;
  void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
  }
} // namespace

int main() {
  SystemBus bus;
  bool changed = false;
  TimeDateService timedate(bus, [&changed]() { changed = true; });

  for (int i = 0; i < 200 && !timedate.ready(); ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(timedate.ready(), "timedate1 reachable");

  timedate.setTimezone("Europe/Berlin");
  for (int i = 0; i < 200 && timedate.timezone() != "Europe/Berlin"; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(timedate.timezone() == "Europe/Berlin", "effective timezone is Europe/Berlin");
  check(timedate.lastError().empty(), "no failure (" + timedate.lastError() + ")");

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

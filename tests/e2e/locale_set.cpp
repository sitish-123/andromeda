// Drives LocaleService against the mock org.freedesktop.locale1 on DBUS_SYSTEM_BUS_ADDRESS: sets a keyboard layout
// and waits for it to come back as the effective value. Prints one line per step and exits non-zero on mismatch.
#include "dbus/locale/locale_service.h"
#include "dbus/system_bus.h"

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
  LocaleService locale(bus, [&changed]() { changed = true; });

  for (int i = 0; i < 200 && !locale.ready(); ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(locale.ready(), "locale1 reachable");

  locale.setX11Keyboard("de", "", "nodeadkeys", "", true);
  for (int i = 0; i < 200 && locale.x11Layout() != "de"; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(locale.x11Layout() == "de", "effective X11Layout is de");
  check(locale.x11Variant() == "nodeadkeys", "effective X11Variant is nodeadkeys");
  check(locale.lastError().empty(), "no failure (" + locale.lastError() + ")");

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

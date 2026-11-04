// Drives ActivityActions against mock_systemd1.py on DBUS_SESSION_BUS_ADDRESS (the user manager): Disable at login
// for a plain unit and a socket-activated one, Enable (undo), Stop, and the XDG autostart override in a temp dir.
// argv[1] is a scratch directory. Prints one line per check and exits non-zero on any failure.
#include "dbus/session_bus.h"
#include "dbus/systemd/systemd_units_service.h"
#include "system/activity_actions.h"
#include "system/xdg_autostart.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

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

  std::string readAll(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return 2;
  }
  const fs::path scratch = argv[1];
  SessionBus bus;
  bool listed = false;
  SystemdUnitsService units(bus.connection(), SystemdUnitsService::Manager::User, [&listed]() { listed = true; });
  units.refresh();
  pumpUntil(bus, [&]() { return listed; });
  const auto unitNamed = [&units](const std::string& name) -> std::optional<SystemdUnit> {
    const auto it = std::ranges::find(units.units(), name, &SystemdUnit::name);
    return it == units.units().end() ? std::nullopt : std::optional(*it);
  };

  ActivityActions actions(bus.connection(), nullptr);
  const auto run = [&](const std::function<void(ActivityActions::Reply)>& start) {
    std::optional<ActivityActionResult> result;
    start([&result](ActivityActionResult r) { result = std::move(r); });
    pumpUntil(bus, [&]() { return result.has_value(); });
    return result.value_or(ActivityActionResult{.error = "no reply"});
  };

  const auto opencode = unitNamed("opencode.service");
  check(opencode.has_value(), "opencode.service listed");
  auto r = run([&](auto reply) { actions.disableAtLogin(*opencode, {}, {}, reply); });
  check(r.ok && !r.stillStartsOnDemand, "disable opencode: ok, nothing starts it on demand (" + r.error + ")");
  r = run([&](auto reply) { actions.enableAtLogin(*opencode, {}, {}, reply); });
  check(r.ok, "undo: enable opencode (" + r.error + ")");

  const auto socketed = unitNamed("socketed.service");
  r = run([&](auto reply) { actions.disableAtLogin(*socketed, {}, {}, reply); });
  check(r.ok && r.stillStartsOnDemand, "disable socketed: a socket still starts it on demand");

  r = run([&](auto reply) { actions.setFrozen(*opencode, true, reply); });
  check(r.ok, "pause opencode (" + r.error + ")");
  r = run([&](auto reply) { actions.setFrozen(*opencode, false, reply); });
  check(r.ok, "resume opencode (" + r.error + ")");

  r = run([&](auto reply) { actions.setLimits(*opencode, 2'000'000, 512ULL << 20, reply); });
  check(r.ok, "limit opencode (" + r.error + ")");
  r = run([&](auto reply) { actions.setLimits(*opencode, ActivityActions::kNoLimit, ActivityActions::kNoLimit, reply); });
  check(r.ok, "remove opencode limits (" + r.error + ")");

  r = run([&](auto reply) { actions.stop(*opencode, reply); });
  check(r.ok, "stop opencode (" + r.error + ")");

  // XDG autostart: a system entry gets a marked override; undo deletes it. The user's own entry keeps its text.
  const AutostartDirs dirs{.user = scratch / "user", .system = {scratch / "etc"}};
  fs::create_directories(dirs.system.front());
  fs::create_directories(dirs.user);
  std::ofstream(scratch / "etc/blueman.desktop") << "[Desktop Entry]\nName=Blueman Applet\nExec=blueman-applet\n";
  std::ofstream(scratch / "user/mine.desktop") << "[Desktop Entry]\nName=Mine\nExec=mine --flag\n";
  auto entries = scanAutostart(dirs, "Andromeda");
  SystemdUnit blueman;
  blueman.name = "app-blueman@autostart.service";
  r = run([&](auto reply) { actions.disableAtLogin(blueman, entries, dirs.user, reply); });
  check(r.ok, "disable blueman autostart (" + r.error + ")");
  entries = scanAutostart(dirs, "Andromeda");
  const auto findEntry = [&entries](const std::string& id) { return *std::ranges::find(entries, id, &AutostartEntry::id); };
  check(!findEntry("blueman").startsHere && findEntry("blueman").userFile, "blueman override hides it at login");
  r = run([&](auto reply) { actions.enableAtLogin(blueman, entries, dirs.user, reply); });
  check(r.ok && !fs::exists(scratch / "user/blueman.desktop"), "undo deletes the override");

  SystemdUnit mine;
  mine.name = "app-mine@autostart.service";
  r = run([&](auto reply) { actions.disableAtLogin(mine, entries, dirs.user, reply); });
  const std::string hiddenText = readAll(scratch / "user/mine.desktop");
  check(r.ok && hiddenText.contains("Hidden=true") && hiddenText.contains("Exec=mine --flag"), "user entry gains Hidden=true, keeps Exec");
  entries = scanAutostart(dirs, "Andromeda");
  r = run([&](auto reply) { actions.enableAtLogin(mine, entries, dirs.user, reply); });
  const std::string restored = readAll(scratch / "user/mine.desktop");
  check(r.ok && !restored.contains("Hidden=") && restored.contains("Exec=mine --flag"), "undo removes only Hidden=");

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

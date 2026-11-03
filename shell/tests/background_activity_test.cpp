#include "system/background_activity.h"
#include "tests/test_check.h"

#include <string>

namespace {

  constexpr std::uint64_t kMb = 1024 * 1024;
  constexpr std::uint64_t kHourUsec = 3600ULL * 1'000'000;
  constexpr std::uint64_t kNow = 1'762'200'000ULL * 1'000'000;

  ActivityRow row(const std::string& unit, const std::string& appId, std::uint64_t memory, double cpuAvgPercent, std::uint64_t ageUsec) {
    ActivityRow r;
    r.key = appId.empty() ? unit : appId;
    r.appId = appId;
    r.units = {unit};
    r.slice = UnitSlice::App;
    r.memoryBytes = memory;
    r.startedUsec = kNow - ageUsec;
    r.cpuUsageUsec = static_cast<std::uint64_t>(cpuAvgPercent / 100.0 * 4 * static_cast<double>(ageUsec));
    return r;
  }

  BackgroundContext context() {
    BackgroundContext c;
    c.nowUsec = kNow;
    c.cpuCount = 4;
    c.homeDir = "/home/u";
    c.enabledFeatures = {ShellFeature::Bluetooth, ShellFeature::Plasma};
    return c;
  }

} // namespace

int main() {
  const BackgroundContext ctx = context();
  using Finding = BackgroundVerdict::Finding;

  // The trigger case: opencode, 3 h, 480 MB, 1.6 % CPU, enabled from ~/.config.
  auto opencode = row("opencode.service", "", 480 * kMb, 1.6, 3 * kHourUsec);
  auto v = classifyBackground(opencode, "/home/u/.config/systemd/user/opencode.service", "disabled", ctx);
  TEST_CHECK(v.background && v.finding == Finding::UsesResources && v.notify);
  TEST_CHECK(v.uptimeSec == 3 * 3600 && v.cpuAveragePercent > 1.59 && v.cpuAveragePercent < 1.61);

  // CPU alone crosses the line; small and young or quiet does not.
  TEST_CHECK(classifyBackground(row("busy.service", "", 20 * kMb, 2.0, kHourUsec), "/etc/systemd/user/busy.service", "", ctx).finding == Finding::UsesResources);
  TEST_CHECK(classifyBackground(row("young.service", "", 900 * kMb, 5.0, kHourUsec / 4), "/etc/x", "", ctx).finding == Finding::None);
  const auto quiet = classifyBackground(row("quiet.service", "", 20 * kMb, 0.1, 5 * kHourUsec), "/etc/x", "", ctx);
  TEST_CHECK(quiet.background && quiet.finding == Finding::None);

  // Session plumbing never flags, whatever it costs.
  for (const char* unit : {"pipewire.service", "andromeda.service", "andromeda-index.service", "noctalia.service",
                           "xdg-desktop-portal.service", "wireplumber.service", "dbus-broker.service"}) {
    TEST_CHECK(classifyBackground(row(unit, "", 900 * kMb, 9.0, 5 * kHourUsec), "/usr/lib/systemd/user/x", "enabled", ctx).finding == Finding::None);
  }
  auto sessionSlice = row("gnome-thing.service", "", 900 * kMb, 9.0, 5 * kHourUsec);
  sessionSlice.slice = UnitSlice::Session;
  TEST_CHECK(classifyBackground(sessionSlice, "/usr/x", "enabled", ctx).finding == Finding::None);

  // A window, a tray icon or a terminal means the user attends it.
  auto windowed = opencode;
  windowed.hasWindow = true;
  TEST_CHECK(classifyBackground(windowed, "/home/u/.config/x", "", ctx).finding == Finding::None);
  BackgroundContext trayCtx = ctx;
  trayCtx.trayUnits.insert("opencode.service");
  TEST_CHECK(classifyBackground(opencode, "/home/u/.config/x", "", trayCtx).finding == Finding::None);
  BackgroundContext ttyCtx = ctx;
  ttyCtx.hasTerminal = [](const ActivityRow&) { return true; };
  TEST_CHECK(classifyBackground(opencode, "/home/u/.config/x", "", ttyCtx).finding == Finding::None);

  // Vendor-enabled units from /usr are listed but never notify.
  const auto vendor = classifyBackground(row("vendord.service", "", 480 * kMb, 0.0, 3 * kHourUsec), "/usr/lib/systemd/user/vendord.service", "enabled", ctx);
  TEST_CHECK(vendor.finding == Finding::UsesResources && !vendor.notify);

  // Duplicates flag at any cost, only while the shell feature is on; autostart entries notify.
  const auto blueman = classifyBackground(
      row("app-blueman@autostart.service", "blueman", 40 * kMb, 0.0, kHourUsec / 60),
      "/run/user/1000/systemd/generator.late/app-blueman@autostart.service", "", ctx
  );
  TEST_CHECK(blueman.finding == Finding::Duplicate && blueman.duplicates == ShellFeature::Bluetooth && blueman.notify);
  TEST_CHECK(classifyBackground(row("dunst.service", "", 40 * kMb, 0.0, kHourUsec), "/usr/x", "", ctx).finding == Finding::None);
  TEST_CHECK(classifyBackground(row("dbus-:1.2-org.kde.kded6@0.service", "org.kde.kded6", 10 * kMb, 0.0, kHourUsec), "/usr/x", "", ctx).duplicates == ShellFeature::Plasma);
  return 0;
}

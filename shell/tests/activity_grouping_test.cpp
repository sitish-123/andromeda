#include "system/activity_service.h"
#include "tests/test_check.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

  const std::string kUser = "/user.slice/user-1000.slice/user@1000.service";

  void writeCgroup(const fs::path& root, const std::string& group, std::uint64_t cpuUsec, std::uint64_t memory, double memPressure = 0.0) {
    const fs::path dir = root / group.substr(1);
    fs::create_directories(dir);
    std::ofstream(dir / "cpu.stat") << "usage_usec " << cpuUsec << "\nuser_usec 1\nsystem_usec 2\n";
    std::ofstream(dir / "memory.current") << memory << "\n";
    std::ofstream(dir / "io.stat") << "259:0 rbytes=" << cpuUsec << " wbytes=10 rios=1 wios=1 dbytes=0 dios=0\n";
    std::ofstream(dir / "memory.pressure") << "some avg10=" << memPressure << " avg60=0.00 avg300=0.00 total=1\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
  }

  SystemdUnit unit(const std::string& name, const std::string& appId, const std::string& slice, std::uint64_t started) {
    SystemdUnit u;
    u.name = name;
    u.appId = appId;
    u.slice = slice;
    u.controlGroup = kUser + "/" + slice + "/" + name;
    u.activeEnterUsec = started;
    return u;
  }

  const ActivityRow* row(const ActivityService& service, const std::string& key) {
    for (const auto& r : service.rows()) {
      if (r.key == key) {
        return &r;
      }
    }
    return nullptr;
  }

} // namespace

int main() {
  TEST_CHECK(unitFromProcCgroup("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-foot@x.service/sub\n") == "app-foot@x.service");
  TEST_CHECK(unitFromProcCgroup("0::/user.slice/user-1000.slice/session-2.scope\n") == "session-2.scope");
  TEST_CHECK(unitFromProcCgroup("0::/\n").empty());

  char tmpl[] = "/tmp/activity-grouping-test-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const std::uint64_t mb = 1024 * 1024;

  std::vector<SystemdUnit> units{
      unit("app-zen\\x2dbrowser@a.service", "zen-browser", "app.slice", 300),
      unit("app-zen\\x2dbrowser@b.service", "zen-browser", "app.slice", 200),
      unit("opencode.service", "", "app.slice", 100),
      unit("app-andromeda-1835-17.scope", "", "app.slice", 400),
      unit("pipewire.service", "", "session.slice", 50),
  };
  for (const auto& u : units) {
    writeCgroup(root, u.controlGroup, 0, 10 * mb);
  }
  writeCgroup(root, units[2].controlGroup, 0, 480 * mb, 4.0);

  ActivityService service(root.string(), 4);
  service.setUnits(units);
  service.setWindowUnits({{"app-zen\\x2dbrowser@a.service", "zen-browser"}, {"app-andromeda-1835-17.scope", "foot"}});
  const auto t0 = std::chrono::steady_clock::now();
  service.sample(t0);

  TEST_CHECK(service.rows().size() == 4);
  const ActivityRow* zen = row(service, "zen-browser");
  TEST_CHECK(zen != nullptr && zen->units.size() == 2 && zen->hasWindow && zen->memoryBytes == 20 * mb);
  TEST_CHECK(zen->startedUsec == 200);
  const ActivityRow* foot = row(service, "foot");
  TEST_CHECK(foot != nullptr && foot->appId == "foot" && foot->hasWindow && foot->units.front() == "app-andromeda-1835-17.scope");
  const ActivityRow* opencode = row(service, "opencode.service");
  TEST_CHECK(opencode != nullptr && !opencode->hasWindow && opencode->appId.empty());
  TEST_CHECK(std::abs(opencode->memoryPressure - 4.0) < 1e-9);
  TEST_CHECK(row(service, "pipewire.service")->slice == UnitSlice::Session);
  TEST_CHECK(service.rows().front().key == "opencode.service"); // largest memory first
  TEST_CHECK(opencode->cpuPercent == 0.0); // one sample gives no rate

  // 1 s of CPU in 5 s on 4 CPUs = 5 % of the machine.
  writeCgroup(root, units[2].controlGroup, 1'000'000, 480 * mb, 4.0);
  service.sample(t0 + std::chrono::seconds(5));
  opencode = row(service, "opencode.service");
  TEST_CHECK(std::abs(opencode->cpuPercent - 5.0) < 1e-6);
  TEST_CHECK(std::abs(opencode->ioReadBytesPerSec - 200'000.0) < 1e-6);

  // A stopped unit leaves its row.
  units.erase(units.begin() + 2);
  service.setUnits(units);
  TEST_CHECK(row(service, "opencode.service") == nullptr);

  fs::remove_all(root);
  return 0;
}

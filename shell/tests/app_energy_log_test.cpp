// Settings > Battery Wh per app: the uncore (GPU) energy goes to apps by their share of all GPU time, the rest of the
// package by CPU time; GPU time of non-app processes counts in the denominator only; the text form keeps the counters.

#include "protocols/app_energy_log.h"
#include "tests/test_check.h"

#include <cmath>

namespace {
  bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
}

int main() {
  const std::int64_t t0 = 3600 * 1000;
  AppEnergyLog log;
  log.bookTotal(0, t0);
  log.bookPackage(0.0, t0);
  log.bookUncore(0.0, t0);
  log.book("app-editor.scope", 0, t0);
  log.book("app-game.scope", 0, t0);
  log.bookGpu("pdev/1", "app-game.scope", 0, t0);
  log.bookGpu("pdev/2", "", 0, t0);

  const std::int64_t t1 = t0 + 600;
  log.bookTotal(100'000'000, t1);                  // 100 CPU s in all
  log.book("app-editor.scope", 50'000'000, t1);    // 50 CPU s
  log.book("app-game.scope", 10'000'000, t1);      // 10 CPU s
  log.bookGpu("pdev/1", "app-game.scope", 30'000'000'000, t1); // 30 GPU s
  log.bookGpu("pdev/2", "", 10'000'000'000, t1);   // the compositor: 10 GPU s
  log.bookPackage(3600.0, t1);                     // 1 Wh in all
  log.bookUncore(1800.0, t1);                      // half of it the GPU

  const auto wh = log.wattHours(t1, 3600);
  // CPU part 0.5 Wh: editor 50 %, game 10 %. GPU part 0.5 Wh: game 30 of 40 GPU s.
  TEST_CHECK(near(wh.at("app-editor.scope"), 0.25));
  TEST_CHECK(near(wh.at("app-game.scope"), 0.05 + 0.375));

  AppEnergyLog again;
  again.parse(log.serialize(t1));
  TEST_CHECK(near(again.wattHours(t1, 3600).at("app-game.scope"), 0.425));
  again.bookGpu("pdev/1", "app-game.scope", 30'000'000'000, t1 + 60);
  TEST_CHECK(near(again.wattHours(t1 + 60, 3600).at("app-game.scope"), 0.425)); // no growth, no new GPU time

  // A launch scope's name holds no app id: the log keeps the one its windows gave, across a restart.
  AppEnergyLog launched;
  launched.book("app-andromeda-1363-14.scope", 0, t0);
  launched.book("app-andromeda-1363-14.scope", 20'000'000, t1);
  launched.name("app-andromeda-1363-14.scope", "vivaldi-stable");
  AppEnergyLog restarted;
  restarted.parse(launched.serialize(t1));
  TEST_CHECK(near(restarted.cpuSeconds(t1, 3600).at("vivaldi-stable"), 20.0));
  TEST_CHECK(!restarted.cpuSeconds(t1, 3600).contains("app-andromeda-1363-14.scope"));
  return 0;
}

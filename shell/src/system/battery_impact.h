#pragma once

#include "system/activity_details.h"
#include "system/activity_service.h"

#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

// Display name and icon of an Activity row: its desktop entry, else the app id, else the unit name.
struct RowIdentity {
  std::string name;
  std::string icon;
};
[[nodiscard]] RowIdentity identityFor(const ActivityRow& row);

// Battery impact (an estimate, not measured energy): each row's share of all CPU time its units used since they
// started, plus 2x its GPU share now. Rows below 0.5 % are left out; largest first. Activity > Battery and
// Settings > Battery use this one ranking.
struct BatteryImpact {
  const ActivityRow* row = nullptr;
  double sharePercent = 0.0;
};
// The 10-day ranking: each app's share of the CPU seconds the compositor booked per unit (protocols/app_energy_log.h).
// Units of one app add up; an app that runs now keeps its row, one that does not gets a row in `past`, which must
// outlive the result. Rows below 0.5 % are left out; largest first.
[[nodiscard]] std::vector<BatteryImpact> rankAppEnergy(
    const std::map<std::string, double>& unitSeconds, std::span<const ActivityRow> rows, std::vector<ActivityRow>& past
);

[[nodiscard]] std::vector<BatteryImpact> rankBatteryImpact(
    std::span<const ActivityRow> rows, const ActivityDetails* details,
    const std::function<bool(const ActivityRow&)>& keep = nullptr
);

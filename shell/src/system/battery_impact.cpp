#include "system/battery_impact.h"

#include "dbus/systemd/systemd_unit_name.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"

#include <algorithm>

RowIdentity identityFor(const ActivityRow& row) {
  if (!row.appId.empty()) {
    if (auto entry = app_identity::findDesktopEntry(row.appId, desktopEntries()); entry.has_value()) {
      return {.name = entry->name.empty() ? row.appId : entry->name, .icon = entry->icon};
    }
    return {.name = row.appId, .icon = row.appId};
  }
  std::string name = row.key;
  if (const auto dot = name.rfind('.'); dot != std::string::npos) {
    name.resize(dot);
  }
  return {.name = std::move(name), .icon = {}};
}

std::vector<BatteryImpact> rankBatteryImpact(
    std::span<const ActivityRow> rows, const ActivityDetails* details, const std::function<bool(const ActivityRow&)>& keep
) {
  std::vector<std::pair<double, const ActivityRow*>> ranked;
  double total = 0.0;
  for (const auto& row : rows) {
    if (keep && !keep(row)) {
      continue;
    }
    const double weight =
        static_cast<double>(row.cpuUsageUsec) / 1e6 + (details != nullptr ? details->gpuPercent(row.key) * 2.0 : 0.0);
    total += weight;
    ranked.emplace_back(weight, &row);
  }
  std::ranges::sort(ranked, std::greater{}, &std::pair<double, const ActivityRow*>::first);
  std::vector<BatteryImpact> out;
  for (const auto& [weight, row] : ranked) {
    const double share = total > 0.0 ? weight / total * 100.0 : 0.0;
    if (share < 0.5) {
      break;
    }
    out.push_back({.row = row, .sharePercent = share});
  }
  return out;
}

std::vector<BatteryImpact> rankAppEnergy(
    const std::map<std::string, double>& unitSeconds, std::span<const ActivityRow> rows, std::vector<ActivityRow>& past
) {
  std::map<std::string, double> byApp;
  double total = 0.0;
  for (const auto& [unit, seconds] : unitSeconds) {
    const std::string appId = appIdFromUnit(unit);
    byApp[appId.empty() ? unit : appId] += seconds;
    total += seconds;
  }
  past.clear();
  past.reserve(byApp.size());
  std::vector<BatteryImpact> out;
  for (const auto& [key, seconds] : byApp) {
    const double share = total > 0.0 ? seconds / total * 100.0 : 0.0;
    if (share < 0.5) {
      continue;
    }
    const auto live = std::ranges::find_if(rows, [&key](const ActivityRow& r) { return r.key == key || r.appId == key; });
    if (live != rows.end()) {
      out.push_back({.row = &*live, .sharePercent = share});
      continue;
    }
    const bool isUnit = key.ends_with(".scope") || key.ends_with(".service");
    past.push_back(ActivityRow{.key = key, .appId = isUnit ? std::string() : key, .units = {}});
    out.push_back({.row = &past.back(), .sharePercent = share});
  }
  std::ranges::sort(out, std::greater{}, &BatteryImpact::sharePercent);
  return out;
}

#include "shell/session/power_inhibitors.h"

#include "core/log.h"

#include <sdbus-c++/sdbus-c++.h>

#include <tuple>

namespace {

  constexpr Logger kLog("power");

  bool listsWhat(std::string_view list, std::string_view what) {
    std::size_t start = 0;
    while (start <= list.size()) {
      const std::size_t end = list.find(':', start);
      const std::string_view item = list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      if (item == what) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

} // namespace

bool powerActionBlocked(const std::vector<LogindInhibitor>& inhibitors, std::string_view what, std::uint32_t ownPid) {
  for (const LogindInhibitor& inhibitor : inhibitors) {
    if (inhibitor.pid != ownPid && inhibitor.mode != "delay" && listsWhat(inhibitor.what, what)) {
      return true;
    }
  }
  return false;
}

std::vector<LogindInhibitor> listLogindInhibitors() {
  std::vector<LogindInhibitor> out;
  try {
    const auto connection = sdbus::createSystemBusConnection();
    const auto login = sdbus::createProxy(
        *connection, sdbus::ServiceName{"org.andromeda.System1"}, sdbus::ObjectPath{"/org/andromeda/System1/Login"}
    );
    std::vector<sdbus::Struct<std::string, std::string, std::string, std::string, std::uint32_t, std::uint32_t>> reply;
    login->callMethod("ListInhibitors").onInterface("org.andromeda.System1.Login").storeResultsTo(reply);
    for (const auto& row : reply) {
      out.push_back({.what = std::get<0>(row), .who = std::get<1>(row), .why = std::get<2>(row), .mode = std::get<3>(row),
                     .uid = std::get<4>(row), .pid = std::get<5>(row)});
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("logind ListInhibitors failed: {}", e.what());
  }
  return out;
}

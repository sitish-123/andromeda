#pragma once

#include "shell/lockscreen/lock_notice_groups.h"

#include <chrono>
#include <cstdint>
#include <sdbus-c++/sdbus-c++.h>
#include <vector>

// org.andromeda.Shell1.LockNotices, D-Bus type a(ssix): app, icon, count, newest time in Unix ms (0 = unknown).
namespace lock_notice_wire {

  using Group = sdbus::Struct<std::string, std::string, std::int32_t, std::int64_t>;
  inline constexpr auto kProperty = "LockNotices";

  [[nodiscard]] inline std::vector<Group> encode(const std::vector<LockNoticeGroup>& groups) {
    std::vector<Group> out;
    out.reserve(groups.size());
    for (const LockNoticeGroup& g : groups) {
      const std::int64_t ms =
          g.newest.has_value()
              ? std::chrono::duration_cast<std::chrono::milliseconds>(g.newest->time_since_epoch()).count()
              : 0;
      out.emplace_back(g.app, g.icon, static_cast<std::int32_t>(g.count), ms);
    }
    return out;
  }

  [[nodiscard]] inline std::vector<LockNoticeGroup> decode(const std::vector<Group>& wire) {
    std::vector<LockNoticeGroup> out;
    out.reserve(wire.size());
    for (const Group& w : wire) {
      LockNoticeGroup g{.app = w.get<0>(), .icon = w.get<1>(), .count = w.get<2>()};
      if (w.get<3>() > 0) {
        g.newest = WallTimePoint{std::chrono::duration_cast<WallClock::duration>(std::chrono::milliseconds{w.get<3>()})};
      }
      out.push_back(std::move(g));
    }
    return out;
  }

} // namespace lock_notice_wire

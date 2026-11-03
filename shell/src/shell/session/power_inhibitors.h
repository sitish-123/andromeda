#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// One logind inhibitor lock (org.freedesktop.login1.Manager.ListInhibitors, a(ssssuu)).
struct LogindInhibitor {
  // Colon-separated: shutdown, sleep, idle, handle-power-key, ...
  std::string what;
  std::string who;
  std::string why;
  // block, block-weak or delay
  std::string mode;
  std::uint32_t uid = 0;
  std::uint32_t pid = 0;
};

// Whether an inhibitor of another process blocks `what` ("shutdown" or "sleep"); delay locks never block.
[[nodiscard]] bool powerActionBlocked(const std::vector<LogindInhibitor>& inhibitors, std::string_view what,
                                      std::uint32_t ownPid);

// Reads the inhibitors over a private system bus connection; empty when logind is unreachable.
[[nodiscard]] std::vector<LogindInhibitor> listLogindInhibitors();

#pragma once

#include <string_view>

namespace andromeda {

  // `andromeda debug crashes|log|report ...`, without a running compositor. `argv[0]` is "debug"; `selfVersion` is
  // this binary's version. Exit codes: 0 ok, 1 failed (report: a failure was found), 2 usage.
  int runDebug(int argc, char** argv, std::string_view selfVersion);

} // namespace andromeda

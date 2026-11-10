#pragma once

#include <string>
#include <string_view>

namespace greeter::passwordless_sync {

  // Implements the conventional-distribution administrator CLI. It manages a
  // dedicated site-local Polkit rule and never edits administrator-authored rules.
  [[nodiscard]] int runCommand(int argc, char* argv[]);

  namespace detail {
    // True when `contents` is a rule this CLI wrote, with the current or the pre-rename action id.
    [[nodiscard]] bool isManagedRule(std::string_view contents, std::string& errorOut);
  } // namespace detail

} // namespace greeter::passwordless_sync

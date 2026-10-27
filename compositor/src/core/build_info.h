#pragma once

#include <string_view>

namespace andromeda::build_info {

  // build_info.cpp is the only unit that sees the generated revision header, so
  // a new commit recompiles it alone.
  [[nodiscard]] std::string_view version() noexcept;

  [[nodiscard]] std::string_view revision() noexcept;

} // namespace andromeda::build_info

#include "core/build_info.h"

#include "andromeda_git_revision.h"

#ifndef ANDROMEDA_VERSION
#define ANDROMEDA_VERSION "unknown"
#endif

namespace andromeda::build_info {

  std::string_view version() noexcept { return ANDROMEDA_VERSION; }

  std::string_view revision() noexcept { return ANDROMEDA_GIT_REVISION; }

} // namespace andromeda::build_info

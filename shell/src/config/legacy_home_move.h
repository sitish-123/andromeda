#pragma once

#include <cstdint>
#include <filesystem>

namespace noctalia::config {

  enum class LegacyHomeMove : std::uint8_t { Nothing, Moved, DroppedLegacy, KeptTarget, Failed };

  // What to do with a legacy dir when the target exists too.
  enum class LegacyContent : std::uint8_t { Keep, Disposable };

  // Moves `legacy` to `target` when only `legacy` exists (a copy across filesystems), then leaves the link
  // legacy -> target. A legacy link to a dir moves as a link. When both exist, a Disposable legacy dir is
  // removed and linked; a Keep one is left alone. Holds an flock on <target parent>/.shell-move.lock.
  [[nodiscard]] LegacyHomeMove moveLegacyHome(
      const std::filesystem::path& legacy, const std::filesystem::path& target,
      LegacyContent content = LegacyContent::Keep
  );

  // The one-time move of the shell's config, state, data and cache roots from <base>/noctalia to <base>/andromeda/shell.
  void moveLegacyHomes();

} // namespace noctalia::config

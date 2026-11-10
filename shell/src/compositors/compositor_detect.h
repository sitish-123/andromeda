#pragma once

#include <cstdint>
#include <string_view>

namespace compositors {

  enum class CompositorKind : std::uint8_t {
    Unknown = 0,
    Andromeda = 8,
  };

  // Detected once per process from env vars. Cached after the first call.
  [[nodiscard]] CompositorKind detect();

  // Human-readable name (e.g. for diagnostics / hardware info). "Unknown" for unrecognized.
  [[nodiscard]] std::string_view name(CompositorKind kind);

  // Concatenated XDG_CURRENT_DESKTOP:XDG_SESSION_DESKTOP:DESKTOP_SESSION env hint for diagnostics.
  [[nodiscard]] std::string_view envHint();

  // Removed in D2b.
  [[nodiscard]] inline bool isNiri() { return false; }
  // Removed in D2b.
  [[nodiscard]] inline bool isHyprland() { return false; }
  // Removed in D2b.
  [[nodiscard]] inline bool isKde() { return false; }
  [[nodiscard]] inline bool isAndromeda() { return detect() == CompositorKind::Andromeda; }

} // namespace compositors

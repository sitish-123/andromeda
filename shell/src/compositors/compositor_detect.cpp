#include "compositors/compositor_detect.h"

#include "util/string_utils.h"

#include <cstdlib>
#include <string>

namespace compositors {

  namespace {

    [[nodiscard]] std::string buildEnvHint() {
      constexpr const char* vars[] = {"XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION"};
      std::string hint;
      for (const char* var : vars) {
        const char* value = std::getenv(var);
        if (value == nullptr || value[0] == '\0') {
          continue;
        }
        if (!hint.empty()) {
          hint += ':';
        }
        hint += value;
      }
      return hint;
    }

    [[nodiscard]] CompositorKind detectImpl() {
      if (const char* v = std::getenv("ANDROMEDA_SOCKET"); v != nullptr && v[0] != '\0') {
        return CompositorKind::Andromeda;
      }
      if (StringUtils::containsInsensitive(buildEnvHint(), "andromeda")) {
        return CompositorKind::Andromeda;
      }
      return CompositorKind::Unknown;
    }

  } // namespace

  CompositorKind detect() {
    static const CompositorKind cached = detectImpl();
    return cached;
  }

  std::string_view name(CompositorKind kind) {
    switch (kind) {
    case CompositorKind::Andromeda:
      return "Andromeda";
    case CompositorKind::Unknown:
      return "Unknown";
    }
    return "Unknown";
  }

  std::string_view envHint() {
    static const std::string cached = buildEnvHint();
    return cached;
  }

} // namespace compositors

#pragma once

#include <nlohmann/json_fwd.hpp>

// Compile shim for overview_launcher_capture.cpp; removed in D2b.
namespace compositors::niri {

  class NiriRuntime {
  public:
    [[nodiscard]] bool requestAction(const nlohmann::json& /*action*/) const { return false; }
  };

} // namespace compositors::niri

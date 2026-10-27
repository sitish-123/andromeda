#pragma once

#include "compositors/andromeda/andromeda_event_handler.h"

#include <optional>
#include <string>
#include <string_view>

namespace compositors::andromeda {
  class AndromedaRuntime;
} // namespace compositors::andromeda

// Caches the Andromeda focused output from the persistent event stream so a query
// is a cache read rather than a blocking IPC round-trip. Andromeda's workspaces
// snapshot marks the single focused workspace, which names the focused output
// even when that workspace is empty.
class AndromedaOutputBackend final : public compositors::andromeda::AndromedaEventHandler {
public:
  explicit AndromedaOutputBackend(compositors::andromeda::AndromedaRuntime& runtime);

  [[nodiscard]] std::optional<std::string> focusedOutputName() const;

  void handleEvent(std::string_view event, const nlohmann::json& data) override;
  void handleStreamReset() override;

private:
  std::optional<std::string> m_focusedOutput;
};

namespace compositors::andromeda {

  [[nodiscard]] bool setOutputPower(AndromedaRuntime& runtime, bool on);

} // namespace compositors::andromeda

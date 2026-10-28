#pragma once

#include "compositors/andromeda/andromeda_event_handler.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace compositors::andromeda {
  class AndromedaRuntime;
} // namespace compositors::andromeda

// Caches focused output and power changes from the persistent event stream.
// Power decisions use a fresh snapshot to include commands not yet dispatched in the stream.
// The focused workspace names the output even when that workspace is empty.
class AndromedaOutputBackend final : public compositors::andromeda::AndromedaEventHandler {
public:
  explicit AndromedaOutputBackend(compositors::andromeda::AndromedaRuntime& runtime);

  [[nodiscard]] std::optional<std::string> focusedOutputName() const;
  [[nodiscard]] std::unordered_map<std::string, bool> outputPowerStates() const;
  void setOutputPowerChangeCallback(std::function<void()> callback);

  void handleEvent(std::string_view event, const nlohmann::json& data) override;
  void handleStreamReset() override;

private:
  std::optional<std::string> m_focusedOutput;
  std::unordered_map<std::string, bool> m_outputPowerStates;
  std::function<void()> m_outputPowerChangeCallback;
};

namespace compositors::andromeda {

  [[nodiscard]] bool setOutputPower(AndromedaRuntime& runtime, bool on);

} // namespace compositors::andromeda

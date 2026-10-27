#include "compositors/andromeda/andromeda_output_backend.h"

#include "compositors/andromeda/andromeda_runtime.h"
#include "util/string_utils.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

AndromedaOutputBackend::AndromedaOutputBackend(compositors::andromeda::AndromedaRuntime& runtime)
    : compositors::andromeda::AndromedaEventHandler(runtime) {}

std::optional<std::string> AndromedaOutputBackend::focusedOutputName() const { return m_focusedOutput; }

void AndromedaOutputBackend::handleEvent(std::string_view event, const nlohmann::json& data) {
  // Full snapshot: follow the single focused workspace, which names the focused
  // output even when that workspace is empty.
  if (event != "workspaces" || !data.is_array()) {
    return;
  }
  for (const auto& entry : data) {
    if (!entry.is_object() || !entry.value("focused", false)) {
      continue;
    }
    const auto outputIt = entry.find("output");
    if (outputIt != entry.end() && outputIt->is_string()) {
      std::string output = StringUtils::trim(outputIt->get<std::string>());
      if (!output.empty()) {
        m_focusedOutput = std::move(output);
      }
    }
    return;
  }
}

void AndromedaOutputBackend::handleStreamReset() { m_focusedOutput.reset(); }

namespace compositors::andromeda {

  bool setOutputPower(AndromedaRuntime& runtime, bool on) { return runtime.requestAction(on ? "dpms-on" : "dpms-off"); }

} // namespace compositors::andromeda

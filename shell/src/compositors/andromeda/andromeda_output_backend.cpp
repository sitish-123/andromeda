#include "compositors/andromeda/andromeda_output_backend.h"

#include "compositors/andromeda/andromeda_runtime.h"
#include "util/string_utils.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

  [[nodiscard]] std::unordered_map<std::string, bool> parseOutputPowerStates(const nlohmann::json& data) {
    std::unordered_map<std::string, bool> states;
    if (!data.is_array()) {
      return states;
    }
    for (const auto& item : data) {
      if (!item.is_object()) {
        continue;
      }
      const auto name = item.find("name");
      const auto power = item.find("powered");
      if (name != item.end()
          && name->is_string()
          && !name->get_ref<const std::string&>().empty()
          && power != item.end()
          && power->is_boolean()) {
        states.emplace(name->get<std::string>(), power->get<bool>());
      }
    }
    return states;
  }

} // namespace

AndromedaOutputBackend::AndromedaOutputBackend(compositors::andromeda::AndromedaRuntime& runtime)
    : compositors::andromeda::AndromedaEventHandler(runtime) {}

std::optional<std::string> AndromedaOutputBackend::focusedOutputName() const { return m_focusedOutput; }

std::unordered_map<std::string, bool> AndromedaOutputBackend::outputPowerStates() const {
  const auto response = m_runtime.requestCommand("outputs");
  return response ? parseOutputPowerStates(response->at("ok")) : std::unordered_map<std::string, bool>{};
}

void AndromedaOutputBackend::setOutputPowerChangeCallback(std::function<void()> callback) {
  m_outputPowerChangeCallback = std::move(callback);
}

void AndromedaOutputBackend::handleEvent(std::string_view event, const nlohmann::json& data) {
  if (event == "outputs") {
    auto states = parseOutputPowerStates(data);
    if (states != m_outputPowerStates) {
      m_outputPowerStates = std::move(states);
      if (m_outputPowerChangeCallback) {
        m_outputPowerChangeCallback();
      }
    }
    return;
  }
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

void AndromedaOutputBackend::handleStreamReset() {
  m_focusedOutput.reset();
  if (!m_outputPowerStates.empty()) {
    m_outputPowerStates.clear();
    if (m_outputPowerChangeCallback) {
      m_outputPowerChangeCallback();
    }
  }
}

namespace compositors::andromeda {

  bool setOutputPower(AndromedaRuntime& runtime, bool on) { return runtime.requestAction(on ? "dpms-on" : "dpms-off"); }

} // namespace compositors::andromeda

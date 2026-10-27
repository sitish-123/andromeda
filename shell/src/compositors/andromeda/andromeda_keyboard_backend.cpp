#include "compositors/andromeda/andromeda_keyboard_backend.h"

#include "compositors/andromeda/andromeda_runtime.h"

#include <utility>

namespace {

  std::optional<KeyboardLayoutState> parseLayoutState(const nlohmann::json& response) {
    if (!response.is_object()) {
      return std::nullopt;
    }

    const auto namesIt = response.find("names");
    const auto currentIt = response.find("current_index");
    if (namesIt == response.end()
        || !namesIt->is_array()
        || currentIt == response.end()
        || !currentIt->is_number_integer()) {
      return std::nullopt;
    }

    KeyboardLayoutState state;
    state.currentIndex = currentIt->get<int>();
    state.names.reserve(namesIt->size());
    for (const auto& entry : *namesIt) {
      if (!entry.is_string()) {
        return std::nullopt;
      }
      state.names.push_back(entry.get<std::string>());
    }

    if (state.currentIndex < 0 || state.currentIndex >= static_cast<int>(state.names.size())) {
      return std::nullopt;
    }
    return state;
  }

} // namespace

AndromedaKeyboardBackend::AndromedaKeyboardBackend(compositors::andromeda::AndromedaRuntime& runtime)
    : compositors::andromeda::AndromedaEventHandler(runtime) {}

bool AndromedaKeyboardBackend::isAvailable() const noexcept { return m_runtime.available(); }

bool AndromedaKeyboardBackend::cycleLayout() const {
  if (!isAvailable()) {
    return false;
  }
  return m_runtime.requestAction("keyboard-layout-next");
}

std::optional<KeyboardLayoutState> AndromedaKeyboardBackend::layoutState() const {
  if (!isAvailable()) {
    return std::nullopt;
  }

  const auto response = m_runtime.requestCommand("keyboard-layouts");
  if (!response.has_value()) {
    return std::nullopt;
  }
  return parseLayoutState(response->at("ok"));
}

std::optional<std::string> AndromedaKeyboardBackend::currentLayoutName() const {
  const auto state = layoutState();
  if (!state.has_value() || state->currentIndex < 0 || state->currentIndex >= static_cast<int>(state->names.size())) {
    return std::nullopt;
  }
  return state->names[static_cast<std::size_t>(state->currentIndex)];
}

void AndromedaKeyboardBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void AndromedaKeyboardBackend::handleEvent(std::string_view event, const nlohmann::json& /*data*/) {
  if (event != "keyboard_layout") {
    return;
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
}

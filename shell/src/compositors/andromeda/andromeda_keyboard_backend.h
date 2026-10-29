#pragma once

#include "compositors/andromeda/andromeda_event_handler.h"
#include "compositors/keyboard_backend.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::andromeda {
  class AndromedaRuntime;
} // namespace compositors::andromeda

class AndromedaKeyboardBackend : public compositors::andromeda::AndromedaEventHandler {
public:
  using ChangeCallback = std::function<void()>;

  explicit AndromedaKeyboardBackend(compositors::andromeda::AndromedaRuntime& runtime);

  [[nodiscard]] bool isAvailable() const noexcept;
  [[nodiscard]] bool cycleLayout() const;
  [[nodiscard]] std::optional<KeyboardLayoutState> layoutState() const;
  [[nodiscard]] std::optional<std::string> currentLayoutName() const;

  void setChangeCallback(ChangeCallback callback);
  void handleEvent(std::string_view event, const nlohmann::json& data) override;

private:
  ChangeCallback m_changeCallback;
};

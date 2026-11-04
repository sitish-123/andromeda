#pragma once

#include "ui/shadow_style.h"

// Live UI settings the toolkit reads from its host (shell config or greeter).
class UiSettings {
public:
  virtual ~UiSettings() = default;
  [[nodiscard]] virtual float uiScale() const = 0;
  [[nodiscard]] virtual ShadowStyle shadowStyle() const = 0;
};

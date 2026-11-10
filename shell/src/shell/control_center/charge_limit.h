#pragma once

#include <cstdint>

struct UPowerChargeLimitState;

namespace charge_limit {

  enum class Mode : std::uint8_t {
    Unsupported,
    UPowerActive,
    UPowerDisabled,
    ExternallyManaged,
    FirmwareManaged,
    ReadOnly,
  };

  struct ControlState {
    bool visible = false;
    bool checked = false;
    bool enabled = false;

    bool operator==(const ControlState&) const = default;
  };

  [[nodiscard]] Mode classify(const UPowerChargeLimitState& state) noexcept;
  [[nodiscard]] ControlState controlState(const UPowerChargeLimitState& state) noexcept;
  [[nodiscard]] bool shouldShow(const UPowerChargeLimitState& state) noexcept;

} // namespace charge_limit

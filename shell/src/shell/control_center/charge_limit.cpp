#include "shell/control_center/charge_limit.h"

#include "dbus/upower/upower_service.h"

namespace charge_limit {

  Mode classify(const UPowerChargeLimitState& state) noexcept {
    if (!state.requestPending && state.enabledAvailable && !state.enabled && state.hasRestrictiveThreshold()) {
      return Mode::ExternallyManaged;
    }

    const bool firmware = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
    const bool hasNumericThreshold = state.configuredStart.has_value()
        || state.configuredEnd.has_value()
        || state.effectiveStart.has_value()
        || state.effectiveEnd.has_value();
    const bool controllable = state.supported && state.methodAvailable && state.enabledAvailable;
    if (controllable) {
      if (!state.enabled) {
        return Mode::UPowerDisabled;
      }
      return firmware && !hasNumericThreshold ? Mode::FirmwareManaged : Mode::UPowerActive;
    }

    if (state.supported && firmware && !hasNumericThreshold) {
      return Mode::FirmwareManaged;
    }
    if (state.supported || hasNumericThreshold) {
      return Mode::ReadOnly;
    }
    return Mode::Unsupported;
  }

  ControlState controlState(const UPowerChargeLimitState& state) noexcept {
    const Mode mode = classify(state);
    ControlState control;
    const bool controllable = state.supported && state.methodAvailable && state.enabledAvailable;
    control.visible = mode == Mode::ExternallyManaged
        || (controllable
            && (mode == Mode::UPowerActive
                || mode == Mode::UPowerDisabled
                || mode == Mode::FirmwareManaged));
    control.checked = mode == Mode::ExternallyManaged ? true : state.requestedEnabled.value_or(state.enabled);
    control.enabled = control.visible && mode != Mode::ExternallyManaged && !state.requestPending;
    return control;
  }

  bool shouldShow(const UPowerChargeLimitState& state) noexcept {
    const bool hasNumericThreshold = state.configuredStart.has_value()
        || state.configuredEnd.has_value()
        || state.effectiveStart.has_value()
        || state.effectiveEnd.has_value();
    const bool firmwareManaged = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
    return hasNumericThreshold || firmwareManaged || controlState(state).visible;
  }

} // namespace charge_limit

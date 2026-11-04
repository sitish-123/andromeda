#include "qos/policy.h"

namespace andromeda::qos {

  Decision ScopePolicy::update(const AppInputs& inputs, bool onBattery, std::uint64_t nowMs) {
    if (inputs.focused || inputs.visible) {
      m_hiddenSinceMs.reset();
      return {.tier = inputs.focused ? Tier::Focus : Tier::Visible};
    }
    if (!m_hiddenSinceMs) {
      m_hiddenSinceMs = nowMs;
    }
    const bool exempt = inputs.audio
        || inputs.call
        || inputs.idleInhibitor
        || inputs.screenCast
        || inputs.rule == Rule::NeverThrottle;
    if (exempt) {
      return {.tier = Tier::Visible};
    }
    const std::uint64_t hiddenAt = *m_hiddenSinceMs + kHiddenDelayMs;
    if (nowMs < hiddenAt) {
      return {.tier = Tier::Visible, .recheckAtMs = hiddenAt};
    }
    const std::uint64_t longAt = hiddenAt + kHiddenLongDelayMs;
    if (!onBattery || nowMs < longAt) {
      // Off battery T3 never comes; a battery change is an input change and calls update again.
      return {.tier = Tier::Hidden, .recheckAtMs = onBattery ? std::optional(longAt) : std::nullopt};
    }
    return {
        .tier = Tier::HiddenLong,
        .frozen = inputs.rule == Rule::FreezeWhenHidden && !inputs.thawIntent,
    };
  }

} // namespace andromeda::qos

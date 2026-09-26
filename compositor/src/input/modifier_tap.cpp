#include "input/modifier_tap.h"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

#include <utility>

namespace umbriel {
  bool modifierTapPressMatches(uint32_t modifiers, uint32_t pressedModifier, uint32_t expected) {
    const uint32_t effective = (modifiers | pressedModifier) & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    return effective == expected && pressedModifier == expected;
  }

  bool ModifierTapState::cancelForKeyPress() {
    const bool wasArmed = armed();
    cancel();
    return wasArmed;
  }

  void ModifierTapState::arm(Keybind bind, const void* source, uint32_t keycode) {
    m_bind = std::move(bind);
    m_source = source;
    m_keycode = keycode;
  }

  std::optional<Keybind> ModifierTapState::release(const void* source, uint32_t keycode) {
    if (!armed() || source != m_source || keycode != m_keycode) {
      return std::nullopt;
    }
    std::optional<Keybind> bind = std::move(m_bind);
    cancel();
    return bind;
  }

  void ModifierTapState::cancel() {
    m_bind.reset();
    m_source = nullptr;
    m_keycode = 0;
  }

} // namespace umbriel

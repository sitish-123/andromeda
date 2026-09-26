#include "input/modifier_tap.h"

#include "check.h"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

using umbriel::Keybind;
using umbriel::KeybindAction;
using umbriel::modifierTapPressMatches;
using umbriel::ModifierTapState;

namespace {

  Keybind launcherBind() {
    Keybind bind;
    bind.useMod = true;
    bind.modifierOnly = true;
    bind.repeat = false;
    bind.action = KeybindAction::Spawn;
    return bind;
  }

} // namespace
UMBRIEL_TEST(currentPressContributesModifierBeforeWlrootsStateUpdate) {
  CHECK(modifierTapPressMatches(0, WLR_MODIFIER_LOGO, WLR_MODIFIER_LOGO));
  CHECK(modifierTapPressMatches(WLR_MODIFIER_CAPS, WLR_MODIFIER_LOGO, WLR_MODIFIER_LOGO));
  CHECK(!modifierTapPressMatches(WLR_MODIFIER_CTRL, WLR_MODIFIER_LOGO, WLR_MODIFIER_LOGO));
  CHECK(!modifierTapPressMatches(0, WLR_MODIFIER_ALT, WLR_MODIFIER_LOGO));
}

UMBRIEL_TEST(releaseCompletesArmedTapOnce) {
  ModifierTapState state;
  int keyboard = 0;
  state.arm(launcherBind(), &keyboard, 42);

  const auto released = state.release(&keyboard, 42);
  CHECK(released.has_value());
  CHECK(released->modifierOnly);
  CHECK(released->action == KeybindAction::Spawn);
  CHECK(!state.armed());
  CHECK(!state.release(&keyboard, 42).has_value());
}

UMBRIEL_TEST(otherKeyPressCancelsWithoutRearming) {
  ModifierTapState state;
  int keyboard = 0;
  CHECK(!state.cancelForKeyPress());

  state.arm(launcherBind(), &keyboard, 42);
  CHECK(state.cancelForKeyPress());
  CHECK(!state.armed());
  CHECK(!state.release(&keyboard, 42).has_value());
}

UMBRIEL_TEST(differentKeyboardCannotCompleteTap) {
  ModifierTapState state;
  int firstKeyboard = 0;
  int secondKeyboard = 0;
  state.arm(launcherBind(), &firstKeyboard, 42);

  CHECK(!state.release(&secondKeyboard, 42).has_value());
  CHECK(state.armed());
  CHECK(state.release(&firstKeyboard, 42).has_value());
}

UMBRIEL_TEST(differentKeyReleaseLeavesTapArmed) {
  ModifierTapState state;
  int keyboard = 0;
  state.arm(launcherBind(), &keyboard, 42);

  CHECK(!state.release(&keyboard, 43).has_value());
  CHECK(state.armed());
  CHECK(state.release(&keyboard, 42).has_value());
}

UMBRIEL_TEST(explicitCancellationDropsTap) {
  ModifierTapState state;
  int keyboard = 0;
  state.arm(launcherBind(), &keyboard, 42);

  state.cancel();
  CHECK(!state.armed());
  CHECK(!state.release(&keyboard, 42).has_value());
}

int main() { return RUN_TESTS(); }

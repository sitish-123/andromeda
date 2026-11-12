#include "shell/polkit/auth_dialog_state.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>

namespace {

  enum class Step : std::uint8_t {
    PolkitBegin,
    PolkitPrompt,
    PolkitEnd,
    PolkitAgentRemoved,
    AskpassBegin,
    AskpassAnswer,
    AskpassHangup,
    UserCancel,
    IdleTimer,
    ExternalOpen,
    ExternalClose,
    RunDeferred,
    TypePassword,
    Count,
  };

  // The agent, the askpass client, the panel and the deferred queue around the state, wired like Application.
  struct World {
    AuthDialogState state;
    PolkitPhase polkit = PolkitPhase::None;
    bool askpass = false;
    bool panelOpen = false;
    bool idleArmed = false;
    int deferredSyncs = 0;
    // Text in the password input, and the mode it was typed for.
    bool typed = false;
    AuthDialogMode typedFor = AuthDialogMode::Closed;

    void send(AuthDialogEvent event) {
      const AuthDialogActions actions = state.handle(event);
      if ((actions.cancelAskpass || actions.refuseAskpass) && askpass) {
        askpass = false;
        send(AuthDialogEvent::AskpassEnded);
      }
      if (actions.cancelPolkit && polkit != PolkitPhase::None) {
        polkit = PolkitPhase::None;
        send(AuthDialogEvent::PolkitEnded);
      }
      idleArmed = idleArmed || actions.armIdleTimer;
      deferredSyncs += actions.syncPanel ? 1 : 0;
      // onOpen clears the input; onClose drops it.
      typed = typed && !actions.clearInput && event != AuthDialogEvent::PanelOpened
          && event != AuthDialogEvent::PanelClosed;
    }

    void syncPanel() {
      if (state.mode() == AuthDialogMode::Closed) {
        if (panelOpen) {
          panelOpen = false;
          send(AuthDialogEvent::PanelClosed);
        }
      } else if (!panelOpen) {
        panelOpen = true;
        send(AuthDialogEvent::PanelOpened);
      }
    }

    void run(Step step) {
      switch (step) {
      case Step::PolkitBegin:
        if (polkit == PolkitPhase::None) {
          polkit = PolkitPhase::Waiting;
          send(AuthDialogEvent::PolkitWaiting);
        }
        break;
      case Step::PolkitPrompt:
        if (polkit != PolkitPhase::None) {
          polkit = PolkitPhase::Ready;
          send(AuthDialogEvent::PolkitReady);
        }
        break;
      case Step::PolkitEnd:
        if (polkit != PolkitPhase::None) {
          polkit = PolkitPhase::None;
          send(AuthDialogEvent::PolkitEnded);
        }
        break;
      case Step::PolkitAgentRemoved:
        polkit = PolkitPhase::None;
        send(AuthDialogEvent::PolkitAgentRemoved);
        break;
      case Step::AskpassBegin:
        if (!askpass && state.canBeginAskpass() && polkit == PolkitPhase::None) {
          askpass = true;
          send(AuthDialogEvent::AskpassBegin);
        }
        break;
      case Step::AskpassAnswer:
      case Step::AskpassHangup:
        if (askpass) {
          askpass = false;
          send(AuthDialogEvent::AskpassEnded);
        }
        break;
      case Step::UserCancel:
        if (panelOpen) {
          send(AuthDialogEvent::UserCancel);
        }
        break;
      case Step::IdleTimer:
        if (idleArmed) {
          idleArmed = false;
          send(AuthDialogEvent::IdleTimer);
        }
        break;
      case Step::ExternalOpen:
        if (!panelOpen) {
          panelOpen = true;
          send(AuthDialogEvent::PanelOpened);
        }
        break;
      case Step::ExternalClose:
        if (panelOpen) {
          panelOpen = false;
          send(AuthDialogEvent::PanelClosed);
        }
        break;
      case Step::RunDeferred:
        if (deferredSyncs > 0) {
          --deferredSyncs;
          syncPanel();
        }
        break;
      case Step::TypePassword:
        if (panelOpen && state.mode() != AuthDialogMode::Closed) {
          typed = true;
          typedFor = state.mode();
        }
        break;
      case Step::Count:
        break;
      }
    }

    void drain() {
      for (int guard = 0; deferredSyncs > 0; ++guard) {
        TEST_CHECK(guard < 64);
        run(Step::RunDeferred);
      }
    }
  };

  void checkInvariants(const World& world) {
    const AuthDialogMode mode = world.state.mode();
    TEST_CHECK(world.askpass == (mode == AuthDialogMode::Askpass));
    TEST_CHECK(world.state.polkitPhase() == world.polkit);
    // A polkit prompt is shown unless askpass holds the dialog.
    TEST_CHECK(world.polkit != PolkitPhase::Ready || mode != AuthDialogMode::Closed);
    // Text typed for one request never goes to another.
    TEST_CHECK(!world.typed || world.typedFor == mode);
  }

} // namespace

int main() {
  std::mt19937 rng(20261112);
  std::uniform_int_distribution<int> pick(0, static_cast<int>(Step::Count) - 1);
  for (int run = 0; run < 2000; ++run) {
    World world;
    for (int i = 0; i < 60; ++i) {
      const auto step = static_cast<Step>(pick(rng));
      const bool askpassBefore = world.askpass;
      world.run(step);
      if (askpassBefore && !world.askpass) {
        TEST_CHECK(step == Step::AskpassAnswer || step == Step::AskpassHangup || step == Step::UserCancel);
      }
      checkInvariants(world);
      if (pick(rng) == 0) {
        world.drain();
        checkInvariants(world);
        TEST_CHECK(world.panelOpen == (world.state.mode() != AuthDialogMode::Closed));
      }
    }
  }
  return 0;
}

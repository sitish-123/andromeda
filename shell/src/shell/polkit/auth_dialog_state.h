#pragma once

#include <cstdint>

// What the authentication dialog shows. A pending askpass request is always the one shown: Askpass <=> pending.
enum class AuthDialogMode : std::uint8_t { Closed, Polkit, Askpass };

// The polkit agent's request: Waiting has no prompt yet (BeginAuthentication alone), Ready has one to show.
enum class PolkitPhase : std::uint8_t { None, Waiting, Ready };

enum class AuthDialogEvent : std::uint8_t {
  PolkitWaiting,
  PolkitReady,
  PolkitEnded,
  // The agent went away (disabled, or registration failed) with whatever request it had.
  PolkitAgentRemoved,
  AskpassBegin,
  // Answered, or the client hung up.
  AskpassEnded,
  // Cancel button or Escape in the dialog.
  UserCancel,
  IdleTimer,
  // The panel opened or closed, from any caller.
  PanelOpened,
  PanelClosed,
};

struct AuthDialogActions {
  bool refuseAskpass = false;
  bool cancelPolkit = false;
  bool cancelAskpass = false;
  bool armIdleTimer = false;
  // The mode changed: wipe the typed text, so it never goes to the other request.
  bool clearInput = false;
  // Make the panel match mode(): open, refresh or close it. Run deferred, never from inside a panel callback.
  bool syncPanel = false;
};

class AuthDialogState {
public:
  // The only place that changes the mode. Events that do not apply in the current state change nothing.
  AuthDialogActions handle(AuthDialogEvent event);

  [[nodiscard]] bool canBeginAskpass() const noexcept {
    return m_polkit == PolkitPhase::None && m_mode != AuthDialogMode::Askpass;
  }
  [[nodiscard]] AuthDialogMode mode() const noexcept { return m_mode; }
  [[nodiscard]] PolkitPhase polkitPhase() const noexcept { return m_polkit; }

private:
  AuthDialogMode m_mode = AuthDialogMode::Closed;
  PolkitPhase m_polkit = PolkitPhase::None;
};

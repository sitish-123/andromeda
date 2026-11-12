#include "shell/polkit/auth_dialog_state.h"

AuthDialogActions AuthDialogState::handle(AuthDialogEvent event) {
  AuthDialogActions actions;
  const AuthDialogMode before = m_mode;
  // After askpass, a polkit request queued behind it takes the dialog.
  const auto afterAskpass = [this] {
    return m_polkit == PolkitPhase::Ready ? AuthDialogMode::Polkit : AuthDialogMode::Closed;
  };
  switch (event) {
  case AuthDialogEvent::PolkitWaiting:
    if (m_polkit == PolkitPhase::None) {
      m_polkit = PolkitPhase::Waiting;
    }
    actions.syncPanel = m_mode == AuthDialogMode::Polkit;
    break;
  case AuthDialogEvent::PolkitReady:
    m_polkit = PolkitPhase::Ready;
    if (m_mode == AuthDialogMode::Closed) {
      m_mode = AuthDialogMode::Polkit;
    }
    actions.syncPanel = m_mode == AuthDialogMode::Polkit;
    break;
  case AuthDialogEvent::PolkitEnded:
  case AuthDialogEvent::PolkitAgentRemoved:
    m_polkit = PolkitPhase::None;
    // Deferred close: a chained BeginAuthentication in the same burst reuses the open dialog.
    actions.armIdleTimer = m_mode == AuthDialogMode::Polkit;
    break;
  case AuthDialogEvent::IdleTimer:
    if (m_mode == AuthDialogMode::Polkit && m_polkit == PolkitPhase::None) {
      m_mode = AuthDialogMode::Closed;
      actions.syncPanel = true;
    }
    break;
  case AuthDialogEvent::AskpassBegin:
    if (!canBeginAskpass()) {
      actions.refuseAskpass = true;
      break;
    }
    m_mode = AuthDialogMode::Askpass;
    actions.syncPanel = true;
    break;
  case AuthDialogEvent::AskpassEnded:
    if (m_mode == AuthDialogMode::Askpass) {
      m_mode = afterAskpass();
      actions.syncPanel = true;
    }
    break;
  case AuthDialogEvent::UserCancel:
    if (m_mode == AuthDialogMode::Askpass) {
      actions.cancelAskpass = true;
      m_mode = afterAskpass();
      actions.syncPanel = true;
    } else if (m_mode == AuthDialogMode::Polkit) {
      actions.cancelPolkit = m_polkit != PolkitPhase::None;
      m_polkit = PolkitPhase::None;
      m_mode = AuthDialogMode::Closed;
      actions.syncPanel = true;
    }
    break;
  case AuthDialogEvent::PanelOpened:
    // Opened by another caller with nothing to show, or showing the current request.
    actions.syncPanel = true;
    break;
  case AuthDialogEvent::PanelClosed:
    if (m_mode == AuthDialogMode::Askpass) {
      // Only the user, an answer or the client ends askpass: the dialog comes back.
      actions.syncPanel = true;
    } else if (m_mode == AuthDialogMode::Polkit) {
      actions.cancelPolkit = m_polkit == PolkitPhase::Ready;
      if (m_polkit == PolkitPhase::Ready) {
        m_polkit = PolkitPhase::None;
      }
      m_mode = AuthDialogMode::Closed;
    }
    break;
  }
  actions.clearInput = m_mode != before;
  return actions;
}

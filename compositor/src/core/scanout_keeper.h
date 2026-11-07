#pragma once

struct wlr_backend;

namespace andromeda::scanout_keeper {

  // The kernel restores the text console when the last open file of a DRM device closes, which shows between two
  // compositors (login, logout). On exit this compositor hands its DRM files to andromeda-system
  // (org.andromeda.System1.Display); the next one releases them once each of its outputs presented a frame.
  void holdForNextCompositor(wlr_backend* backend);

  // greetd's login hand-off (session/greetd patches): the session starts on an inactive VT; "switch" asks greetd to
  // activate it, "ready" (after the first frame) retires the greeter.
  [[nodiscard]] bool switchOffered();
  void requestSwitch();
  // Logout: greetd starts the greeter, which takes the VT once it holds the DRM device. False without a hand-off.
  [[nodiscard]] bool requestLogout();

  void expectFirstFrame();
  void firstFrameShown();

} // namespace andromeda::scanout_keeper

#pragma once

struct wlr_backend;

namespace andromeda::scanout_keeper {

  // The kernel restores the text console when the last open file of a DRM device closes, which shows between two
  // compositors (login, logout). On exit this compositor hands its DRM files to andromeda-system
  // (org.andromeda.System1.Display); the next one releases them once each of its outputs presented a frame.
  void holdForNextCompositor(wlr_backend* backend);

  void expectFirstFrame();
  void firstFrameShown();

} // namespace andromeda::scanout_keeper

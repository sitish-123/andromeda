#pragma once

#include "motion/desk_motion.h"

#include <memory>

struct wlr_output;
struct wlr_scene_buffer;
struct wlr_scene_tree;

namespace andromeda {

  // The frame a previous DRM master (the greeter) left on this output's CRTC, shown above everything from the first
  // commit until the session shell is ready, then crossed into the desktop (.agent/DESIGN.md "Login transition design").
  class HeldFrame {
  public:
    // Empty when the output is not DRM, its CRTC holds no frame, or the frame cannot be exported.
    static std::unique_ptr<HeldFrame> capture(wlr_output* output, wlr_scene_tree* parent);
    ~HeldFrame();
    HeldFrame(const HeldFrame&) = delete;
    HeldFrame& operator=(const HeldFrame&) = delete;

    // Places the frame over the output's layout box (logical size).
    void setBox(int x, int y, int width, int height);
    // Starts the cross: opacity 1 -> 0, scale 1 -> 1.02 on `close`; reduced motion fades on `fade_out` only.
    void release(bool reducedMotion);
    [[nodiscard]] bool released() const noexcept { return m_released; }
    // Advances the cross; returns false once it settled (the caller then destroys the frame).
    bool step(double seconds);

  private:
    explicit HeldFrame(wlr_scene_buffer* node);
    void apply();

    wlr_scene_buffer* m_node = nullptr;
    int m_x = 0;
    int m_y = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_released = false;
    bool m_reducedMotion = false;
    desk::motion::Spring m_progress;
  };

} // namespace andromeda

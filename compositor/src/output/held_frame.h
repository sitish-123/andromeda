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
    // True when the greeter started this session moments ago (ANDROMEDA_LOGIN_HANDOFF = its epoch seconds, at most
    // 15 s old): a TTY start, a restart or a crash recovery never holds the console's frame.
    static bool loginHandoff();
    [[nodiscard]] int bufferWidth() const noexcept { return m_bufferWidth; }
    [[nodiscard]] int bufferHeight() const noexcept { return m_bufferHeight; }
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
    HeldFrame(wlr_scene_buffer* node, int bufferWidth, int bufferHeight);
    void apply();

    wlr_scene_buffer* m_node = nullptr;
    int m_bufferWidth = 0;
    int m_bufferHeight = 0;
    int m_x = 0;
    int m_y = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_released = false;
    bool m_reducedMotion = false;
    desk::motion::Spring m_progress;
  };

} // namespace andromeda

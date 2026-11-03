#pragma once

#include "ipc/pixel_stream_server.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <chrono>
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <vector>

class GlSharedContext;

// Draws a user fragment shader in the lock helper (Shadertoy style: `void mainImage(out vec4 fragColor,
// in vec2 fragCoord)` with iResolution, iTime, iTimeDelta, iFrame). It owns a GL context of its own,
// so its raw GL state never mixes with the renderer's cached state.
class LockShader {
public:
  // Throws std::runtime_error when the context, the framebuffer or the program cannot be made
  // (a compile error included).
  LockShader(GlSharedContext& shared, const std::string& source, std::uint32_t width, std::uint32_t height);
  ~LockShader();
  LockShader(const LockShader&) = delete;
  LockShader& operator=(const LockShader&) = delete;

  // Draws the next frame into frame buffer `index` (0 or 1) and waits until the GPU is done.
  bool draw(std::size_t index);
  // Copies frame buffer `index` into `rgba` as top-down RGBA8 rows (width * height * 4).
  bool read(std::size_t index, std::span<std::uint8_t> rgba);
  // The two frame buffers as dmabufs, in index order; empty when the driver cannot export them.
  [[nodiscard]] const std::vector<PixelStreamServer::Dmabuf>& dmabufs() const noexcept { return m_dmabufs; }
  [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }

private:
  void release() noexcept;
  void exportDmabufs();
  bool makeCurrent();

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLContext m_context = EGL_NO_CONTEXT;
  std::array<GLuint, 2> m_textures{};
  std::array<GLuint, 2> m_framebuffers{};
  std::vector<PixelStreamServer::Dmabuf> m_dmabufs;
  GLuint m_program = 0;
  GLint m_position = -1;
  GLint m_resolution = -1;
  GLint m_time = -1;
  GLint m_timeDelta = -1;
  GLint m_frame = -1;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  int m_frameCount = 0;
  std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point m_last = m_start;
};

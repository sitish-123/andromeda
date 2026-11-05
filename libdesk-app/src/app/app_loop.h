#pragma once

#include "core/poll_source.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "wayland/key_repeat_poll_source.h"
#include "wayland/wayland_connection.h"

#include <chrono>
#include <unordered_map>
#include <vector>

// The run loop of a standalone libdesk-ui app: one Wayland connection, one GL context, one render context, and
// poll sources. DeferredCall, TimerManager and key repeat are built in.
class AppLoop {
public:
  AppLoop();
  ~AppLoop();

  AppLoop(const AppLoop&) = delete;
  AppLoop& operator=(const AppLoop&) = delete;

  // Connects Wayland, starts GL and the render context. False when no compositor answers.
  bool initialize();

  [[nodiscard]] WaylandConnection& wayland() noexcept { return m_wayland; }
  [[nodiscard]] const WaylandConnection& wayland() const noexcept { return m_wayland; }
  [[nodiscard]] GlSharedContext& gl() noexcept { return m_gl; }
  [[nodiscard]] RenderContext& renderContext() noexcept { return m_render; }

  // The caller keeps ownership; remove a source before it is destroyed.
  void addSource(PollSource* source);
  void removeSource(PollSource* source);

  // Runs until quit() or a compositor hangup. Returns the quit code, 0 on hangup, 1 on a Wayland error.
  int run();
  void quit(int code = 0);
  [[nodiscard]] bool quitting() const noexcept { return m_quit; }

private:
  [[nodiscard]] std::vector<PollSource*> sources();

  WaylandConnection m_wayland;
  GlSharedContext m_gl;
  RenderContext m_render;
  KeyRepeatPollSource m_keyRepeat;
  std::vector<PollSource*> m_sources;
  std::unordered_map<PollSource*, std::chrono::steady_clock::time_point> m_deadlines;
  bool m_quit = false;
  int m_exitCode = 0;
};

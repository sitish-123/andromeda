#include "app/app_loop.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "wayland/surface.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <limits>
#include <poll.h>
#include <wayland-client-core.h>

namespace {
  constexpr Logger kLog("app-loop");

  class DeferredSource final : public PollSource {
  public:
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
      if (startIdx < fds.size() && (fds[startIdx].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        DeferredCall::drainWakeFd();
      }
      for (auto& fn : DeferredCall::takePending()) {
        if (fn) {
          fn();
        }
      }
    }

  private:
    void doAddPollFds(std::vector<pollfd>& fds) override {
      if (const int fd = DeferredCall::wakeFd(); fd >= 0) {
        fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
      }
    }
  };

  class TimerSource final : public PollSource {
  public:
    [[nodiscard]] int pollTimeoutMs() const override { return TimerManager::instance().pollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
      TimerManager::instance().tick();
    }

  private:
    void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}
  };

  DeferredSource g_deferred;
  TimerSource g_timers;

  bool isDisconnect(int err) { return err == EPIPE || err == ECONNRESET || err == ENOTCONN || err == EBADF; }

  // A C++ exception must not unwind through libwayland's C dispatch.
  int dispatchGuarded(wl_display* display) {
    try {
      return wl_display_dispatch_pending(display);
    } catch (const std::exception& e) {
      kLog.error("exception in Wayland event handler ({}); continuing", e.what());
    } catch (...) {
      kLog.error("unknown exception in Wayland event handler; continuing");
    }
    return 0;
  }
} // namespace

AppLoop::AppLoop() : m_keyRepeat(m_wayland) {}

AppLoop::~AppLoop() = default;

bool AppLoop::initialize() {
  if (!m_wayland.connect()) {
    kLog.error("failed to connect to the Wayland compositor");
    return false;
  }
  try {
    m_gl.initialize(m_wayland.display());
    m_render.initialize(m_gl);
  } catch (const std::exception& e) {
    kLog.error("GL setup failed: {}", e.what());
    return false;
  }
  return true;
}

void AppLoop::addSource(PollSource* source) {
  if (source != nullptr && !std::ranges::contains(m_sources, source)) {
    m_sources.push_back(source);
  }
}

void AppLoop::removeSource(PollSource* source) {
  std::erase(m_sources, source);
  m_deadlines.erase(source);
}

void AppLoop::quit(int code) {
  m_quit = true;
  m_exitCode = code;
}

std::vector<PollSource*> AppLoop::sources() {
  std::vector<PollSource*> all{&g_deferred, &g_timers, &m_keyRepeat};
  all.insert(all.end(), m_sources.begin(), m_sources.end());
  return all;
}

int AppLoop::run() {
  wl_display* display = m_wayland.display();
  auto fail = [&](const char* what) {
    const int err = errno;
    if (isDisconnect(err)) {
      kLog.info("compositor closed the connection during {}", what);
      return 0;
    }
    kLog.error("{} failed: {}", what, m_wayland.describeDisplayError(err));
    return 1;
  };

  while (!m_quit) {
    for (auto& fn : DeferredCall::takePending()) {
      if (fn) {
        fn();
      }
    }
    if (m_quit) {
      break;
    }

    while (wl_display_prepare_read(display) != 0) {
      if (dispatchGuarded(display) < 0) {
        return fail("Wayland dispatch");
      }
    }
    short waylandEvents = POLLIN;
    int flushed = 0;
    do {
      flushed = wl_display_flush(display);
    } while (flushed < 0 && errno == EINTR);
    if (flushed < 0) {
      if (errno != EAGAIN) {
        wl_display_cancel_read(display);
        return fail("Wayland flush");
      }
      waylandEvents |= POLLOUT;
    }

    const std::vector<PollSource*> list = sources();
    std::vector<pollfd> fds{{.fd = wl_display_get_fd(display), .events = waylandEvents, .revents = 0}};
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(list.size());
    int timeoutMs = -1;
    const auto before = std::chrono::steady_clock::now();
    for (PollSource* source : list) {
      const std::size_t start = source->addPollFds(fds);
      ranges.emplace_back(start, fds.size() - start);
      const int t = source->pollTimeoutMs();
      if (t < 0) {
        m_deadlines.erase(source);
        continue;
      }
      const auto wanted = before + std::chrono::milliseconds(t);
      auto [it, inserted] = m_deadlines.try_emplace(source, wanted);
      if (!inserted) {
        it->second = std::min(it->second, wanted);
      }
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(it->second - before).count();
      const int leftMs = left < 0 ? 0 : static_cast<int>(std::min<std::int64_t>(left, std::numeric_limits<int>::max()));
      timeoutMs = timeoutMs < 0 ? leftMs : std::min(timeoutMs, leftMs);
    }
    if (Surface::hasPendingFrameWork() || Surface::hasPendingRenders()) {
      timeoutMs = 0;
    }
    // A full send buffer with a 0 ms source would spin; wait up to one 60 Hz frame for POLLOUT.
    if ((waylandEvents & POLLOUT) != 0 && timeoutMs >= 0 && timeoutMs < 16) {
      timeoutMs = 16;
    }

    if (::poll(fds.data(), fds.size(), timeoutMs) < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) {
        continue;
      }
      kLog.error("poll failed");
      return 1;
    }
    if ((fds[0].revents & POLLOUT) != 0) {
      do {
        flushed = wl_display_flush(display);
      } while (flushed < 0 && errno == EINTR);
      if (flushed < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        return fail("Wayland flush");
      }
    }
    if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      if (wl_display_read_events(display) < 0) {
        return fail("Wayland read");
      }
    } else {
      wl_display_cancel_read(display);
    }
    if (dispatchGuarded(display) < 0) {
      return fail("Wayland dispatch");
    }
    if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) {
      kLog.info("compositor closed the connection");
      return 0;
    }

    const auto after = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < list.size(); ++i) {
      PollSource* source = list[i];
      const auto [start, count] = ranges[i];
      const bool ready = std::any_of(
          fds.begin() + static_cast<std::ptrdiff_t>(start), fds.begin() + static_cast<std::ptrdiff_t>(start + count),
          [](const pollfd& p) { return p.revents != 0; }
      );
      const auto deadline = m_deadlines.find(source);
      const bool due = deadline != m_deadlines.end() && after >= deadline->second;
      if (!ready && !due) {
        continue;
      }
      // An earlier source may have removed this one in the same pass.
      if (i >= 3 && !std::ranges::contains(m_sources, source)) {
        continue;
      }
      m_deadlines.erase(source);
      source->dispatch(fds, start);
    }

    Surface::drainPendingFrameWork();
    Surface::drainPendingRenders();
  }
  return m_exitCode;
}

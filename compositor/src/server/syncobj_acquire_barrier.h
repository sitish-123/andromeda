#pragma once

#include <functional>
#include <wayland-server-core.h>

struct wlr_compositor;
struct wlr_surface;

namespace andromeda {

  // wlroots holds a linux-drm-syncobj commit back only until its acquire fence materializes, so a commit applies while
  // the client's GPU work is still running. Direct scanout then hands KMS a buffer that is not ready, the page flip
  // waits on its fence, and the frame callback sent right after the commit lets a GPU-bound game queue its next frame
  // early: it presents in bursts (a short frame, then a long one) instead of at its render rate. This holds each such
  // commit until its acquire point has signalled, so a buffer reaches the scene only once it is ready to show.
  class SyncobjAcquireBarrier {
  public:
    // scannedOut: true while the surface's buffer is on a plane (direct scanout). Its commits are not held: the client's
    // next GPU work can wait for that buffer's release, which comes only once the next commit is shown.
    SyncobjAcquireBarrier(wlr_compositor* compositor, std::function<bool(wlr_surface*)> scannedOut);
    ~SyncobjAcquireBarrier();

    SyncobjAcquireBarrier(const SyncobjAcquireBarrier&) = delete;
    SyncobjAcquireBarrier& operator=(const SyncobjAcquireBarrier&) = delete;

  private:
    static void onNewSurface(wl_listener* listener, void* data);

    wl_listener m_newSurface{};
  };

} // namespace andromeda

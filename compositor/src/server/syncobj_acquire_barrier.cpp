#include "server/syncobj_acquire_barrier.h"

#include "wlr.h"

#include <cstdint>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>

namespace andromeda {

  namespace {

    struct SurfaceWatch;

    // One held commit: either a syncobj acquire point (waiter) or an implicit dmabuf write fence (fenceFd + source).
    struct AcquireLock {
      wlr_drm_syncobj_timeline_waiter waiter{};
      bool syncobj = false;
      int fenceFd = -1;
      wl_event_source* source = nullptr;
      SurfaceWatch* watch = nullptr;
      uint32_t seq = 0;
    };

    void finishLock(AcquireLock* lock) {
      if (lock->syncobj) {
        wlr_drm_syncobj_timeline_waiter_finish(&lock->waiter);
      }
      if (lock->source != nullptr) {
        wl_event_source_remove(lock->source);
      }
      if (lock->fenceFd >= 0) {
        close(lock->fenceFd);
      }
    }

    struct SurfaceWatch {
      wlr_surface* surface = nullptr;
      wl_event_loop* loop = nullptr;
      std::vector<AcquireLock*> locks;
      wl_listener clientCommit{};
      wl_listener destroy{};
    };

    void releaseLock(AcquireLock* lock) {
      SurfaceWatch* watch = lock->watch;
      finishLock(lock);
      std::erase(watch->locks, lock);
      const uint32_t seq = lock->seq;
      delete lock;
      // May apply the cached state, and with it every later one no other lock holds back.
      wlr_surface_unlock_cached(watch->surface, seq);
    }

    void onAcquireSignalled(wlr_drm_syncobj_timeline_waiter* waiter) {
      AcquireLock* lock;
      lock = wl_container_of(waiter, lock, waiter);
      releaseLock(lock);
    }

    int onFenceReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
      releaseLock(static_cast<AcquireLock*>(data));
      return 0;
    }

    // Implicit-sync dmabuf clients (Mesa EGL without linux-drm-syncobj) commit while their GPU write may still run; a
    // frame that samples the buffer then waits for the client's GPU work. Hold the commit until the write fence
    // signals, as mutter and KWin do. A signalled fence costs two syscalls and no delay.
    void holdForImplicitFence(SurfaceWatch* watch, wlr_buffer* buffer) {
      wlr_dmabuf_attributes attribs{};
      if (!wlr_buffer_get_dmabuf(buffer, &attribs) || attribs.n_planes < 1) {
        return;
      }
      dma_buf_export_sync_file exported{.flags = DMA_BUF_SYNC_READ, .fd = -1};
      if (ioctl(attribs.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) != 0) {
        return;
      }
      pollfd pfd{.fd = exported.fd, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 0) != 0) {
        close(exported.fd);
        return;
      }
      auto* lock = new AcquireLock();
      lock->watch = watch;
      lock->fenceFd = exported.fd;
      lock->source = wl_event_loop_add_fd(watch->loop, exported.fd, WL_EVENT_READABLE, onFenceReadable, lock);
      if (lock->source == nullptr) {
        close(exported.fd);
        delete lock;
        return;
      }
      lock->seq = wlr_surface_lock_pending(watch->surface);
      watch->locks.push_back(lock);
    }

    void onClientCommit(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, clientCommit);
      wlr_surface* surface = watch->surface;
      if ((surface->pending.committed & WLR_SURFACE_STATE_BUFFER) == 0 || surface->pending.buffer == nullptr) {
        return;
      }
      uint64_t point = 0;
      wlr_drm_syncobj_timeline* timeline =
          andromedafx_linux_drm_syncobj_state_acquire_point(surface, &surface->pending, &point);
      if (timeline == nullptr) {
        holdForImplicitFence(watch, surface->pending.buffer);
        return;
      }
      // WAIT_FOR_SUBMIT reports a point that has not materialized yet as unsignalled instead of failing.
      bool signalled = false;
      if (!wlr_drm_syncobj_timeline_check(timeline, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &signalled)
          || signalled) {
        return;
      }
      auto* lock = new AcquireLock();
      lock->watch = watch;
      lock->syncobj = true;
      // No flags: wait for the point to signal, not just to materialize.
      if (!wlr_drm_syncobj_timeline_waiter_init(&lock->waiter, timeline, point, 0, watch->loop, onAcquireSignalled)) {
        delete lock;
        return;
      }
      lock->seq = wlr_surface_lock_pending(surface);
      watch->locks.push_back(lock);
    }

    void onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      // Release the locks the way wlroots' own acquire wait does when the surface goes away.
      for (AcquireLock* lock : watch->locks) {
        finishLock(lock);
        wlr_surface_unlock_cached(watch->surface, lock->seq);
        delete lock;
      }
      wl_list_remove(&watch->clientCommit.link);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

  } // namespace

  SyncobjAcquireBarrier::SyncobjAcquireBarrier(wlr_compositor* compositor) {
    m_newSurface.notify = onNewSurface;
    wl_signal_add(&compositor->events.new_surface, &m_newSurface);
  }

  SyncobjAcquireBarrier::~SyncobjAcquireBarrier() { wl_list_remove(&m_newSurface.link); }

  void SyncobjAcquireBarrier::onNewSurface(wl_listener* /*listener*/, void* data) {
    auto* watch = new SurfaceWatch();
    watch->surface = static_cast<wlr_surface*>(data);
    watch->loop = wl_display_get_event_loop(wl_client_get_display(wl_resource_get_client(watch->surface->resource)));
    watch->clientCommit.notify = onClientCommit;
    wl_signal_add(&watch->surface->events.client_commit, &watch->clientCommit);
    watch->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&watch->surface->events.destroy, &watch->destroy);
  }

} // namespace andromeda

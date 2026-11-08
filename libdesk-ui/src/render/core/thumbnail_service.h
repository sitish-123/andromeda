#pragma once

#include "core/poll_source.h"
#include "core/idle_worker_slots.h"
#include "render/core/texture_handle.h"
#include "render/core/thumbnail_pipeline.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TextureManager;

// Shared async thumbnail loader. Worker threads run thumbnail_pipeline (cache
// first, then a decode or Thumbnailer1); the main loop uploads finished
// pixmaps through TextureManager via uploadPending(). The service keeps
// explicit in-memory acquire/release ownership.
class ThumbnailService : public PollSource {
public:
  using PendingUploadCallback = std::function<void()>;
  using ReadyCallback = std::function<void(const std::string& path, TextureHandle handle)>;

  // Default thumbnail long-edge in pixels. Callers wanting a sharper preview in
  // a larger box pass an explicit targetPx; each distinct size is cached
  // separately (in memory and on disk).
  static constexpr int kDefaultTargetPx = 192;

  class Subscription {
  public:
    Subscription() = default;
    ~Subscription();

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;

    void disconnect();

  private:
    friend class ThumbnailService;

    explicit Subscription(std::function<void()> disconnect);

    std::function<void()> m_disconnect;
  };

  // The blocking work of one request; the default is thumbnail_pipeline (cache, then decode or Thumbnailer1).
  using Resolver = std::function<std::optional<thumbnail_pipeline::Pixels>(const std::string& path, int targetPx)>;

  ThumbnailService();
  // workers 0 = by core count.
  ThumbnailService(Resolver resolver, std::size_t workers);
  ~ThumbnailService() override;

  ThumbnailService(const ThumbnailService&) = delete;
  ThumbnailService& operator=(const ThumbnailService&) = delete;

  // Decode completion is only a wakeup: consumers should schedule an update,
  // call uploadPending() with their current render context, then re-query
  // visible thumbnail handles.
  [[nodiscard]] Subscription subscribePendingUpload(PendingUploadCallback callback);
  [[nodiscard]] Subscription
  subscribeReady(const std::string& path, ReadyCallback callback, int targetPx = kDefaultTargetPx);

  // Visible requests run before Prefetch ones (rows just outside the view).
  enum class Priority { Visible, Prefetch };

  // Returns the uploaded handle if already decoded, or {0} if a decode is
  // pending. Each acquire must be paired with a release() using the same
  // (path, targetPx).
  [[nodiscard]] TextureHandle
  acquire(const std::string& path, int targetPx = kDefaultTargetPx, Priority priority = Priority::Visible);

  // Moves a queued Prefetch request ahead of other Prefetch requests, behind the queued Visible ones.
  void prioritize(const std::string& path, int targetPx = kDefaultTargetPx);

  // Non-owning lookup used while refreshing an already-acquired thumbnail.
  [[nodiscard]] TextureHandle peek(const std::string& path, int targetPx = kDefaultTargetPx) const;

  // Releases one acquire() owner. The texture is unloaded only when the last
  // owner releases it; a request that has not started yet leaves the queue.
  void release(const std::string& path, int targetPx = kDefaultTargetPx);

  // Uploads decoded pixmaps to textures. Must run on the main thread with the
  // owning render context current.
  [[nodiscard]] bool uploadPending(TextureManager& textures);
  void abandonGpuResources() noexcept;
  void invalidateGpuResources(TextureManager& textures);

  [[nodiscard]] int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  // Identifies a cached thumbnail: same source decoded at different long-edge
  // sizes are independent entries.
  struct RequestKey {
    std::string path;
    int targetPx = kDefaultTargetPx;

    bool operator==(const RequestKey& other) const = default;
  };

  struct RequestKeyHash {
    std::size_t operator()(const RequestKey& key) const noexcept;
  };

  struct DecodedJob {
    RequestKey key;
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool failed = false;
  };

  void workerLoop(std::size_t slot);
  void signalMain();
  void pushResult(DecodedJob job);
  void deleteAllTextures();
  void notifyPendingUpload();
  void notifyReady(const RequestKey& key, TextureHandle handle);
  void enqueueDecodeIfNeeded(const RequestKey& key, Priority priority = Priority::Visible);

  struct CacheEntry {
    TextureHandle handle;
    std::size_t refCount = 0;
    bool failed = false;
  };

  struct PendingListener {
    PendingUploadCallback callback;
  };

  struct ReadyListener {
    RequestKey key;
    ReadyCallback callback;
  };

  const Resolver m_resolver;
  int m_eventFd = -1;
  std::unique_ptr<IdleWorkerSlots> m_workers; // guarded by m_queueMutex
  std::atomic<bool> m_shutdown{false};

  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<RequestKey> m_jobQueue;
  std::deque<RequestKey> m_prefetchQueue;
  // Queued and running requests; m_canceled holds running ones released before their result arrived.
  std::unordered_set<RequestKey, RequestKeyHash> m_inFlight;
  std::unordered_set<RequestKey, RequestKeyHash> m_canceled;

  mutable std::mutex m_resultMutex;
  std::deque<DecodedJob> m_results;

  // Main thread only state.
  TextureManager* m_textureManager = nullptr;
  std::unordered_map<RequestKey, CacheEntry, RequestKeyHash> m_entries;
  std::unordered_map<std::uint64_t, PendingListener> m_pendingListeners;
  std::unordered_map<std::uint64_t, ReadyListener> m_readyListeners;
  std::uint64_t m_nextListenerId = 1;
  std::shared_ptr<bool> m_lifetimeToken = std::make_shared<bool>(true);
};

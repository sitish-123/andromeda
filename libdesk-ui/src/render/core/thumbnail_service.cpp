#include "render/core/thumbnail_service.h"

#include "core/log.h"
#include "render/core/texture_manager.h"
#include "render/core/thumbnail_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <thread>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("thumbnail");
  constexpr std::size_t kMinWorkers = 2;
  constexpr std::size_t kMaxWorkers = 4;

} // namespace

std::size_t ThumbnailService::RequestKeyHash::operator()(const RequestKey& key) const noexcept {
  const std::size_t pathHash = std::hash<std::string>{}(key.path);
  const std::size_t sizeHash = std::hash<int>{}(key.targetPx);
  return pathHash ^ (sizeHash + 0x9e3779b97f4a7c15ULL + (pathHash << 6U) + (pathHash >> 2U));
}

ThumbnailService::Subscription::Subscription(std::function<void()> disconnect) : m_disconnect(std::move(disconnect)) {}

ThumbnailService::Subscription::~Subscription() { disconnect(); }

ThumbnailService::Subscription::Subscription(Subscription&& other) noexcept
    : m_disconnect(std::move(other.m_disconnect)) {}

ThumbnailService::Subscription& ThumbnailService::Subscription::operator=(Subscription&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  disconnect();
  m_disconnect = std::move(other.m_disconnect);
  return *this;
}

void ThumbnailService::Subscription::disconnect() {
  if (!m_disconnect) {
    return;
  }
  auto disconnect = std::move(m_disconnect);
  m_disconnect = nullptr;
  disconnect();
}

ThumbnailService::ThumbnailService() : ThumbnailService(Resolver{}, 0) {}

ThumbnailService::ThumbnailService(Resolver resolver, std::size_t workers)
    : m_resolver(resolver ? std::move(resolver) : [](const std::string& path, int targetPx) {
        return thumbnail_pipeline::resolve(path, targetPx, thumbnail_pipeline::generate);
      }) {
  static std::once_flag pruned;
  std::call_once(pruned, [] { std::thread([] { thumbnail_pipeline::pruneCache(); }).detach(); });
  m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_eventFd < 0) {
    kLog.warn("failed to create eventfd; thumbnail wakeups will be disabled");
  }

  const unsigned hc = std::thread::hardware_concurrency();
  const std::size_t suggested = (hc == 0) ? kMinWorkers : std::max<std::size_t>(kMinWorkers, hc / 2);
  const std::size_t n = workers > 0 ? workers : std::clamp<std::size_t>(suggested, kMinWorkers, kMaxWorkers);
  m_workers = std::make_unique<IdleWorkerSlots>(n, "dsk-thumb");
}

ThumbnailService::~ThumbnailService() {
  if (m_lifetimeToken != nullptr) {
    *m_lifetimeToken = false;
  }
  m_pendingListeners.clear();
  m_readyListeners.clear();

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  m_workers->joinAll();

  deleteAllTextures();

  if (m_eventFd >= 0) {
    ::close(m_eventFd);
    m_eventFd = -1;
  }
}

ThumbnailService::Subscription ThumbnailService::subscribePendingUpload(PendingUploadCallback callback) {
  if (!callback) {
    return {};
  }

  const std::uint64_t id = m_nextListenerId++;
  m_pendingListeners.emplace(id, PendingListener{.callback = std::move(callback)});
  bool hasPendingResults = false;
  {
    std::scoped_lock lock(m_resultMutex);
    hasPendingResults = !m_results.empty();
  }
  if (hasPendingResults && m_pendingListeners.contains(id) && m_pendingListeners[id].callback) {
    m_pendingListeners[id].callback();
  }

  std::weak_ptr<bool> token = m_lifetimeToken;
  return Subscription([this, token, id] {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_pendingListeners.erase(id);
  });
}

ThumbnailService::Subscription
ThumbnailService::subscribeReady(const std::string& path, ReadyCallback callback, int targetPx) {
  if (path.empty() || !callback) {
    return {};
  }

  const TextureHandle current = peek(path, targetPx);
  if (current.id != 0) {
    callback(path, current);
    return {};
  }

  const std::uint64_t id = m_nextListenerId++;
  m_readyListeners.emplace(
      id, ReadyListener{.key = RequestKey{.path = path, .targetPx = targetPx}, .callback = std::move(callback)}
  );

  std::weak_ptr<bool> token = m_lifetimeToken;
  return Subscription([this, token, id] {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_readyListeners.erase(id);
  });
}

TextureHandle ThumbnailService::acquire(const std::string& path, int targetPx, Priority priority) {
  if (path.empty()) {
    return {};
  }

  const RequestKey key{.path = path, .targetPx = targetPx};
  CacheEntry& entry = m_entries[key];
  ++entry.refCount;
  if (entry.handle.id != 0 || entry.failed) {
    return entry.handle;
  }

  enqueueDecodeIfNeeded(key, priority);
  return {};
}

void ThumbnailService::prioritize(const std::string& path, int targetPx) {
  const RequestKey key{.path = path, .targetPx = targetPx};
  std::scoped_lock lock(m_queueMutex);
  // A second copy at the front; the worker runs whichever it meets first and skips the other (m_running).
  if (m_inFlight.contains(key) && !m_running.contains(key)) {
    m_prefetchQueue.push_front(key);
  }
}

TextureHandle ThumbnailService::peek(const std::string& path, int targetPx) const {
  const auto it = m_entries.find(RequestKey{.path = path, .targetPx = targetPx});
  if (it == m_entries.end()) {
    return {};
  }
  return it->second.handle;
}

void ThumbnailService::reload(const std::string& path) {
  for (auto& [key, entry] : m_entries) {
    if (key.path == path && entry.refCount > 0) {
      entry.failed = false;
      enqueueDecodeIfNeeded(key);
    }
  }
}

void ThumbnailService::release(const std::string& path, int targetPx) {
  const RequestKey key{.path = path, .targetPx = targetPx};
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }

  CacheEntry& entry = it->second;
  if (entry.refCount > 0) {
    --entry.refCount;
  }

  if (entry.refCount > 0) {
    return;
  }

  if (entry.handle.id != 0 && m_textureManager != nullptr) {
    m_textureManager->unload(entry.handle);
  }
  m_entries.erase(it);

  std::scoped_lock lock(m_queueMutex);
  if (!m_inFlight.contains(key)) {
    return;
  }
  // Queued: its queue copy goes stale and the worker skips it. Running: its result is dropped.
  if (m_running.contains(key)) {
    m_canceled.insert(key);
  } else {
    m_inFlight.erase(key);
  }
}

void ThumbnailService::enqueueDecodeIfNeeded(const RequestKey& key, Priority priority) {
  std::scoped_lock lock(m_queueMutex);
  m_canceled.erase(key);
  if (m_inFlight.contains(key)) {
    if (priority == Priority::Now && !m_running.contains(key)) {
      m_visibleQueued.insert(key);
      m_jobQueue.push_front(key); // a second copy at the front; the worker skips the stale one
      m_queueCv.notify_one();
      return;
    }
    if (priority == Priority::Visible && !m_running.contains(key) && m_visibleQueued.insert(key).second) {
      m_jobQueue.push_back(key); // promoted: the prefetch copy goes stale
      m_queueCv.notify_one();
    }
    return;
  }
  m_inFlight.insert(key);
  if (priority != Priority::Prefetch) {
    m_visibleQueued.insert(key);
  }
  if (priority == Priority::Now) {
    m_jobQueue.push_front(key);
  } else {
    (priority == Priority::Visible ? m_jobQueue : m_prefetchQueue).push_back(key);
  }
  m_workers->spawn([this](std::size_t slot) { workerLoop(slot); });
  m_queueCv.notify_one();
}

bool ThumbnailService::uploadPending(TextureManager& textures) {
  m_textureManager = &textures;

  std::deque<DecodedJob> jobs;
  {
    std::scoped_lock lock(m_resultMutex);
    jobs = std::move(m_results);
    m_results.clear();
  }
  if (jobs.empty()) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();
  std::size_t uploaded = 0;
  bool changed = false;
  // Uploads past this time wait for the next frame, so a burst of decodes during a scroll never costs one frame more
  // than the budget.
  constexpr auto kFrameBudget = std::chrono::microseconds(3000);
  for (auto it = jobs.begin(); it != jobs.end(); ++it) {
    if (uploaded > 0 && std::chrono::steady_clock::now() - start >= kFrameBudget) {
      {
        std::scoped_lock lock(m_resultMutex);
        m_results.insert(m_results.begin(), std::make_move_iterator(it), std::make_move_iterator(jobs.end()));
      }
      signalMain();
      break;
    }
    auto& job = *it;
    bool dropped = false;
    {
      std::scoped_lock lock(m_queueMutex);
      m_inFlight.erase(job.key);
      m_running.erase(job.key);
      if (auto c = m_canceled.find(job.key); c != m_canceled.end()) {
        m_canceled.erase(c);
        dropped = true;
      }
    }
    if (dropped) {
      continue;
    }

    auto entryIt = m_entries.find(job.key);
    if (entryIt == m_entries.end() || entryIt->second.refCount == 0) {
      continue;
    }
    if (job.failed || job.rgba.empty() || job.width <= 0 || job.height <= 0) {
      entryIt->second.failed = true;
      changed = true;
      continue;
    }

    TextureHandle handle = textures.loadFromRgba(job.rgba.data(), job.width, job.height);
    if (handle.id == 0) {
      kLog.warn("failed to upload thumbnail texture for {}", job.key.path);
      entryIt->second.failed = true;
      changed = true;
      continue;
    }

    if (entryIt->second.handle.id != 0 && m_textureManager != nullptr) {
      m_textureManager->unload(entryIt->second.handle);
    }
    entryIt->second.handle = handle;
    entryIt->second.failed = false;
    changed = true;
    ++uploaded;
    notifyReady(job.key, handle);
  }
  if (uploaded > 0) {
    kLog.debug("thumbnails uploaded {} in {:.2f} ms", uploaded,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  return changed;
}

void ThumbnailService::invalidateGpuResources(TextureManager& textures) {
  m_textureManager = &textures;

  std::vector<RequestKey> liveKeys;
  liveKeys.reserve(m_entries.size());
  for (auto& [key, entry] : m_entries) {
    if (entry.handle.id != 0) {
      m_textureManager->unload(entry.handle);
    }
    entry.handle = {};
    entry.failed = false;
    if (entry.refCount > 0) {
      liveKeys.push_back(key);
    }
  }

  for (const RequestKey& key : liveKeys) {
    enqueueDecodeIfNeeded(key);
  }
}

void ThumbnailService::abandonGpuResources() noexcept {
  for (auto& [key, entry] : m_entries) {
    (void)key;
    entry.handle = {};
    entry.failed = false;
  }
  m_textureManager = nullptr;
}

void ThumbnailService::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_eventFd < 0) {
    return;
  }
  fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0});
}

void ThumbnailService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_eventFd < 0 || startIdx >= fds.size()) {
    return;
  }
  if ((fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  std::uint64_t ignored = 0;
  while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
  }

  notifyPendingUpload();
}

void ThumbnailService::signalMain() {
  if (m_eventFd < 0) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
  if (written < 0 && errno != EAGAIN) {
    kLog.warn("failed to signal thumbnail eventfd: errno={}", errno);
  }
}

void ThumbnailService::pushResult(DecodedJob job) {
  {
    std::scoped_lock lock(m_resultMutex);
    m_results.push_back(std::move(job));
  }
  signalMain();
}

void ThumbnailService::deleteAllTextures() {
  for (auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.handle.id != 0 && m_textureManager != nullptr) {
      m_textureManager->unload(entry.handle);
    }
  }
  m_entries.clear();
}

void ThumbnailService::notifyPendingUpload() {
  std::vector<PendingUploadCallback> callbacks;
  callbacks.reserve(m_pendingListeners.size());
  for (const auto& [id, listener] : m_pendingListeners) {
    (void)id;
    if (listener.callback) {
      callbacks.push_back(listener.callback);
    }
  }

  for (auto& callback : callbacks) {
    callback();
  }
}

void ThumbnailService::notifyReady(const RequestKey& key, TextureHandle handle) {
  std::vector<std::pair<std::uint64_t, ReadyCallback>> callbacks;
  for (const auto& [id, listener] : m_readyListeners) {
    if (listener.key == key && listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (const auto& [id, callback] : callbacks) {
    const auto it = m_readyListeners.find(id);
    if (it == m_readyListeners.end()) {
      continue;
    }
    m_readyListeners.erase(it);
    callback(key.path, handle);
  }
}

void ThumbnailService::workerLoop(std::size_t slot) {
  while (true) {
    RequestKey key;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      if (!m_queueCv.wait_for(lock, IdleWorkerSlots::kIdleExit, [this]() {
            return m_shutdown.load() || !m_jobQueue.empty() || !m_prefetchQueue.empty();
          })) {
        m_workers->exited(slot);
        return;
      }
      if (m_shutdown.load()) {
        return;
      }
      // Copies of released, finished or already started keys are stale: skip them.
      bool found = false;
      while (!found && (!m_jobQueue.empty() || !m_prefetchQueue.empty())) {
        auto& queue = m_jobQueue.empty() ? m_prefetchQueue : m_jobQueue;
        const bool visible = &queue == &m_jobQueue;
        key = std::move(queue.front());
        queue.pop_front();
        if (visible) {
          m_visibleQueued.erase(key);
        }
        found = m_inFlight.contains(key) && m_running.insert(key).second;
      }
      if (!found) {
        continue;
      }
    }

    DecodedJob result;
    result.key = key;
    if (auto pixels = m_resolver(key.path, key.targetPx)) {
      result.rgba = std::move(pixels->rgba);
      result.width = pixels->width;
      result.height = pixels->height;
    } else {
      result.failed = true;
    }
    pushResult(std::move(result));
  }
}

#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <print>
#include <string>
#include <vector>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "thumbnail_service_test: {}", what);
      ++g_failures;
    }
  }

  // Counts uploads; there is no GPU in the test.
  class FakeTextures final : public TextureManager {
  public:
    int uploads = 0;
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRgba(const std::uint8_t*, int width, int height, bool) override {
      ++uploads;
      return {.id = TextureId(static_cast<std::uint32_t>(uploads)), .width = width, .height = height};
    }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override { return {}; }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    TextureHandle importDmabuf(const DmabufImage&) override { return {}; }
    int submittedWorkFence() override { return -1; }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  // Holds "running" until opened and records the order of the work.
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    bool entered = false;
    std::vector<std::string> calls;
  };
} // namespace

int main() {
  Gate gate;
  ThumbnailService service(
      [&gate](const std::string& path, int) {
        std::unique_lock lock(gate.mutex);
        gate.calls.push_back(path);
        if (path == "running") {
          gate.entered = true;
          gate.cv.notify_all();
          gate.cv.wait_for(lock, std::chrono::seconds(3), [&] { return gate.open; });
        }
        return std::optional<thumbnail_pipeline::Pixels>(thumbnail_pipeline::Pixels{.rgba = {1, 2, 3, 255}, .width = 1, .height = 1});
      },
      1
  );

  (void)service.acquire("running");
  {
    std::unique_lock lock(gate.mutex);
    expect(gate.cv.wait_for(lock, std::chrono::seconds(3), [&] { return gate.entered; }), "the worker takes the first request");
  }
  (void)service.acquire("queued");
  (void)service.acquire("p1", ThumbnailService::kDefaultTargetPx, ThumbnailService::Priority::Prefetch);
  (void)service.acquire("p2", ThumbnailService::kDefaultTargetPx, ThumbnailService::Priority::Prefetch);
  (void)service.acquire("p2", ThumbnailService::kDefaultTargetPx, ThumbnailService::Priority::Visible);
  service.release("queued");
  service.release("running");
  bool p1Ready = false;
  auto ready = service.subscribeReady("p1", [&](const std::string&, TextureHandle) { p1Ready = true; });
  (void)service.acquire("after");
  {
    std::scoped_lock lock(gate.mutex);
    gate.open = true;
  }
  gate.cv.notify_all();

  FakeTextures textures;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!p1Ready && std::chrono::steady_clock::now() < until) {
    std::vector<pollfd> fds;
    const std::size_t start = service.addPollFds(fds);
    ::poll(fds.data(), fds.size(), 100);
    service.dispatch(fds, start);
    (void)service.uploadPending(textures);
  }
  expect(p1Ready, "the last request arrives");

  std::vector<std::string> calls;
  {
    std::scoped_lock lock(gate.mutex);
    calls = gate.calls;
  }
  expect(calls == std::vector<std::string>{"running", "p2", "after", "p1"},
         "a released queued request never runs, a Visible acquire lifts a Prefetch one");
  expect(textures.uploads == 3, "the released running request is not uploaded: " + std::to_string(textures.uploads));
  expect(service.peek("running").id.value() == 0, "the released running request has no texture");
  return g_failures == 0 ? 0 : 1;
}

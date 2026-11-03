#include "shell/lockscreen/lock_thumbnail.h"

#include "core/log.h"
#include "core/process/process.h"
#include "render/gl_shared_context.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_shader.h"
#include "shell/lockscreen/lock_video.h"
#include "util/file_utils.h"

#include <cairo.h>
#include <glib.h>
#include <poll.h>

#include <chrono>
#include <deque>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace lock_thumbnail {
  namespace {

    constexpr Logger kLog("lock-thumbnail");
    // 16:9 at twice the editor tile height, so tiles stay sharp at scale 2.
    constexpr std::uint32_t kWidth = 320;
    constexpr std::uint32_t kHeight = 180;
    constexpr int kFirstFrameTimeoutMs = 5000;

    std::string fileUri(const std::string& file) {
      gchar* uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
      std::string text = uri != nullptr ? uri : "";
      g_free(uri);
      return text;
    }

    std::string md5(const std::string& text) {
      gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, text.c_str(), static_cast<gssize>(text.size()));
      std::string hex = sum != nullptr ? sum : "";
      g_free(sum);
      return hex;
    }

    std::filesystem::path cacheHome() {
      const char* xdg = std::getenv("XDG_CACHE_HOME");
      return xdg != nullptr && *xdg != '\0' ? std::filesystem::path(xdg)
                                             : FileUtils::expandUserPath("~/.cache");
    }

    bool writePng(const std::vector<std::uint8_t>& rgba, const std::string& out) {
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, kWidth, kHeight);
      const int stride = cairo_image_surface_get_stride(surface);
      unsigned char* data = cairo_image_surface_get_data(surface);
      cairo_surface_flush(surface);
      for (std::uint32_t y = 0; y < kHeight; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(data + static_cast<std::size_t>(stride) * y);
        for (std::uint32_t x = 0; x < kWidth; ++x) {
          const std::uint8_t* p = rgba.data() + (std::size_t{y} * kWidth + x) * 4;
          row[x] = 0xFF000000U | std::uint32_t{p[0]} << 16U | std::uint32_t{p[1]} << 8U | p[2];
        }
      }
      cairo_surface_mark_dirty(surface);
      std::filesystem::create_directories(std::filesystem::path(out).parent_path());
      // Written next to the target and renamed: the editor never reads half a file.
      const std::string partial = out + ".part";
      const bool ok = cairo_surface_write_to_png(surface, partial.c_str()) == CAIRO_STATUS_SUCCESS;
      cairo_surface_destroy(surface);
      std::error_code error;
      if (!ok) {
        std::filesystem::remove(partial, error);
        return false;
      }
      std::filesystem::rename(partial, out, error);
      return !error;
    }

    // Waits for the first decoded frame and hands it to the shader as iChannel0.
    bool firstFrame(LockVideo& video, LockShader& shader) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFirstFrameTimeoutMs);
      while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{.fd = video.wakeFd(), .events = POLLIN, .revents = 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (::poll(&fd, 1, static_cast<int>(std::max<std::int64_t>(left.count(), 0))) <= 0) {
          return false;
        }
        if (!video.dispatch()) {
          return false;
        }
        if (const auto frame = video.takeFrame(); frame.has_value()) {
          return shader.setChannel0Pixels(frame->pixels, frame->width, frame->height);
        }
      }
      return false;
    }

  } // namespace

  std::filesystem::path cachePath(const std::string& file) {
    std::error_code error;
    const auto mtime = std::filesystem::last_write_time(file, error);
    const auto stamp = error ? 0 : mtime.time_since_epoch().count();
    return cacheHome() / "andromeda" / "lock-thumbs" / (md5(fileUri(file) + ":" + std::to_string(stamp)) + ".png");
  }

  std::string find(const std::string& file) {
    const std::string name = md5(fileUri(file)) + ".png";
    for (const char* size : {"x-large", "large", "normal"}) {
      const std::filesystem::path path = cacheHome() / "thumbnails" / size / name;
      if (std::filesystem::exists(path)) {
        return path.string();
      }
    }
    const std::filesystem::path own = cachePath(file);
    return std::filesystem::exists(own) ? own.string() : std::string{};
  }

  namespace {
    struct Queue {
      std::deque<std::pair<std::string, std::function<void()>>> waiting;
      bool running = false;
    };
    Queue& queue() {
      static Queue instance;
      return instance;
    }

    // Each helper owns a GL context and a decoder: run one at a time.
    void startNext() {
      Queue& q = queue();
      while (!q.running && !q.waiting.empty()) {
        auto [file, done] = std::move(q.waiting.front());
        q.waiting.pop_front();
        process::RunCallbacks callbacks;
        callbacks.onExit = [done = std::move(done)](const process::RunResult&) {
          queue().running = false;
          if (done) {
            done();
          }
          startNext();
        };
        process::RunOptions options;
        options.timeout = std::chrono::seconds(15);
        q.running = process::runAsync(
            std::vector<std::string>{"/proc/self/exe", "lock-helper", "--thumbnail", file, cachePath(file).string()},
            std::move(callbacks), std::move(options)
        );
        if (!q.running) {
          kLog.warn("could not start the lock helper for '{}'", file);
        }
      }
    }
  } // namespace

  void request(const std::string& file, std::function<void()> done) {
    // One try per file per shell run: a file the helper cannot render keeps its plain tile.
    static std::unordered_set<std::string> requested;
    if (!requested.insert(file).second) {
      return;
    }
    queue().waiting.emplace_back(file, std::move(done));
    startNext();
  }

  int render(const std::string& in, const std::string& out) {
    const std::string type = LockScreen::liveWallpaperType(in);
    if (type.empty()) {
      return 2;
    }
    GlSharedContext shared;
    try {
      shared.initializeSurfaceless();
      std::string source;
      std::unique_ptr<LockVideo> video;
      if (type == "video") {
        source = LockVideo::kCoverShader;
        video = std::make_unique<LockVideo>(in, false);
      } else {
        std::ifstream stream(in);
        source.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
      }
      LockShader shader(shared, source, kWidth, kHeight);
      if (video != nullptr && !firstFrame(*video, shader)) {
        kLog.warn("'{}': no first frame", in);
        return 1;
      }
      std::vector<std::uint8_t> rgba(std::size_t{kWidth} * kHeight * 4);
      if (!shader.draw(0) || !shader.read(0, rgba) || !writePng(rgba, out)) {
        kLog.warn("'{}': cannot render the still", in);
        return 1;
      }
    } catch (const std::exception& e) {
      kLog.warn("'{}': {}", in, e.what());
      return 1;
    }
    return 0;
  }

} // namespace lock_thumbnail

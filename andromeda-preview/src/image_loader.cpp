#include "image_loader.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <glycin-2/glycin.h>

namespace preview {

  namespace {
    constexpr Logger kLog("preview-image");

    template <typename T> struct GObjectDeleter {
      void operator()(T* p) const noexcept { g_object_unref(p); }
    };
    template <typename T> using GPtr = std::unique_ptr<T, GObjectDeleter<T>>;

    Pixels decode(const std::string& path, std::uint32_t maxWidth, std::uint32_t maxHeight) {
      GPtr<GFile> file(g_file_new_for_path(path.c_str()));
      GPtr<GlyLoader> loader(gly_loader_new(file.get()));
      gly_loader_set_accepted_memory_formats(loader.get(), GLY_MEMORY_SELECTION_R8G8B8A8);
      GError* error = nullptr;
      GPtr<GlyImage> image(gly_loader_load(loader.get(), &error));
      if (!image) {
        kLog.info("cannot load {}: {}", path, error != nullptr ? error->message : "unknown");
        g_clear_error(&error);
        return {};
      }
      GPtr<GlyFrameRequest> request(gly_frame_request_new());
      gly_frame_request_set_scale(request.get(), maxWidth, maxHeight);
      GPtr<GlyFrame> frame(gly_image_get_specific_frame(image.get(), request.get(), &error));
      if (!frame) {
        kLog.info("cannot decode {}: {}", path, error != nullptr ? error->message : "unknown");
        g_clear_error(&error);
        return {};
      }
      if (gly_frame_get_memory_format(frame.get()) != GLY_MEMORY_R8G8B8A8) {
        kLog.warn("glycin returned an unexpected memory format for {}", path);
        return {};
      }
      GBytes* bytes = gly_frame_get_buf_bytes(frame.get());
      gsize size = 0;
      const auto* data = static_cast<const std::uint8_t*>(g_bytes_get_data(bytes, &size));
      Pixels out;
      out.width = gly_frame_get_width(frame.get());
      out.height = gly_frame_get_height(frame.get());
      out.stride = gly_frame_get_stride(frame.get());
      if (static_cast<std::uint64_t>(out.stride) * out.height > size) {
        return {};
      }
      out.owner = std::shared_ptr<const void>(g_bytes_ref(bytes), [](const void* b) {
        g_bytes_unref(static_cast<GBytes*>(const_cast<void*>(b)));
      });
      out.bytes = {data, size};
      return out;
    }
  } // namespace

  ImageDecoder::ImageDecoder() : m_thread([this] { run(); }) {}

  ImageDecoder::~ImageDecoder() {
    {
      std::scoped_lock lock(m_mutex);
      m_stop = true;
      m_queued.reset();
    }
    m_wake.notify_one();
    m_thread.join();
  }

  void ImageDecoder::request(std::string path, std::uint32_t maxWidth, std::uint32_t maxHeight,
                             std::function<void(Pixels)> done) {
    {
      std::scoped_lock lock(m_mutex);
      const auto serial = m_latest->fetch_add(1) + 1;
      m_queued = Job{std::move(path), maxWidth, maxHeight, std::move(done), serial};
    }
    m_wake.notify_one();
  }

  void ImageDecoder::cancel() {
    std::scoped_lock lock(m_mutex);
    m_queued.reset();
    m_latest->fetch_add(1);
  }

  void ImageDecoder::run() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this] { return m_stop || m_queued.has_value(); });
        if (m_stop) {
          return;
        }
        job = std::move(*m_queued);
        m_queued.reset();
      }
      Pixels pixels = decode(job.path, job.maxWidth, job.maxHeight);
      // The posted call checks the serial on the main loop, so a superseded or cancelled result is dropped there.
      DeferredCall::callLater([latest = m_latest, serial = job.serial, pixels = std::move(pixels),
                               done = std::move(job.done)]() mutable {
        if (latest->load() == serial) {
          done(std::move(pixels));
        }
      });
    }
  }

} // namespace preview

#pragma once

#include "render/core/texture_handle.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class PixmapFormat {
  RGBA, // Red, Green, Blue, Alpha
  BGRA, // Blue, Green, Red, Alpha
  ARGB, // Alpha, Red, Green, Blue
  RGB,  // Red, Green, Blue (No Alpha)
  BGR   // Blue, Green, Red (No Alpha)
};

enum class TextureDataFormat {
  Alpha,
  LuminanceAlpha,
  Rgba,
};

enum class TextureFilter {
  Nearest,
  Linear,
};

class TextureManager {
public:
  virtual ~TextureManager() = default;

  TextureManager(const TextureManager&) = delete;
  TextureManager& operator=(const TextureManager&) = delete;

  static void setGlobalMipmapsEnabled(bool enabled) { s_mipmapsEnabled = enabled; }
  [[nodiscard]] static bool globalMipmapsEnabled() { return s_mipmapsEnabled; }

  [[nodiscard]] virtual TextureHandle
  loadFromFile(const std::string& path, int targetSize = 0, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle
  loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle
  loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle loadFromRaw(
      const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format,
      bool mipmap = false
  ) = 0;
  [[nodiscard]] virtual TextureHandle loadFromPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) = 0;
  [[nodiscard]] virtual TextureHandle
  createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter = TextureFilter::Linear) = 0;
  virtual bool replace(
      TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) = 0;
  virtual bool updateSubImage(
      TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
  ) = 0;
  // A single-plane dmabuf from another process (the lock helper). The fd stays the caller's.
  struct DmabufImage {
    int fd = -1;
    int width = 0;
    int height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t stride = 0;
    std::uint32_t offset = 0;
  };
  // Wraps the dmabuf without a copy; an invalid handle when the driver cannot import it.
  [[nodiscard]] virtual TextureHandle importDmabuf(const DmabufImage& image) = 0;
  virtual void unload(TextureHandle& handle) = 0;
  virtual void cleanup() = 0;
  // Forget GL names without deleting them. Used after a robust-context reset,
  // when every object in the old share group is already invalid.
  virtual void abandonGpuResources() noexcept = 0;

  virtual void probeExtensions() = 0;

protected:
  TextureManager() = default;

private:
  static inline bool s_mipmapsEnabled = true;
};

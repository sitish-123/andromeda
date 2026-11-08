#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The blocking part of ThumbnailService: call it from a worker thread.
namespace thumbnail_pipeline {

  struct Pixels {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
  };

  using Generator = std::function<std::optional<Pixels>(const std::string& path, int targetPx)>;

  // A fresh cached thumbnail (the freedesktop cache, then the in-process WebP cache) no larger than targetPx on its
  // long edge. `generate` runs only when no cache has one.
  [[nodiscard]] std::optional<Pixels> resolve(const std::string& path, int targetPx, const Generator& generate);

  // Asks an out-of-process thumbnailer to fill the freedesktop cache; true when it reports the file ready.
  using Thumbnailer = std::function<bool(const std::string& path, int targetPx)>;

  // PNG, JPEG, GIF and BMP decode in process with wuffs and fill the WebP cache. Every other file, and one of those
  // that wuffs cannot decode, goes to `thumbnailer`.
  [[nodiscard]] std::optional<Pixels>
  generateWith(const std::string& path, int targetPx, const Thumbnailer& thumbnailer);

  // generateWith Thumbnailer1 (andromeda-thumbnailer, sandboxed), unless it failed on this version of the file.
  [[nodiscard]] std::optional<Pixels> generate(const std::string& path, int targetPx);

} // namespace thumbnail_pipeline

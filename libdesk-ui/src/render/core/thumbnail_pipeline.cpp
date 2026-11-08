#include "render/core/thumbnail_pipeline.h"

#include "core/files/directory_scanner.h"
#include "core/freedesktop_thumbnails.h"
#include "core/log.h"
#include "render/core/image_decoder.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stb/stb_image_resize2.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <webp/encode.h>

namespace thumbnail_pipeline {

  namespace {

    constexpr float kThumbnailWebPQuality = 82.0F;
    constexpr std::string_view kThumbnailCacheVersion = "thumbnail-service-v3";
    constexpr Logger kLog("thumbnail");
    constexpr std::array<std::string_view, 5> kWuffsExtensions = {".png", ".jpg", ".jpeg", ".gif", ".bmp"};

    std::string lowerExtension(const std::string& path) {
      std::string ext = std::filesystem::path(path).extension().string();
      std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return ext;
    }

    bool decodesInProcess(const std::string& path) { return std::ranges::contains(kWuffsExtensions, lowerExtension(path)); }

    std::filesystem::path thumbnailCacheDir() {
      if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "noctalia" / "thumbnails";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".cache" / "noctalia" / "thumbnails";
      }
      return std::filesystem::path("/tmp") / "noctalia" / "thumbnails";
    }

    std::uint64_t fnv1a64(std::string_view text) {
      std::uint64_t hash = 14695981039346656037ULL;
      for (char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
      }
      return hash;
    }

    std::string hex64(std::uint64_t value) {
      static constexpr char kDigits[] = "0123456789abcdef";
      std::string out(16, '0');
      for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
      }
      return out;
    }

    std::optional<std::filesystem::path> cachePathForSource(const std::string& sourcePath, int targetPx) {
      namespace fs = std::filesystem;
      std::error_code ec;
      const auto size = fs::file_size(sourcePath, ec);
      if (ec) {
        return std::nullopt;
      }
      const auto mtime = fs::last_write_time(sourcePath, ec);
      if (ec) {
        return std::nullopt;
      }
      const auto ticks = mtime.time_since_epoch().count();
      const std::string key = sourcePath + '\n' + std::to_string(size) + '\n'
          + std::to_string(static_cast<long long>(ticks)) + '\n' + std::to_string(targetPx) + '\n'
          + std::string(kThumbnailCacheVersion);
      return thumbnailCacheDir() / (hex64(fnv1a64(key)) + ".webp");
    }

    bool writeFile(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        return false;
      }
      // Readers on other threads see the whole file or none: write aside, then rename.
      auto tmp = path;
      tmp += ".tmp." + std::to_string(::getpid()) + "." + std::to_string(::gettid());
      {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file.flush()) {
          std::filesystem::remove(tmp, ec);
          return false;
        }
      }
      std::filesystem::rename(tmp, path, ec);
      if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
      }
      return true;
    }

    std::vector<std::uint8_t> rgbaToRgb(const std::vector<std::uint8_t>& rgba) {
      const std::size_t pixelCount = rgba.size() / 4;
      std::vector<std::uint8_t> rgb(pixelCount * 3);
      for (std::size_t i = 0; i < pixelCount; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
      }
      return rgb;
    }

    std::vector<std::uint8_t> rgbToRgba(const std::vector<std::uint8_t>& rgb) {
      const std::size_t pixelCount = rgb.size() / 3;
      std::vector<std::uint8_t> rgba(pixelCount * 4);
      for (std::size_t i = 0; i < pixelCount; ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
      }
      return rgba;
    }

    bool resizeThumbnail(std::vector<std::uint8_t>& pixels, int& width, int& height, int targetPx) {
      const int maxDim = std::max(width, height);
      if (maxDim <= targetPx || width <= 0 || height <= 0) {
        return true;
      }
      const float scale = static_cast<float>(targetPx) / static_cast<float>(maxDim);
      const int resizedW = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
      const int resizedH = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale)));
      std::vector<std::uint8_t> out(static_cast<std::size_t>(resizedW) * static_cast<std::size_t>(resizedH) * 3);
      if (stbir_resize_uint8_linear(pixels.data(), width, height, 0, out.data(), resizedW, resizedH, 0, STBIR_RGB)
          == nullptr) {
        return false;
      }
      pixels = std::move(out);
      width = resizedW;
      height = resizedH;
      return true;
    }

    // Decodes and fits to targetPx; the RGB pass drops alpha, as the WebP cache does.
    using Decoder = std::expected<DecodedRasterImage, std::string> (*)(const std::uint8_t*, std::size_t);

    std::optional<std::vector<std::uint8_t>> decodeFitted(
        const std::vector<std::uint8_t>& bytes, int targetPx, int& width, int& height, Decoder decode = decodeWuffsImage
    ) {
      if (bytes.empty()) {
        return std::nullopt;
      }
      auto decoded = decode(bytes.data(), bytes.size());
      if (!decoded) {
        return std::nullopt;
      }
      width = decoded->width;
      height = decoded->height;
      auto rgb = rgbaToRgb(decoded->pixels);
      if (!resizeThumbnail(rgb, width, height, targetPx)) {
        return std::nullopt;
      }
      return rgb;
    }

    std::optional<Pixels> fromFile(const std::filesystem::path& file, int targetPx) {
      Pixels out;
      auto rgb = decodeFitted(FileUtils::readBinaryFile(file.string()), targetPx, out.width, out.height);
      if (!rgb) {
        return std::nullopt;
      }
      out.rgba = rgbToRgba(*rgb);
      return out;
    }

  } // namespace

  std::optional<Pixels> resolve(const std::string& path, int targetPx, const Generator& generate) {
    // An image takes only a large enough shared entry; a smaller one would look worse than its own decode.
    if (const auto shared = freedesktop_thumbnails::lookup(path, targetPx, !DirectoryScanner::isImagePath(path))) {
      if (auto pixels = fromFile(*shared, targetPx)) {
        return pixels;
      }
    }
    if (const auto cachePath = cachePathForSource(path, targetPx)) {
      auto bytes = FileUtils::readBinaryFile(cachePath->string());
      if (!bytes.empty()) {
        if (auto cached = decodeRasterImage(bytes.data(), bytes.size())) {
          return Pixels{.rgba = std::move(cached->pixels), .width = cached->width, .height = cached->height};
        }
        std::error_code ec;
        std::filesystem::remove(*cachePath, ec);
      }
    }
    return generate ? generate(path, targetPx) : std::nullopt;
  }

  std::optional<Pixels> generate(const std::string& path, int targetPx) {
    return generateWith(path, targetPx, [](const std::string& file, int px) {
      return !freedesktop_thumbnails::knownFailure(file) && freedesktop_thumbnails::generate(file, px);
    });
  }

  std::optional<Pixels> generateWith(const std::string& path, int targetPx, const Thumbnailer& thumbnailer) {
    Pixels out;
    std::optional<std::vector<std::uint8_t>> rgb;
    if (decodesInProcess(path)) {
      rgb = decodeFitted(FileUtils::readBinaryFile(path), targetPx, out.width, out.height);
    }
    if (!rgb) {
      if (thumbnailer && thumbnailer(path, targetPx)) {
        const auto made = freedesktop_thumbnails::lookup(path, targetPx);
        return made ? fromFile(*made, targetPx) : std::nullopt;
      }
      // Without a working Thumbnailer1 a WebP (common for wallpapers) still decodes in process, by libwebp only.
      if (lowerExtension(path) != ".webp") {
        return std::nullopt;
      }
      kLog.info("no thumbnailer result for {}; decoding the WebP in process", path);
      rgb = decodeFitted(FileUtils::readBinaryFile(path), targetPx, out.width, out.height, decodeWebPImage);
      if (!rgb) {
        return std::nullopt;
      }
    }
    if (const auto cachePath = cachePathForSource(path, targetPx)) {
      std::uint8_t* encoded = nullptr;
      const std::size_t encodedSize =
          WebPEncodeRGB(rgb->data(), out.width, out.height, out.width * 3, kThumbnailWebPQuality, &encoded);
      if (encoded != nullptr && encodedSize > 0) {
        (void)writeFile(*cachePath, encoded, encodedSize);
        WebPFree(encoded);
      }
    }
    out.rgba = rgbToRgba(*rgb);
    return out;
  }

} // namespace thumbnail_pipeline

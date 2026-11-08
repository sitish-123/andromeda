#include "render/core/image_encoder.h"
#include "render/core/thumbnail_pipeline.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <print>
#include <string>
#include <unistd.h>
#include <utime.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "thumbnail_pipeline_test: {}", what);
      ++g_failures;
    }
  }

  std::uint32_t crc32(const std::string& bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char ch : bytes) {
      crc ^= static_cast<unsigned char>(ch);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
      }
    }
    return ~crc;
  }

  std::string be32(std::uint32_t n) {
    return {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  }

  // A real PNG with the Thumb::URI and Thumb::MTime text chunks after IHDR.
  void writeThumb(const fs::path& png, const std::string& uri, long long mtime, int size) {
    const std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4, 200);
    const auto encoded = encodePng(rgba.data(), size, size);
    std::string bytes(encoded.begin(), encoded.end());
    std::string text;
    for (const auto& [key, value] : std::array<std::pair<std::string, std::string>, 2>{
             {{"Thumb::URI", uri}, {"Thumb::MTime", std::to_string(mtime)}}}) {
      const std::string body = "tEXt" + key + '\0' + value;
      text += be32(static_cast<std::uint32_t>(body.size() - 4)) + body + be32(crc32(body));
    }
    constexpr std::size_t kAfterIhdr = 8 + 4 + 4 + 13 + 4;
    bytes.insert(kAfterIhdr, text);
    fs::create_directories(png.parent_path());
    std::ofstream(png, std::ios::binary) << bytes;
  }
} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("thumb-pipeline-" + std::to_string(getpid()));
  fs::create_directories(root);
  // Before the first g_get_user_cache_dir(): glib reads it once.
  setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);

  const fs::path file = root / "report.pdf";
  std::ofstream(file) << "%PDF";
  utimbuf times{1700000000, 1700000000};
  utime(file.c_str(), &times);
  char* uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
  char* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);

  int generated = 0;
  const thumbnail_pipeline::Generator counting = [&](const std::string&, int) {
    ++generated;
    return std::optional<thumbnail_pipeline::Pixels>{};
  };

  auto missing = thumbnail_pipeline::resolve(file.string(), 128, counting);
  expect(!missing && generated == 1, "a cache miss runs the generator");

  writeThumb(root / "cache" / "thumbnails" / "large" / (std::string(md5) + ".png"), uri, 1700000000, 256);
  generated = 0;
  auto cached = thumbnail_pipeline::resolve(file.string(), 128, counting);
  expect(generated == 0, "a fresh cached thumbnail runs no generator");
  expect(cached && cached->width == 128 && cached->height == 128, "the large entry is fitted to 128 px");
  expect(cached && cached->rgba.size() == 128U * 128U * 4U, "the pixels are RGBA");

  times = {1700000500, 1700000500};
  utime(file.c_str(), &times);
  generated = 0;
  (void)thumbnail_pipeline::resolve(file.string(), 128, counting);
  expect(generated == 1, "a thumbnail of an older version of the file is not used");

  // HEIF bytes would reach libheif if decoded in process; they must go to the sandboxed thumbnailer.
  const fs::path heif = root / "photo.heif";
  std::ofstream(heif, std::ios::binary) << std::string("\0\0\0\x18" "ftypheic", 12);
  utime(heif.c_str(), &times);
  int thumbnailerRuns = 0;
  const thumbnail_pipeline::Thumbnailer fakeThumbnailer = [&](const std::string& path, int) {
    ++thumbnailerRuns;
    char* heifUri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    char* heifMd5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, heifUri, -1);
    writeThumb(root / "cache" / "thumbnails" / "normal" / (std::string(heifMd5) + ".png"), heifUri, 1700000500, 96);
    g_free(heifMd5);
    g_free(heifUri);
    return true;
  };
  auto made = thumbnail_pipeline::generateWith(heif.string(), 128, fakeThumbnailer);
  expect(thumbnailerRuns == 1, "a HEIF request goes to the thumbnailer");
  expect(made && made->width == 96, "the HEIF thumbnail comes from the thumbnailer's cache entry");

  const fs::path png = root / "shot.png";
  const std::vector<std::uint8_t> rgba(64U * 32U * 4U, 90);
  const auto pngBytes = encodePng(rgba.data(), 64, 32);
  std::ofstream(png, std::ios::binary).write(reinterpret_cast<const char*>(pngBytes.data()),
                                             static_cast<std::streamsize>(pngBytes.size()));
  thumbnailerRuns = 0;
  auto decoded = thumbnail_pipeline::generateWith(png.string(), 128, fakeThumbnailer);
  expect(thumbnailerRuns == 0 && decoded && decoded->width == 64, "a PNG decodes in process");

  g_free(md5);
  g_free(uri);
  fs::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}

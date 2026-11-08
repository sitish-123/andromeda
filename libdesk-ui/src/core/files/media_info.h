#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

// Header facts of an image, video or audio file, read from the file's own bytes (no decoder, no subprocess).
// Images: PNG, GIF, WebP, JPEG. Video: MP4/MOV, Matroska/WebM. Audio: MP3 (ID3v2 + Xing or CBR), FLAC, WAV, M4A.
// Every read is bounds-checked: the input is untrusted.
struct MediaInfo {
  std::optional<std::pair<std::uint32_t, std::uint32_t>> size; // width, height in pixels
  std::optional<double> seconds;                              // duration
  std::string artist;
};

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> readImageSize(const std::filesystem::path& file);
// Empty fields where the format is unknown or the header has no such fact.
[[nodiscard]] MediaInfo readMediaInfo(const std::filesystem::path& file);
// "0:42", "1:02:05".
[[nodiscard]] std::string formatDuration(double seconds);

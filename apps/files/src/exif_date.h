#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <string_view>

// Date taken (EXIF DateTimeOriginal, else DateTime) of a JPEG or TIFF, read from the first 128 KiB of the file.
// Every read is bounds-checked: the input is untrusted. The EXIF time has no zone: it comes back as that wall time in
// UTC, so a UTC calendar date of it (batch_rename isoDate) is the date the camera wrote.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> readDateTaken(const std::filesystem::path& file);
// The same on bytes already read.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseDateTaken(std::string_view bytes);
// The file offset of IFD0's Orientation value (a SHORT, 1-8) in a JPEG's first bytes; none when it has no such tag.
[[nodiscard]] std::optional<std::size_t> jpegOrientationOffset(std::string_view bytes);
// Turns a JPEG a quarter clockwise by rewriting its Orientation tag in place (lossless: pixels untouched). False when
// the file has no Orientation tag to rewrite.
bool rotateJpegClockwise(const std::filesystem::path& file);

// What the Gallery Info card shows (mockup section 15): camera, exposure chips, date taken and the GPS position.
struct PhotoExif {
  std::string camera; // Make + Model, the make left out when the model already starts with it
  std::optional<double> fNumber;
  std::optional<double> exposureSeconds;
  std::optional<int> iso;
  std::optional<double> focalMm;
  std::optional<std::chrono::system_clock::time_point> taken;
  std::optional<std::pair<double, double>> gps; // latitude, longitude in degrees (south and west negative)
  int width = 0; // PixelXDimension / PixelYDimension; 0 when absent
  int height = 0;
};
// Reads the first 128 KiB of a JPEG or TIFF; every read is bounds-checked (untrusted input). Blocks: call off the UI
// thread.
[[nodiscard]] PhotoExif readPhotoExif(const std::filesystem::path& file);
[[nodiscard]] PhotoExif parsePhotoExif(std::string_view bytes);

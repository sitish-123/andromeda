#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>

// Date taken (EXIF DateTimeOriginal, else DateTime) of a JPEG or TIFF, read from the first 128 KiB of the file.
// Every read is bounds-checked: the input is untrusted. The EXIF time has no zone: it comes back as that wall time in
// UTC, so a UTC calendar date of it (batch_rename isoDate) is the date the camera wrote.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> readDateTaken(const std::filesystem::path& file);
// The same on bytes already read.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseDateTaken(std::string_view bytes);

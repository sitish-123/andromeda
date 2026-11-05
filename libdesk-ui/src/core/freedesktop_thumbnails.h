#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

// The freedesktop thumbnail cache (~/.cache/thumbnails, Thumbnail Managing Standard 0.9) and its maker,
// org.freedesktop.thumbnails.Thumbnailer1 (andromeda-thumbnailer). Blocking: call from a worker thread.
namespace freedesktop_thumbnails {

  // A valid thumbnail of `path` at least `targetPx` on its long edge, else (allowSmaller) the largest smaller one:
  // Thumb::URI must name the file and Thumb::MTime must match its mtime.
  [[nodiscard]] std::optional<std::filesystem::path> lookup(const std::filesystem::path& path, int targetPx,
                                                            bool allowSmaller = true);

  // A thumbnailer already failed on this version of the file (a fail/<app>/ entry with a matching Thumb::MTime).
  [[nodiscard]] bool knownFailure(const std::filesystem::path& path);

  // Asks Thumbnailer1 for a thumbnail and waits for its answer. True when it reports the file ready.
  bool generate(const std::filesystem::path& path, int targetPx,
                std::chrono::milliseconds timeout = std::chrono::seconds(10));

} // namespace freedesktop_thumbnails

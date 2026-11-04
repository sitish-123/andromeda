#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Still images of live lock wallpapers (videos and shaders) for the editor's tiles. The shell decodes nothing: it reads
// the freedesktop thumbnail of the file, or a still the lock helper rendered (`noctalia lock-helper --thumbnail`).
namespace lock_thumbnail {

  // An image that shows `file`: its freedesktop thumbnail, or the helper's still. Empty while none exists.
  [[nodiscard]] std::string find(const std::string& file);
  // Where the helper writes the still of `file` (keyed by its URI and mtime).
  [[nodiscard]] std::filesystem::path cachePath(const std::string& file);
  // Queues the helper once per file per shell run (one helper at a time); `done` runs on the main loop after it exits.
  void request(const std::string& file, std::function<void()> done);

  // Helper side: renders the first video frame or one shader frame of `in` into the PNG `out`. Exit code.
  int render(const std::string& in, const std::string& out);

} // namespace lock_thumbnail

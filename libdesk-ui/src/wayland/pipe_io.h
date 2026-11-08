#pragma once

#include <cstddef>
#include <string>

// Blocking pipe I/O for Wayland data transfers (clipboard, primary selection, drag and drop).
namespace pipe_io {
  // Reads `fd` to its end and closes it; run it off the main thread. Gives "" after 5 s of silence or past `maxBytes`.
  std::string readPipe(int fd, std::size_t maxBytes = static_cast<std::size_t>(-1));
  // Writes all of `bytes` to `fd` (does not close it).
  void writeAll(int fd, const std::string& bytes);
} // namespace pipe_io

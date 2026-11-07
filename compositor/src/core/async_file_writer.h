#pragma once

#include <filesystem>
#include <string>

namespace andromeda {

  // Replaces `path` whole (temporary file + rename) on a background thread, so a slow disk never stalls the main
  // thread. Writes keep their order; a newer write to the same path replaces a pending one. Pending writes finish
  // at exit.
  void writeFileAsync(std::filesystem::path path, std::string text);

} // namespace andromeda

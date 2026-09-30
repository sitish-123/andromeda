#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace FileUtils {

  [[nodiscard]] inline std::vector<std::uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      return {};
    }
    const auto size = file.tellg();
    if (size <= 0) {
      return {};
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file) {
      return {};
    }
    return data;
  }

} // namespace FileUtils

#include "ui/files/file_format.h"

#include "core/files/file_info.h"

#include <array>
#include <cctype>
#include <ctime>
#include <format>
#include <gio/gio.h>
#include <string_view>
#include <unordered_map>

std::string formatFileSize(std::uint64_t bytes) {
  static constexpr std::array<const char*, 5> kUnits{"bytes", "KB", "MB", "GB", "TB"};
  if (bytes < 1000) {
    return std::format("{} {}", bytes, bytes == 1 ? "byte" : "bytes");
  }
  auto value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1000.0 && unit + 1 < kUnits.size()) {
    value /= 1000.0;
    ++unit;
  }
  return value >= 10.0 ? std::format("{:.0f} {}", value, kUnits[unit]) : std::format("{:.1f} {}", value, kUnits[unit]);
}

std::string formatFileDate(std::int64_t mtimeNs) {
  const std::time_t t = static_cast<std::time_t>(mtimeNs / 1'000'000'000);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  std::tm today{};
  ::localtime_r(&t, &local);
  ::localtime_r(&now, &today);
  std::array<char, 64> buf{};
  const char* fmt = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday ? "Today, %H:%M"
      : local.tm_year == today.tm_year                                             ? "%-d %b, %H:%M"
                                                                                   : "%-d %b %Y";
  std::strftime(buf.data(), buf.size(), fmt, &local);
  return buf.data();
}

std::string fileKindLabel(const FileInfo& info) {
  if (info.isDir) {
    return "Folder";
  }
  // g_content_type_get_description reads shared-mime-info; cache per type, it is called per bound row.
  static std::unordered_map<std::string, std::string> cache;
  if (auto it = cache.find(info.contentType); it != cache.end()) {
    return it->second;
  }
  gchar* desc = g_content_type_get_description(info.contentType.c_str());
  std::string out = desc != nullptr ? desc : info.contentType;
  g_free(desc);
  if (!out.empty()) {
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  }
  cache.emplace(info.contentType, out);
  return out;
}

const char* fileTypeGlyph(const FileInfo& info) {
  if (info.isDir) {
    return "folder";
  }
  const std::string_view t = info.contentType;
  if (t.starts_with("image/")) {
    return "photo";
  }
  if (t.starts_with("video/")) {
    return "movie";
  }
  if (t.starts_with("audio/")) {
    return "music";
  }
  if (t == "application/pdf") {
    return "file-type-pdf";
  }
  if (t.starts_with("text/")) {
    return "file-text";
  }
  if (t.find("zip") != std::string_view::npos || t.find("tar") != std::string_view::npos
      || t.find("compressed") != std::string_view::npos) {
    return "file-zip";
  }
  return "file";
}

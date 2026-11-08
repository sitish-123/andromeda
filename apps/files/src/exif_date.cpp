#include "exif_date.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

namespace {
  constexpr std::size_t kReadLimit = 128 * 1024;
  constexpr std::uint16_t kTagDateTime = 0x0132;
  constexpr std::uint16_t kTagExifIfd = 0x8769;
  constexpr std::uint16_t kTagDateTimeOriginal = 0x9003;
  constexpr int kMaxEntries = 512; // a real IFD has a few dozen

  struct Tiff {
    std::string_view data; // starts at the TIFF header
    bool little = true;

    [[nodiscard]] std::optional<std::uint16_t> u16(std::size_t at) const {
      if (at + 2 > data.size()) {
        return std::nullopt;
      }
      const auto b0 = static_cast<std::uint8_t>(data[at]);
      const auto b1 = static_cast<std::uint8_t>(data[at + 1]);
      return static_cast<std::uint16_t>(little ? b0 | (b1 << 8) : (b0 << 8) | b1);
    }
    [[nodiscard]] std::optional<std::uint32_t> u32(std::size_t at) const {
      const auto a = u16(at);
      const auto b = u16(at + 2);
      if (!a || !b) {
        return std::nullopt;
      }
      return little ? (std::uint32_t{*b} << 16) | *a : (std::uint32_t{*a} << 16) | *b;
    }

    // The value offset of `tag` in the IFD at `ifd` (for ASCII: where its bytes start), with its count.
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> find(std::uint32_t ifd, std::uint16_t tag) const {
      const auto entries = u16(ifd);
      if (!entries || *entries > kMaxEntries) {
        return std::nullopt;
      }
      for (std::uint32_t i = 0; i < *entries; ++i) {
        const std::size_t at = ifd + 2 + std::size_t{i} * 12;
        const auto t = u16(at);
        const auto count = u32(at + 4);
        if (!t || !count) {
          return std::nullopt;
        }
        if (*t == tag) {
          // Values of 4 bytes or less sit in the entry itself.
          if (*count <= 4) {
            return std::pair{static_cast<std::uint32_t>(at + 8), *count};
          }
          const auto offset = u32(at + 8);
          return offset ? std::optional{std::pair{*offset, *count}} : std::nullopt;
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> date(std::uint32_t ifd, std::uint16_t tag) const {
      const auto where = find(ifd, tag);
      if (!where || where->second < 19 || std::size_t{where->first} + 19 > data.size()) {
        return std::nullopt;
      }
      // "YYYY:MM:DD HH:MM:SS"
      const std::string text(data.substr(where->first, 19));
      std::tm tm{};
      if (std::sscanf(text.c_str(), "%4d:%2d:%2d %2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                      &tm.tm_min, &tm.tm_sec) != 6 ||
          tm.tm_year < 1900 || tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31) {
        return std::nullopt;
      }
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;

      const std::time_t t = ::timegm(&tm);
      if (t == static_cast<std::time_t>(-1)) {
        return std::nullopt;
      }
      return std::chrono::system_clock::from_time_t(t);
    }
  };

  std::optional<std::chrono::system_clock::time_point> fromTiff(std::string_view data) {
    if (data.size() < 8 || !(data.starts_with("II*\0") || data.starts_with(std::string_view("MM\0*", 4)))) {
      return std::nullopt;
    }
    Tiff tiff{.data = data, .little = data[0] == 'I'};
    const auto ifd0 = tiff.u32(4);
    if (!ifd0) {
      return std::nullopt;
    }
    if (const auto exif = tiff.find(*ifd0, kTagExifIfd)) {
      if (const auto exifIfd = tiff.u32(exif->first)) {
        if (auto taken = tiff.date(*exifIfd, kTagDateTimeOriginal)) {
          return taken;
        }
      }
    }
    return tiff.date(*ifd0, kTagDateTime);
  }
} // namespace

std::optional<std::chrono::system_clock::time_point> parseDateTaken(std::string_view bytes) {
  if (bytes.starts_with("II*") || bytes.starts_with("MM")) {
    return fromTiff(bytes);
  }
  if (bytes.size() < 4 || static_cast<std::uint8_t>(bytes[0]) != 0xff || static_cast<std::uint8_t>(bytes[1]) != 0xd8) {
    return std::nullopt;
  }
  // JPEG: walk the segments to APP1 "Exif\0\0".
  std::size_t at = 2;
  while (at + 4 <= bytes.size()) {
    if (static_cast<std::uint8_t>(bytes[at]) != 0xff) {
      return std::nullopt;
    }
    const auto marker = static_cast<std::uint8_t>(bytes[at + 1]);
    if (marker == 0xda || marker == 0xd9) { // image data or the end: no EXIF before it
      return std::nullopt;
    }
    const std::size_t length = (std::size_t{static_cast<std::uint8_t>(bytes[at + 2])} << 8) |
                               static_cast<std::uint8_t>(bytes[at + 3]);
    if (length < 2 || at + 2 + length > bytes.size()) {
      return std::nullopt;
    }
    const std::string_view body = bytes.substr(at + 4, length - 2);
    if (marker == 0xe1 && body.size() > 6 && body.starts_with(std::string_view("Exif\0\0", 6))) {
      return fromTiff(body.substr(6));
    }
    at += 2 + length;
  }
  return std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> readDateTaken(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string bytes(kReadLimit, '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(in.gcount()));
  return parseDateTaken(bytes);
}

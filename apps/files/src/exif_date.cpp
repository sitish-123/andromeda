#include "exif_date.h"

#include <array>
#include <cmath>
#include <utility>
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

    struct Entry {
      std::uint16_t type = 0;
      std::uint32_t count = 0;
      std::size_t at = 0; // where the value bytes start
    };
    // Type-aware: a value of 4 bytes or less sits in the entry, a longer one at the offset stored there.
    [[nodiscard]] std::optional<Entry> entry(std::uint32_t ifd, std::uint16_t tag) const {
      const auto entries = u16(ifd);
      if (!entries || *entries > kMaxEntries) {
        return std::nullopt;
      }
      for (std::uint32_t i = 0; i < *entries; ++i) {
        const std::size_t at = ifd + 2 + std::size_t{i} * 12;
        if (u16(at) != tag) {
          continue;
        }
        const auto type = u16(at + 2);
        const auto count = u32(at + 4);
        if (!type || !count || *count == 0 || *count > 4096) {
          return std::nullopt;
        }
        std::size_t unit = 0;
        switch (*type) {
        case 1: case 2: case 7: unit = 1; break; // BYTE, ASCII, UNDEFINED
        case 3: unit = 2; break;                 // SHORT
        case 4: case 9: unit = 4; break;         // LONG, SLONG
        case 5: case 10: unit = 8; break;        // RATIONAL, SRATIONAL
        default: return std::nullopt;
        }
        const std::size_t bytes = unit * *count;
        std::size_t value = at + 8;
        if (bytes > 4) {
          const auto offset = u32(at + 8);
          if (!offset) {
            return std::nullopt;
          }
          value = *offset;
        }
        if (value + bytes > data.size()) {
          return std::nullopt;
        }
        return Entry{.type = *type, .count = *count, .at = value};
      }
      return std::nullopt;
    }
    [[nodiscard]] std::optional<double> rational(const Entry& e, std::uint32_t index) const {
      if ((e.type != 5 && e.type != 10) || index >= e.count) {
        return std::nullopt;
      }
      const auto num = u32(e.at + std::size_t{index} * 8);
      const auto den = u32(e.at + std::size_t{index} * 8 + 4);
      if (!num || !den || *den == 0) {
        return std::nullopt;
      }
      if (e.type == 10) {
        return static_cast<double>(static_cast<std::int32_t>(*num)) / static_cast<std::int32_t>(*den);
      }
      return static_cast<double>(*num) / *den;
    }
    [[nodiscard]] std::optional<double> rational(std::uint32_t ifd, std::uint16_t tag) const {
      const auto e = entry(ifd, tag);
      return e ? rational(*e, 0) : std::nullopt;
    }
    // Printable ASCII only, trimmed, at most 64 bytes: the text reaches the UI.
    [[nodiscard]] std::string ascii(std::uint32_t ifd, std::uint16_t tag) const {
      const auto e = entry(ifd, tag);
      if (!e || e->type != 2) {
        return {};
      }
      std::string out;
      for (std::size_t i = 0; i < e->count && out.size() < 64; ++i) {
        const char c = data[e->at + i];
        if (c == '\0') {
          break;
        }
        if (c >= 0x20 && c < 0x7f) {
          out += c;
        }
      }
      while (!out.empty() && out.back() == ' ') {
        out.pop_back();
      }
      return out;
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

namespace {
  constexpr std::uint16_t kTagOrientation = 0x0112;

  // The TIFF block of a JPEG's APP1 "Exif\0\0" segment: its start in `bytes` and its length (the segment's end).
  std::optional<std::pair<std::size_t, std::size_t>> jpegTiff(std::string_view bytes) {
    if (bytes.size() < 4 || static_cast<std::uint8_t>(bytes[0]) != 0xff || static_cast<std::uint8_t>(bytes[1]) != 0xd8) {
      return std::nullopt;
    }
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
        return std::pair{at + 4 + 6, body.size() - 6};
      }
      at += 2 + length;
    }
    return std::nullopt;
  }

  std::string readHead(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return {};
    }
    std::string bytes(kReadLimit, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return bytes;
  }
} // namespace

std::optional<std::chrono::system_clock::time_point> parseDateTaken(std::string_view bytes) {
  if (bytes.starts_with("II*") || bytes.starts_with("MM")) {
    return fromTiff(bytes);
  }
  const auto tiff = jpegTiff(bytes);
  return tiff ? fromTiff(bytes.substr(tiff->first, tiff->second)) : std::nullopt;
}

std::optional<std::size_t> jpegOrientationOffset(std::string_view bytes) {
  const auto block = jpegTiff(bytes);
  if (!block) {
    return std::nullopt;
  }
  // Bounded to the APP1 segment: a bad IFD offset must never reach the image data this function's caller writes to.
  const std::string_view data = bytes.substr(block->first, block->second);
  if (data.size() < 8 || !(data.starts_with("II*\0") || data.starts_with(std::string_view("MM\0*", 4)))) {
    return std::nullopt;
  }
  Tiff tiff{.data = data, .little = data[0] == 'I'};
  const auto ifd0 = tiff.u32(4);
  const auto where = ifd0 ? tiff.find(*ifd0, kTagOrientation) : std::nullopt;
  // A SHORT with count 1: the value sits in the entry, 8 bytes after its tag; the type is 2 bytes after the tag.
  if (!where || where->second != 1 || where->first < 6 || tiff.u16(where->first - 6) != std::uint16_t{3}) {
    return std::nullopt;
  }
  const auto value = tiff.u16(where->first);
  if (!value || *value < 1 || *value > 8) {
    return std::nullopt;
  }
  return block->first + where->first;
}

bool rotateJpegClockwise(const std::filesystem::path& file) {
  const std::string head = readHead(file);
  const auto at = jpegOrientationOffset(head);
  if (!at) {
    return false;
  }
  const bool little = head[jpegTiff(head)->first] == 'I';
  const auto b0 = static_cast<std::uint8_t>(head[*at]);
  const auto b1 = static_cast<std::uint8_t>(head[*at + 1]);
  const int value = little ? b0 | (b1 << 8) : (b0 << 8) | b1;
  // EXIF orientation after one more quarter turn clockwise (the mirrored ones keep their mirror).
  constexpr std::array<std::uint8_t, 9> kClockwise{0, 6, 7, 8, 5, 2, 3, 4, 1};
  const std::uint8_t next = kClockwise[static_cast<std::size_t>(value)];
  const std::array<char, 2> out = little ? std::array<char, 2>{static_cast<char>(next), 0}
                                         : std::array<char, 2>{0, static_cast<char>(next)};
  std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
  if (!io) {
    return false;
  }
  io.seekp(static_cast<std::streamoff>(*at));
  io.write(out.data(), out.size());
  return static_cast<bool>(io.flush());
}

std::optional<std::chrono::system_clock::time_point> readDateTaken(const std::filesystem::path& file) {
  return parseDateTaken(readHead(file));
}

namespace {
  constexpr std::uint16_t kTagMake = 0x010f;
  constexpr std::uint16_t kTagModel = 0x0110;
  constexpr std::uint16_t kTagGpsIfd = 0x8825;
  constexpr std::uint16_t kTagExposure = 0x829a;
  constexpr std::uint16_t kTagFNumber = 0x829d;
  constexpr std::uint16_t kTagIso = 0x8827;
  constexpr std::uint16_t kTagFocal = 0x920a;

  std::optional<double> gpsDegrees(const Tiff& tiff, std::uint32_t gps, std::uint16_t refTag, std::uint16_t tag,
                                   char negative) {
    const auto e = tiff.entry(gps, tag);
    const std::string ref = tiff.ascii(gps, refTag);
    if (!e || e->count < 3 || ref.empty()) {
      return std::nullopt;
    }
    const auto d = tiff.rational(*e, 0);
    const auto m = tiff.rational(*e, 1);
    const auto sec = tiff.rational(*e, 2);
    if (!d || !m || !sec) {
      return std::nullopt;
    }
    const double value = *d + *m / 60.0 + *sec / 3600.0;
    return ref.front() == negative ? -value : value;
  }

  PhotoExif photoFromTiff(std::string_view data) {
    PhotoExif out;
    if (data.size() < 8 || !(data.starts_with("II*\0") || data.starts_with(std::string_view("MM\0*", 4)))) {
      return out;
    }
    const Tiff tiff{.data = data, .little = data[0] == 'I'};
    const auto ifd0 = tiff.u32(4);
    if (!ifd0) {
      return out;
    }
    const std::string make = tiff.ascii(*ifd0, kTagMake);
    const std::string model = tiff.ascii(*ifd0, kTagModel);
    out.camera = make.empty() || model.starts_with(make) ? model : model.empty() ? make : make + " " + model;
    out.taken = fromTiff(data);
    if (const auto exif = tiff.entry(*ifd0, kTagExifIfd); exif && exif->type == 4) {
      if (const auto ifd = tiff.u32(exif->at)) {
        out.fNumber = tiff.rational(*ifd, kTagFNumber);
        out.exposureSeconds = tiff.rational(*ifd, kTagExposure);
        out.focalMm = tiff.rational(*ifd, kTagFocal);
        const auto dimension = [&](std::uint16_t tag) {
          const auto e = tiff.entry(*ifd, tag);
          const auto v = !e ? std::nullopt
                         : e->type == 3 ? tiff.u16(e->at).transform([](std::uint16_t x) { return std::uint32_t{x}; })
                         : e->type == 4 ? tiff.u32(e->at)
                                        : std::nullopt;
          return v && *v <= 1'000'000 ? static_cast<int>(*v) : 0;
        };
        out.width = dimension(0xa002);
        out.height = dimension(0xa003);
        if (const auto iso = tiff.entry(*ifd, kTagIso); iso && iso->type == 3) {
          if (const auto v = tiff.u16(iso->at); v && *v > 0) {
            out.iso = *v;
          }
        }
      }
    }
    if (const auto gpsPtr = tiff.entry(*ifd0, kTagGpsIfd); gpsPtr && gpsPtr->type == 4) {
      if (const auto gps = tiff.u32(gpsPtr->at)) {
        const auto lat = gpsDegrees(tiff, *gps, 1, 2, 'S');
        const auto lon = gpsDegrees(tiff, *gps, 3, 4, 'W');
        if (lat && lon && std::abs(*lat) <= 90.0 && std::abs(*lon) <= 180.0 && (*lat != 0.0 || *lon != 0.0)) {
          out.gps = std::pair{*lat, *lon};
        }
      }
    }
    return out;
  }
} // namespace

PhotoExif parsePhotoExif(std::string_view bytes) {
  if (bytes.starts_with("II*") || bytes.starts_with("MM")) {
    return photoFromTiff(bytes);
  }
  const auto tiff = jpegTiff(bytes);
  return tiff ? photoFromTiff(bytes.substr(tiff->first, tiff->second)) : PhotoExif{};
}

PhotoExif readPhotoExif(const std::filesystem::path& file) { return parsePhotoExif(readHead(file)); }

#include "core/files/media_info.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
  constexpr std::size_t kHeadBytes = 256 * 1024;   // ID3 tags with art can be large; the facts sit before the art
  constexpr std::uint64_t kMoovMax = 16ULL << 20; // a moov box past this is not read

  using Bytes = std::span<const unsigned char>;

  std::vector<unsigned char> readAt(std::ifstream& in, std::uint64_t offset, std::size_t len) {
    std::vector<unsigned char> b(len);
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(len));
    b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return b;
  }

  std::uint32_t be16(Bytes b, std::size_t i) { return (std::uint32_t{b[i]} << 8U) | b[i + 1]; }
  std::uint32_t be32(Bytes b, std::size_t i) { return (be16(b, i) << 16U) | be16(b, i + 2); }
  std::uint64_t be64(Bytes b, std::size_t i) { return (std::uint64_t{be32(b, i)} << 32U) | be32(b, i + 4); }
  std::uint32_t le16(Bytes b, std::size_t i) { return b[i] | (std::uint32_t{b[i + 1]} << 8U); }
  std::uint32_t le32(Bytes b, std::size_t i) { return le16(b, i) | (le16(b, i + 2) << 16U); }
  bool has(Bytes b, std::size_t i, std::size_t len) { return i <= b.size() && len <= b.size() - i; }
  bool tag(Bytes b, std::size_t i, std::string_view t) {
    return has(b, i, t.size()) && std::memcmp(b.data() + i, t.data(), t.size()) == 0;
  }

  void appendUtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6U));
      out += static_cast<char>(0x80 | (c & 0x3FU));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12U));
      out += static_cast<char>(0x80 | ((c >> 6U) & 0x3FU));
      out += static_cast<char>(0x80 | (c & 0x3FU));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18U));
      out += static_cast<char>(0x80 | ((c >> 12U) & 0x3FU));
      out += static_cast<char>(0x80 | ((c >> 6U) & 0x3FU));
      out += static_cast<char>(0x80 | (c & 0x3FU));
    }
  }

  // An ID3v2 text frame body: encoding byte, then Latin-1, UTF-16 (BOM), UTF-16BE or UTF-8; up to the first NUL.
  std::string id3Text(Bytes b) {
    if (b.empty()) {
      return {};
    }
    const unsigned char enc = b[0];
    b = b.subspan(1);
    std::string out;
    if (enc == 0 || enc == 3) {
      for (unsigned char c : b) {
        if (c == 0) {
          break;
        }
        if (enc == 0) {
          appendUtf8(out, c);
        } else {
          out += static_cast<char>(c);
        }
      }
      return out;
    }
    bool little = false;
    std::size_t i = 0;
    if (enc == 1 && b.size() >= 2) {
      little = b[0] == 0xFF && b[1] == 0xFE;
      i = 2;
    }
    for (; i + 1 < b.size(); i += 2) {
      std::uint32_t u = little ? le16(b, i) : be16(b, i);
      if (u == 0) {
        break;
      }
      if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
        const std::uint32_t lo = little ? le16(b, i + 2) : be16(b, i + 2);
        if (lo >= 0xDC00 && lo < 0xE000) {
          u = 0x10000 + ((u - 0xD800) << 10U) + (lo - 0xDC00);
          i += 2;
        }
      }
      appendUtf8(out, u);
    }
    return out;
  }

  void readMp3(Bytes b, std::uint64_t fileSize, MediaInfo& info) {
    std::size_t audio = 0;
    if (tag(b, 0, "ID3") && has(b, 0, 10)) {
      const unsigned version = b[3];
      const std::size_t tagSize = (std::size_t{b[6]} << 21U) | (std::size_t{b[7]} << 14U) | (std::size_t{b[8]} << 7U) | b[9];
      audio = 10 + tagSize;
      for (std::size_t i = 10; version >= 3 && has(b, i, 10) && i < audio;) {
        const std::size_t len = version == 4 ? (std::size_t{b[i + 4]} << 21U) | (std::size_t{b[i + 5]} << 14U) |
                                                   (std::size_t{b[i + 6]} << 7U) | b[i + 7]
                                             : be32(b, i + 4);
        if (b[i] == 0 || len == 0 || !has(b, i + 10, len)) {
          break;
        }
        if (tag(b, i, "TPE1") && info.artist.empty()) {
          info.artist = id3Text(b.subspan(i + 10, len));
        }
        i += 10 + len;
      }
    }
    // The first frame header after the tag: MPEG version, bitrate and sample rate; a Xing/Info header gives frames.
    for (std::size_t i = audio; has(b, i, 4) && i < audio + 4096; ++i) {
      if (b[i] != 0xFF || (b[i + 1] & 0xE0U) != 0xE0U || ((b[i + 1] >> 1U) & 3U) != 1U) { // layer III only
        continue;
      }
      const unsigned ver = (b[i + 1] >> 3U) & 3U; // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
      const unsigned rateIndex = (b[i + 2] >> 2U) & 3U;
      const unsigned bitIndex = b[i + 2] >> 4U;
      if (ver == 1 || rateIndex == 3 || bitIndex == 0 || bitIndex == 15) {
        continue;
      }
      static constexpr std::array<unsigned, 15> kV1{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
      static constexpr std::array<unsigned, 15> kV2{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
      static constexpr std::array<unsigned, 3> kRates{44100, 48000, 32000};
      const unsigned rate = kRates[rateIndex] >> (ver == 3 ? 0U : ver == 2 ? 1U : 2U);
      const unsigned kbps = ver == 3 ? kV1[bitIndex] : kV2[bitIndex];
      const bool mono = (b[i + 3] >> 6U) == 3U;
      const std::size_t side = ver == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
      const std::size_t x = i + 4 + side;
      if ((tag(b, x, "Xing") || tag(b, x, "Info")) && has(b, x, 12) && (be32(b, x + 4) & 1U) != 0) {
        const double samples = ver == 3 ? 1152.0 : 576.0;
        info.seconds = be32(b, x + 8) * samples / rate;
      } else if (fileSize > i) {
        info.seconds = static_cast<double>(fileSize - i) * 8.0 / (kbps * 1000.0);
      }
      return;
    }
  }

  void readFlac(Bytes b, MediaInfo& info) {
    for (std::size_t i = 4; has(b, i, 4);) {
      const unsigned type = b[i] & 0x7FU;
      const bool last = (b[i] & 0x80U) != 0;
      const std::size_t len = (std::size_t{b[i + 1]} << 16U) | (std::size_t{b[i + 2]} << 8U) | b[i + 3];
      const std::size_t body = i + 4;
      if (!has(b, body, len)) {
        return;
      }
      if (type == 0 && len >= 18) {
        const std::uint32_t rate = (be32(b, body + 10) >> 12U);
        const std::uint64_t total = ((std::uint64_t{b[body + 13]} & 0x0FU) << 32U) | be32(b, body + 14);
        if (rate > 0 && total > 0) {
          info.seconds = static_cast<double>(total) / rate;
        }
      } else if (type == 4 && len >= 8) {
        Bytes c = b.subspan(body, len);
        std::size_t j = 4 + std::size_t{le32(c, 0)};
        if (has(c, j, 4)) {
          const std::uint32_t count = le32(c, j);
          j += 4;
          for (std::uint32_t k = 0; k < count && has(c, j, 4); ++k) {
            const std::size_t n = le32(c, j);
            j += 4;
            if (!has(c, j, n)) {
              break;
            }
            const std::string_view kv(reinterpret_cast<const char*>(c.data() + j), n);
            if (kv.size() > 7 && (kv.starts_with("ARTIST=") || kv.starts_with("artist=")) && info.artist.empty()) {
              info.artist = std::string(kv.substr(7));
            }
            j += n;
          }
        }
      }
      if (last) {
        return;
      }
      i = body + len;
    }
  }

  void readWav(Bytes b, MediaInfo& info) {
    std::uint32_t byteRate = 0;
    for (std::size_t i = 12; has(b, i, 8);) {
      const std::size_t len = le32(b, i + 4);
      if (tag(b, i, "fmt ") && has(b, i + 8, 12)) {
        byteRate = le32(b, i + 16);
      } else if (tag(b, i, "data")) {
        if (byteRate > 0) {
          info.seconds = static_cast<double>(len) / byteRate;
        }
        return;
      }
      i += 8 + len + (len & 1U);
    }
  }

  // Matroska / WebM: EBML ids keep their length marker; sizes drop it. Unknown sizes (all ones) run to the end.
  struct Ebml {
    Bytes b;
    std::size_t i = 0;
    bool vint(bool keepMarker, std::uint64_t& out) {
      if (!has(b, i, 1) || b[i] == 0) {
        return false;
      }
      const int len = std::countl_zero(static_cast<std::uint8_t>(b[i])) + 1;
      if (!has(b, i, static_cast<std::size_t>(len))) {
        return false;
      }
      std::uint64_t v = keepMarker ? b[i] : (b[i] & (0xFFU >> static_cast<unsigned>(len)));
      bool allOnes = v == (0xFFU >> static_cast<unsigned>(len));
      for (int k = 1; k < len; ++k) {
        v = (v << 8U) | b[i + static_cast<std::size_t>(k)];
        allOnes = allOnes && b[i + static_cast<std::size_t>(k)] == 0xFF;
      }
      i += static_cast<std::size_t>(len);
      out = (!keepMarker && allOnes) ? UINT64_MAX : v;
      return true;
    }
  };

  std::uint64_t uintOf(Bytes b) {
    std::uint64_t v = 0;
    for (unsigned char c : b.first(std::min<std::size_t>(b.size(), 8))) {
      v = (v << 8U) | c;
    }
    return v;
  }

  void readMatroska(Bytes b, MediaInfo& info) {
    std::uint64_t scale = 1'000'000;
    std::optional<double> duration;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    Ebml e{.b = b};
    while (e.i < b.size()) {
      std::uint64_t id = 0;
      std::uint64_t size = 0;
      if (!e.vint(true, id) || !e.vint(false, size)) {
        break;
      }
      // Containers to walk into: Segment, Info, Tracks, TrackEntry, Video. A Cluster ends the header part.
      if (id == 0x18538067 || id == 0x1549A966 || id == 0x1654AE6B || id == 0xAE || id == 0xE0) {
        continue;
      }
      if (id == 0x1F43B675 || size == UINT64_MAX || !has(b, e.i, size)) {
        break;
      }
      const Bytes body = b.subspan(e.i, size);
      if (id == 0x2AD7B1) {
        scale = uintOf(body);
      } else if (id == 0x4489 && (size == 4 || size == 8)) {
        if (size == 4) {
          const std::uint32_t raw = be32(body, 0);
          float f = 0;
          std::memcpy(&f, &raw, 4);
          duration = f;
        } else {
          const std::uint64_t raw = be64(body, 0);
          double d = 0;
          std::memcpy(&d, &raw, 8);
          duration = d;
        }
      } else if (id == 0xB0 && w == 0) {
        w = static_cast<std::uint32_t>(uintOf(body));
      } else if (id == 0xBA && h == 0) {
        h = static_cast<std::uint32_t>(uintOf(body));
      }
      e.i += size;
    }
    if (duration && std::isfinite(*duration) && *duration > 0) {
      info.seconds = *duration * static_cast<double>(scale) / 1e9;
    }
    if (w > 0 && h > 0) {
      info.size = std::pair{w, h};
    }
  }

  // MP4 / MOV / M4A: moov > mvhd (duration), trak > tkhd (size), udta > meta > ilst > ©ART > data (artist).
  // Real files nest moov > trak > … > ilst about 6 deep; a crafted file must not recurse until the stack runs out.
  constexpr int kMp4MaxDepth = 16;

  void walkMp4(Bytes b, MediaInfo& info, int depth = 0) {
    if (depth > kMp4MaxDepth) {
      return;
    }
    for (std::size_t i = 0; has(b, i, 8);) {
      std::uint64_t size = be32(b, i);
      std::size_t head = 8;
      if (size == 1 && has(b, i, 16)) {
        size = be64(b, i + 8);
        head = 16;
      } else if (size == 0) {
        size = b.size() - i;
      }
      if (size < head || !has(b, i, size)) {
        return;
      }
      const Bytes body = b.subspan(i + head, size - head);
      if (tag(b, i + 4, "trak") || tag(b, i + 4, "udta") || tag(b, i + 4, "ilst")) {
        walkMp4(body, info, depth + 1);
      } else if (tag(b, i + 4, "meta") && body.size() > 4) {
        walkMp4(body.subspan(4), info, depth + 1); // a full box: version and flags first
      } else if (tag(b, i + 4, "mvhd") && !body.empty()) {
        const bool v1 = body[0] == 1;
        if (has(body, 0, v1 ? 32 : 20)) {
          const std::uint32_t timescale = be32(body, v1 ? 20 : 12);
          const std::uint64_t d = v1 ? be64(body, 24) : be32(body, 16);
          if (timescale > 0 && d > 0) {
            info.seconds = static_cast<double>(d) / timescale;
          }
        }
      } else if (tag(b, i + 4, "tkhd") && body.size() >= 8 && !info.size) {
        const std::uint32_t w = be32(body, body.size() - 8) >> 16U;
        const std::uint32_t h = be32(body, body.size() - 4) >> 16U;
        if (w > 0 && h > 0) {
          info.size = std::pair{w, h};
        }
      } else if (has(b, i + 4, 4) && b[i + 4] == 0xA9 && tag(b, i + 5, "ART") && info.artist.empty()) {
        // data box: size, "data", type, locale, then UTF-8 text.
        if (has(body, 0, 16) && tag(body, 4, "data")) {
          const std::size_t n = std::min<std::size_t>(be32(body, 0), body.size());
          if (n > 16) {
            info.artist.assign(reinterpret_cast<const char*>(body.data() + 16), n - 16);
          }
        }
      }
      i += size;
    }
  }

  void readMp4(std::ifstream& in, std::uint64_t fileSize, MediaInfo& info) {
    for (std::uint64_t at = 0; at + 8 <= fileSize;) {
      const auto h = readAt(in, at, 16);
      if (h.size() < 8) {
        return;
      }
      std::uint64_t size = be32(h, 0);
      std::uint64_t head = 8;
      if (size == 1 && h.size() == 16) {
        size = be64(h, 8);
        head = 16;
      } else if (size == 0) {
        size = fileSize - at;
      }
      // Untrusted: a box must fit in what is left of the file, or `at` could wrap around and the walk never end.
      if (size < head || size > fileSize - at) {
        return;
      }
      if (tag(h, 4, "moov")) {
        if (size - head <= kMoovMax) {
          const auto moov = readAt(in, at + head, static_cast<std::size_t>(size - head));
          walkMp4(moov, info);
        }
        return;
      }
      at += size;
    }
  }
} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> readImageSize(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  const auto v = readAt(in, 0, kHeadBytes); // a JPEG SOF can follow a large EXIF block
  const Bytes b(v);
  if (has(b, 0, 24) && b[0] == 0x89 && tag(b, 1, "PNG")) {
    return std::pair{be32(b, 16), be32(b, 20)};
  }
  if (has(b, 0, 10) && tag(b, 0, "GIF")) {
    return std::pair{le16(b, 6), le16(b, 8)};
  }
  if (has(b, 0, 30) && tag(b, 0, "RIFF") && tag(b, 8, "WEBPVP8")) {
    if (b[15] == ' ') {
      return std::pair{le16(b, 26) & 0x3FFFU, le16(b, 28) & 0x3FFFU};
    }
    if (b[15] == 'L') {
      const std::uint32_t x = le32(b, 21);
      return std::pair{(x & 0x3FFFU) + 1, ((x >> 14U) & 0x3FFFU) + 1};
    }
    if (b[15] == 'X') {
      return std::pair{(le32(b, 24) & 0xFFFFFFU) + 1, (le32(b, 27) & 0xFFFFFFU) + 1};
    }
  }
  if (has(b, 0, 4) && b[0] == 0xFF && b[1] == 0xD8) {
    std::size_t i = 2;
    while (has(b, i, 9) && b[i] == 0xFF) {
      const unsigned char marker = b[i + 1];
      // SOF0-SOF15 carry the size, except DHT (C4), JPG (C8) and DAC (CC).
      if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
        return std::pair{be16(b, i + 7), be16(b, i + 5)};
      }
      i += 2 + be16(b, i + 2);
    }
  }
  return std::nullopt;
}

MediaInfo readMediaInfo(const fs::path& file) {
  MediaInfo info;
  std::error_code ec;
  const std::uint64_t fileSize = fs::file_size(file, ec);
  if (ec) {
    return info;
  }
  std::ifstream in(file, std::ios::binary);
  const auto head = readAt(in, 0, kHeadBytes);
  const Bytes b(head);
  if (tag(b, 4, "ftyp")) {
    readMp4(in, fileSize, info);
  } else if (has(b, 0, 4) && be32(b, 0) == 0x1A45DFA3) {
    readMatroska(b, info);
  } else if (tag(b, 0, "fLaC")) {
    readFlac(b, info);
  } else if (tag(b, 0, "RIFF") && tag(b, 8, "WAVE")) {
    readWav(b, info);
  } else if (tag(b, 0, "ID3") || (has(b, 0, 2) && b[0] == 0xFF && (b[1] & 0xE0U) == 0xE0U)) {
    readMp3(b, fileSize, info);
  } else {
    info.size = readImageSize(file);
  }
  return info;
}

std::string formatDuration(double seconds) {
  const auto total = static_cast<long long>(std::llround(std::max(0.0, seconds)));
  const long long h = total / 3600;
  const long long m = (total / 60) % 60;
  const long long s = total % 60;
  return h > 0 ? std::format("{}:{:02}:{:02}", h, m, s) : std::format("{}:{:02}", m, s);
}

#include "exif_date.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "exif_date_test: {}", what);
      ++g_failures;
    }
  }

  void put16(std::string& s, std::uint16_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>(v >> 8);
  }
  void put32(std::string& s, std::uint32_t v) {
    put16(s, static_cast<std::uint16_t>(v & 0xffff));
    put16(s, static_cast<std::uint16_t>(v >> 16));
  }

  // Little-endian TIFF: IFD0 with an Exif IFD pointer, the Exif IFD with DateTimeOriginal.
  std::string tiff(const char* date) {
    std::string t = "II*";
    t += '\0';
    put32(t, 8);   // IFD0 at 8
    put16(t, 1);   // one entry
    put16(t, 0x8769);
    put16(t, 4);   // LONG
    put32(t, 1);
    put32(t, 26);  // Exif IFD at 26
    put32(t, 0);   // no next IFD
    put16(t, 1);   // Exif IFD: one entry
    put16(t, 0x9003);
    put16(t, 2);   // ASCII
    put32(t, 20);
    put32(t, 44);  // the string at 44
    put32(t, 0);
    t += date;
    t += '\0';
    return t;
  }

  std::string jpeg(const std::string& tiffBytes) {
    std::string body = std::string("Exif\0\0", 6) + tiffBytes;
    std::string j = "\xff\xd8\xff\xe1";
    const std::size_t len = body.size() + 2;
    j += static_cast<char>(len >> 8);
    j += static_cast<char>(len & 0xff);
    j += body;
    j += "\xff\xda";
    return j;
  }

  // Parses `bytes` placed at the very end of a page followed by an unreadable page: reading past the buffer faults.
  std::optional<std::chrono::system_clock::time_point> guarded(const std::string& bytes) {
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const std::size_t pages = (bytes.size() + page - 1) / page + 1;
    auto* base = static_cast<char*>(::mmap(nullptr, (pages + 1) * page, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ::mprotect(base + pages * page, page, PROT_NONE);
    char* start = base + pages * page - bytes.size();
    std::memcpy(start, bytes.data(), bytes.size());
    auto out = parseDateTaken(std::string_view(start, bytes.size()));
    ::munmap(base, (pages + 1) * page);
    return out;
  }

  std::string ymd(std::chrono::system_clock::time_point t) {
    return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(t));
  }
} // namespace

int main() {
  const std::string good = jpeg(tiff("2026:10:02 08:15:00"));
  const auto taken = guarded(good);
  expect(taken.has_value() && ymd(*taken) == "2026-10-02 08:15:00", "a JPEG with DateTimeOriginal reads its date");
  expect(guarded(tiff("2026:10:02 08:15:00")).has_value(), "a bare TIFF reads its date");

  // Every truncation of a good file: never a read past the end, and no date once the string is cut.
  for (std::size_t n = 0; n < good.size() - 2; ++n) {
    const auto cut = guarded(good.substr(0, n));
    expect(!cut.has_value() || n >= good.size() - 3, std::format("truncated at {} gave a date", n));
  }

  // Malformed: offsets and counts that point outside the buffer, or nonsense values.
  std::string t = tiff("2026:10:02 08:15:00");
  std::string badIfd0 = t;
  badIfd0[4] = '\xff';
  badIfd0[5] = '\xff';
  expect(!guarded(jpeg(badIfd0)).has_value(), "an IFD0 offset past the end");
  std::string badExif = t;
  badExif[18] = '\xf0'; // Exif IFD offset low byte
  badExif[19] = '\xff';
  expect(!guarded(jpeg(badExif)).has_value(), "an Exif IFD offset past the end");
  std::string hugeCount = t;
  hugeCount[26] = '\xff'; // Exif IFD entry count 0xffff
  hugeCount[27] = '\xff';
  expect(!guarded(jpeg(hugeCount)).has_value(), "an entry count past the end");
  std::string badString = t;
  badString[36] = '\xfa'; // string offset 0xfffa
  badString[37] = '\xff';
  expect(!guarded(jpeg(badString)).has_value(), "a string offset past the end");
  expect(!guarded(jpeg(tiff("2026:13:40 99:99:99"))).has_value(), "an impossible date");
  expect(!guarded(jpeg(tiff("not a date at all!!"))).has_value(), "text that is no date");
  std::string longSegment = good;
  longSegment[4] = '\x7f'; // APP1 length larger than the file
  expect(!guarded(longSegment).has_value(), "a segment length past the end");
  expect(!guarded("\xff\xd8").has_value(), "a JPEG with only its start marker");
  expect(!guarded("").has_value(), "an empty file");
  return g_failures == 0 ? 0 : 1;
}

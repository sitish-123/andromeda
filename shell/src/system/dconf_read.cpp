#include "system/dconf_read.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glib.h>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace {

  constexpr std::size_t kMaxDatabaseBytes = 64U * 1024U * 1024U;
  constexpr std::size_t kHeaderBytes = 24;
  constexpr std::size_t kItemBytes = 24;
  constexpr std::uint32_t kNoParent = 0xffffffffU;
  constexpr std::uint32_t kBloomCountMask = (1U << 27U) - 1U;

  // GVDB file layout: header {"GVariant" signature, version, options, root pointer {start, end}}; the root is a
  // hash table {bloom count, bucket count, bloom words, buckets, items}; each item is {hash, parent index,
  // key start, key size (u16), type, pad, value pointer {start, end}}. A key segment is relative to its parent.
  class Gvdb {
  public:
    explicit Gvdb(std::string data) : m_data(std::move(data)) {}

    bool open() {
      if (m_data.size() < kHeaderBytes) {
        return false;
      }
      if (std::memcmp(m_data.data(), "GVariant", 8) == 0) {
        m_swapped = false;
      } else if (std::memcmp(m_data.data(), "raVGtnai", 8) == 0) {
        m_swapped = true;
      } else {
        return false;
      }
      const std::uint32_t rootStart = u32(16);
      const std::uint32_t rootEnd = u32(20);
      if (rootStart > rootEnd || rootEnd > m_data.size() || rootEnd - rootStart < 8) {
        return false;
      }
      const std::uint64_t bloomWords = u32(rootStart) & kBloomCountMask;
      const std::uint64_t buckets = u32(rootStart + 4);
      const std::uint64_t itemsStart = rootStart + 8ULL + (bloomWords + buckets) * 4ULL;
      if (itemsStart > rootEnd) {
        return false;
      }
      m_itemsStart = static_cast<std::size_t>(itemsStart);
      m_itemCount = (rootEnd - m_itemsStart) / kItemBytes;
      return true;
    }

    std::optional<std::string> readString(std::string_view key) const {
      for (std::size_t i = 0; i < m_itemCount; ++i) {
        const std::size_t item = m_itemsStart + (i * kItemBytes);
        if (m_data[item + 14] != 'v' || !keyMatches(i, key)) {
          continue;
        }
        return decodeString(u32(item + 16), u32(item + 20));
      }
      return std::nullopt;
    }

  private:
    std::uint32_t u32(std::size_t offset) const {
      std::uint32_t value = 0;
      std::memcpy(&value, m_data.data() + offset, sizeof(value));
      if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
      }
      return value;
    }

    std::uint16_t u16(std::size_t offset) const {
      std::uint16_t value = 0;
      std::memcpy(&value, m_data.data() + offset, sizeof(value));
      if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
      }
      return value;
    }

    // Real key segments are never empty; the depth bound stops a cycle of empty segments.
    bool keyMatches(std::size_t index, std::string_view key) const {
      std::size_t remaining = key.size();
      for (std::size_t depth = 0; depth <= key.size(); ++depth) {
        const std::size_t item = m_itemsStart + (index * kItemBytes);
        const std::uint32_t parent = u32(item + 4);
        const std::uint32_t start = u32(item + 8);
        const std::uint16_t size = u16(item + 12);
        if (static_cast<std::uint64_t>(start) + size > m_data.size() || size > remaining) {
          return false;
        }
        remaining -= size;
        if (key.substr(remaining, size) != std::string_view(m_data.data() + start, size)) {
          return false;
        }
        if (parent == kNoParent) {
          return remaining == 0;
        }
        if (parent >= m_itemCount) {
          return false;
        }
        index = parent;
      }
      return false;
    }

    std::optional<std::string> decodeString(std::uint32_t start, std::uint32_t end) const {
      if (start > end || end > m_data.size()) {
        return std::nullopt;
      }
      GBytes* bytes = g_bytes_new(m_data.data() + start, end - start);
      GVariant* boxed = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE_VARIANT, bytes, FALSE));
      g_bytes_unref(bytes);
      if (m_swapped != (std::endian::native == std::endian::big)) {
        GVariant* swapped = g_variant_ref_sink(g_variant_byteswap(boxed));
        g_variant_unref(boxed);
        boxed = swapped;
      }
      GVariant* value = g_variant_get_variant(boxed);
      g_variant_unref(boxed);
      std::optional<std::string> result;
      if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        result = g_variant_get_string(value, nullptr);
      }
      g_variant_unref(value);
      return result;
    }

    std::string m_data;
    // GVDB structure fields are always little-endian; the signature order says only how values are stored.
    bool m_swapped = false;
    std::size_t m_itemsStart = 0;
    std::size_t m_itemCount = 0;
  };

  std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return std::nullopt;
    }
    std::string data;
    std::array<char, 65536> chunk{};
    while (file.read(chunk.data(), chunk.size()) || file.gcount() > 0) {
      data.append(chunk.data(), static_cast<std::size_t>(file.gcount()));
      if (data.size() > kMaxDatabaseBytes) {
        return std::nullopt;
      }
    }
    return data;
  }

  fs::path configHome() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] == '/') {
      return xdg;
    }
    const char* home = std::getenv("HOME");
    return fs::path(home != nullptr ? home : "") / ".config";
  }

  std::optional<fs::path> profilePath() {
    const char* env = std::getenv("DCONF_PROFILE");
    const std::string name = env != nullptr && env[0] != '\0' ? env : "user";
    if (name.front() == '/') {
      return fs::path(name);
    }
    for (const fs::path dir : {"/etc/dconf/profile", "/usr/share/dconf/profile"}) {
      std::error_code ec;
      if (fs::is_regular_file(dir / name, ec)) {
        return dir / name;
      }
    }
    return std::nullopt;
  }

  struct Profile {
    std::vector<fs::path> databases;
    std::optional<fs::path> user;
  };

  Profile readProfile() {
    const auto profile = profilePath();
    if (!profile.has_value()) {
      const fs::path user = configHome() / "dconf" / "user";
      return {.databases = {user}, .user = user};
    }
    Profile result;
    auto& databases = result.databases;
    std::ifstream file(*profile);
    std::string line;
    while (std::getline(file, line)) {
      const std::string_view entry = std::string_view(line).substr(0, line.find('#'));
      const auto colon = entry.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      const std::string_view kind = entry.substr(0, colon);
      std::string_view name = entry.substr(colon + 1);
      while (!name.empty() && g_ascii_isspace(name.back())) {
        name.remove_suffix(1);
      }
      if (name.empty()) {
        continue;
      }
      if (kind == "user-db") {
        databases.push_back(configHome() / "dconf" / name);
        if (!result.user.has_value()) {
          result.user = databases.back();
        }
      } else if (kind == "system-db") {
        databases.push_back(fs::path("/etc/dconf/db") / name);
      } else if (kind == "file-db") {
        databases.emplace_back(name);
      }
    }
    return result;
  }

} // namespace

namespace dconf {

  std::optional<std::string> readStringFromDatabase(const fs::path& database, std::string_view key) {
    auto data = readFile(database);
    if (!data.has_value()) {
      return std::nullopt;
    }
    Gvdb gvdb(std::move(*data));
    if (!gvdb.open()) {
      return std::nullopt;
    }
    return gvdb.readString(key);
  }

  std::optional<std::string> readString(std::string_view key) {
    for (const auto& database : readProfile().databases) {
      if (auto value = readStringFromDatabase(database, key); value.has_value()) {
        return value;
      }
    }
    return std::nullopt;
  }

  std::optional<fs::path> userDatabasePath() { return readProfile().user; }

} // namespace dconf

#include "permission_access.h"

namespace {
  // Read bit of each class; write is read >> 1, run (x) is read >> 2.
  std::uint32_t readBit(PermClass cls) {
    switch (cls) {
    case PermClass::Owner:
      return 0400;
    case PermClass::Group:
      return 0040;
    case PermClass::Everyone:
      return 0004;
    }
    return 0;
  }
} // namespace

Access accessOf(std::uint32_t mode, PermClass cls) {
  const std::uint32_t read = readBit(cls);
  const bool r = (mode & read) != 0;
  const bool w = (mode & (read >> 1)) != 0;
  return r && w ? Access::ReadWrite : r ? Access::Read : w ? Access::WriteOnly : Access::None;
}

std::pair<std::uint32_t, std::uint32_t> accessBits(PermClass cls, Access access) {
  const std::uint32_t read = readBit(cls);
  const std::uint32_t write = read >> 1;
  const std::uint32_t mode = access == Access::ReadWrite ? read | write
      : access == Access::Read                          ? read
      : access == Access::WriteOnly                     ? write
                                                        : 0;
  return {mode, read | write};
}

std::pair<std::uint32_t, std::uint32_t> runBits(std::uint32_t mode, bool allow) {
  return {allow ? (mode & 0444) >> 2 : 0, 0111};
}

std::optional<Access> commonAccess(std::span<const std::uint32_t> modes, PermClass cls) {
  std::optional<Access> out;
  for (const std::uint32_t mode : modes) {
    const Access a = accessOf(mode, cls);
    if (out.has_value() && *out != a) {
      return std::nullopt;
    }
    out = a;
  }
  return out;
}

const char* accessLabel(Access access) {
  switch (access) {
  case Access::None:
    return "No access";
  case Access::Read:
    return "Read only";
  case Access::ReadWrite:
    return "Read and write";
  case Access::WriteOnly:
    return "Write only";
  }
  return "";
}

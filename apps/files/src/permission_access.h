#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

// Unix permission bits in the plain words of Get Info (mockup files-v2-live.html §11): per class No access, Read
// only, Read and write. A folder's open (x) bit follows read in andromeda-fileops, so it is not a separate choice.
enum class PermClass : std::uint8_t { Owner, Group, Everyone };
enum class Access : std::uint8_t { None, Read, ReadWrite, WriteOnly };

[[nodiscard]] Access accessOf(std::uint32_t mode, PermClass cls);
// The (mode, mask) for FileOps1.SetPermissions that gives `cls` this access and leaves the rest.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> accessBits(PermClass cls, Access access);
// "Allow running as a program": x for every class that can read (on), or no x at all (off).
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> runBits(std::uint32_t mode, bool allow);
// The access all modes share; empty when they differ ("Mixed").
[[nodiscard]] std::optional<Access> commonAccess(std::span<const std::uint32_t> modes, PermClass cls);
[[nodiscard]] const char* accessLabel(Access access);

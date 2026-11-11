#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

struct AtomicWriteTarget {
  std::filesystem::path path;
  bool throughSymlink = false;
};

[[nodiscard]] std::optional<AtomicWriteTarget> resolveAtomicWriteTarget(const std::filesystem::path& path);
// Durable fsyncs the file and its directory, so a crash keeps the old or the new content; it blocks on disk, so
// only worker-thread callers use it.
enum class AtomicWrite : bool { Fast, Durable };

[[nodiscard]] bool writeTextFileAtomic(
    const std::filesystem::path& path, std::string_view content,
    std::optional<std::filesystem::perms> mode = std::nullopt, AtomicWrite durability = AtomicWrite::Fast
);

#pragma once

#include <filesystem>
#include <string_view>

// Replaces a state file (JSON, TSV) whole or not at all: a temp file, fsync, checked rename. A full disk or a crash
// keeps the old file instead of a truncated one. Makes the parent folders. False: the old file stays; logged.
bool writeStateFile(const std::filesystem::path& path, std::string_view content);

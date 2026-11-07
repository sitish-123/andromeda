#pragma once

#include <cstdint>
#include <string>

struct FileInfo;

// "4.2 MB", "12 KB", "1 byte" (SI units, as Finder and GNOME Files).
[[nodiscard]] std::string formatFileSize(std::uint64_t bytes);
// "Today, 09:41", "3 Nov, 14:02", "3 Nov 2025".
[[nodiscard]] std::string formatFileDate(std::int64_t mtimeNs);
// "PNG image", "Folder", "PDF document": the shared-mime-info description.
[[nodiscard]] std::string fileKindLabel(const FileInfo& info);
// Symbol name for the type (until FB-ICON art replaces it).
[[nodiscard]] const char* fileTypeGlyph(const FileInfo& info);

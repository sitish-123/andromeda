#pragma once

#include "files/file_ops_client.h"
#include "ui/files/file_browser.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Node;

// An archive opened like a folder (mockup files-v2-live.html §8): the ListArchive entries as one FileBrowser
// listing per folder inside it. Row names are virtual absolute paths "<archive>/<inner path>", so selection,
// drag and the host's activate hook know which entry a row is; innerOf() maps them back.
class ArchiveListing {
public:
  ArchiveListing(std::filesystem::path archive, const std::vector<FileOpsClient::ArchiveEntry>& entries);

  // .zip, .7z, .tar and the compressed tars FileOps1 reads.
  [[nodiscard]] static bool isArchive(const std::filesystem::path& path);
  // "archive:<archive>" for the root, "archive:<archive>/<inner>" inside it.
  [[nodiscard]] static bool isArchiveId(const std::string& listingId);

  [[nodiscard]] const std::filesystem::path& archive() const noexcept { return m_archive; }
  // The rows of one folder inside the archive ("" = its top); folders that only appear as a path part get a row too.
  [[nodiscard]] FileBrowser::Listing listing(const std::filesystem::path& inner = {}) const;
  [[nodiscard]] std::string id(const std::filesystem::path& inner = {}) const;
  // The inner path of a row name; empty when the row is not in this archive.
  [[nodiscard]] std::optional<std::filesystem::path> innerOf(const std::filesystem::path& rowPath) const;
  [[nodiscard]] bool isFolder(const std::filesystem::path& inner) const;
  // Names at the top of the archive (what Extract Here looks at).
  [[nodiscard]] std::vector<std::string> topNames() const;
  [[nodiscard]] std::size_t itemCount() const noexcept { return m_items; }
  [[nodiscard]] std::uint64_t unpackedBytes() const noexcept { return m_unpacked; }

  // The bar on top of an archive listing: "ZIP archive · 248 items · 1.2 GB unpacked from 980 MB", Extract To…,
  // Extract All.
  [[nodiscard]] std::unique_ptr<Node> buildExtractBar(float scale, std::function<void()> extractTo,
                                                      std::function<void()> extractAll) const;

private:
  std::filesystem::path m_archive;
  // Folder inner path -> its rows by name.
  std::map<std::filesystem::path, std::map<std::string, std::shared_ptr<FileInfo>>> m_folders;
  std::size_t m_items = 0;
  std::uint64_t m_unpacked = 0;
};

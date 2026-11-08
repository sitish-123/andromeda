#include "archive_listing.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <gio/gio.h>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {
  constexpr std::string_view kIdPrefix = "archive:";
  constexpr std::array<std::string_view, 10> kExtensions = {".zip", ".7z",      ".tar",     ".tar.gz", ".tgz",
                                                            ".tar.xz", ".txz", ".tar.zst", ".tar.bz2", ".tbz2"};

  std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  std::string owned(gchar* text) {
    std::string out = text != nullptr ? text : "";
    g_free(text);
    return out;
  }

  std::shared_ptr<FileInfo> makeRow(const fs::path& archive, const fs::path& inner, bool isDir) {
    auto info = std::make_shared<FileInfo>();
    info->name = (archive / inner).string();
    info->displayName = inner.filename().string();
    info->collateKey = owned(g_utf8_collate_key_for_filename(info->displayName.c_str(), -1));
    info->isDir = isDir;
    info->kind = isDir ? FileKind::Directory : FileKind::File;
    info->hidden = info->displayName.starts_with('.');
    if (isDir) {
      info->contentType = "inode/directory";
    } else {
      gboolean uncertain = FALSE;
      gchar* type = g_content_type_guess(info->displayName.c_str(), nullptr, 0, &uncertain);
      gchar* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
      g_free(type);
      info->contentType = mime != nullptr ? owned(mime) : "application/octet-stream";
    }
    return info;
  }

  std::string formatLabel(const fs::path& archive) {
    const std::string name = lower(archive.filename().string());
    if (name.ends_with(".zip")) {
      return "ZIP archive";
    }
    if (name.ends_with(".7z")) {
      return "7z archive";
    }
    return "tar archive";
  }
} // namespace

ArchiveListing::ArchiveListing(fs::path archive, const std::vector<FileOpsClient::ArchiveEntry>& entries)
    : m_archive(std::move(archive)) {
  m_folders[fs::path()];
  // The folder row for `inner`, made (with its parents) when the archive has no record for it.
  const auto folder = [this](const fs::path& inner) {
    fs::path at;
    for (const fs::path& part : inner) {
      const fs::path parent = at;
      at /= part;
      auto& rows = m_folders[parent];
      if (!rows.contains(part.string())) {
        rows[part.string()] = makeRow(m_archive, at, true);
      }
      m_folders[at];
    }
    return m_folders[inner.parent_path()][inner.filename().string()];
  };
  for (const FileOpsClient::ArchiveEntry& entry : entries) {
    const fs::path inner = entry.path.lexically_normal();
    if (inner.empty() || inner == "." || inner.is_absolute()) {
      continue;
    }
    ++m_items;
    if (entry.isDir) {
      folder(inner)->mtimeNs = static_cast<std::int64_t>(entry.mtime) * 1'000'000'000;
      continue;
    }
    if (inner.has_parent_path()) {
      (void)folder(inner.parent_path());
    }
    auto row = makeRow(m_archive, inner, false);
    row->size = static_cast<std::uint64_t>(std::max<std::int64_t>(entry.size, 0));
    row->mtimeNs = static_cast<std::int64_t>(entry.mtime) * 1'000'000'000;
    row->kind = entry.kind == "link" ? FileKind::Symlink : FileKind::File;
    m_unpacked += row->size;
    // A folder shows the bytes under it.
    for (fs::path up = inner.parent_path(); !up.empty(); up = up.parent_path()) {
      m_folders[up.parent_path()][up.filename().string()]->size += row->size;
    }
    m_folders[inner.parent_path()][inner.filename().string()] = std::move(row);
  }
}

bool ArchiveListing::isArchive(const fs::path& path) {
  std::string name = lower(path.filename().string());
  // Split volumes open from part 1: photos.zip.001.
  if (name.ends_with(".001")) {
    name.resize(name.size() - 4);
  }
  return std::ranges::any_of(kExtensions, [&](std::string_view ext) { return name.size() > ext.size() && name.ends_with(ext); });
}

bool ArchiveListing::isArchiveId(const std::string& listingId) { return listingId.starts_with(kIdPrefix); }

std::string ArchiveListing::id(const fs::path& inner) const {
  return std::string(kIdPrefix) + (inner.empty() ? m_archive : m_archive / inner).string();
}

FileBrowser::Listing ArchiveListing::listing(const fs::path& inner) const {
  FileBrowser::Listing out{.id = id(inner), .title = m_archive.filename().string(), .rows = {}, .activatable = true};
  if (!inner.empty()) {
    out.title = inner.filename().string();
  }
  if (const auto it = m_folders.find(inner); it != m_folders.end()) {
    out.rows.reserve(it->second.size());
    for (const auto& [name, row] : it->second) {
      out.rows.push_back(row);
    }
  }
  return out;
}

std::optional<fs::path> ArchiveListing::innerOf(const fs::path& rowPath) const {
  const fs::path rel = rowPath.lexically_relative(m_archive);
  if (rel.empty() || *rel.begin() == "..") {
    return std::nullopt;
  }
  return rel == "." ? fs::path() : rel;
}

bool ArchiveListing::isFolder(const fs::path& inner) const { return m_folders.contains(inner); }

std::vector<std::string> ArchiveListing::topNames() const {
  std::vector<std::string> out;
  for (const auto& [name, row] : m_folders.at(fs::path())) {
    out.push_back(name);
  }
  return out;
}

std::unique_ptr<Node> ArchiveListing::buildExtractBar(float scale, std::function<void()> extractTo,
                                                      std::function<void()> extractAll) const {
  const float s = scale;
  std::error_code ec;
  const auto packed = fs::file_size(m_archive, ec);
  std::string text = std::format("{} · {} {} · {} unpacked", formatLabel(m_archive), m_items, m_items == 1 ? "item" : "items",
                                 formatFileSize(m_unpacked));
  if (!ec) {
    text += " from " + formatFileSize(packed);
  }
  auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                      .height = Tokens::sizeRowCompact * s + Tokens::spaceP8 * s});
  bar->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeCaption * s,
                           .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                           .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
  bar->addChild(ui::button({.text = std::string("Extract To…"), .variant = ButtonVariant::Default,
                            .onClick = std::move(extractTo)}));
  bar->addChild(ui::button({.text = std::string("Extract All"), .variant = ButtonVariant::Primary,
                            .onClick = std::move(extractAll)}));
  return bar;
}

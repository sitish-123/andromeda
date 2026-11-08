#include "details_pane.h"

#include "core/files/file_info.h"
#include "core/log.h"
#include "design_tokens.h"
#include "portal_sheets.h"
#include "render/core/renderer.h"
#include "tags.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <fcntl.h>
#include <format>
#include <fstream>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("details");
  constexpr float kPaneWidth = 280.0F;  // mockup .info
  constexpr float kPreviewHeight = 160.0F;
  constexpr float kKeyWidth = 88.0F;    // mockup .kv span:first-child
  constexpr float kPreviewIcon = 96.0F;
  constexpr std::size_t kHeaderBytes = 256 * 1024; // a JPEG's SOF can follow a large EXIF block

  // A file icon bound at the first layout, when the renderer is there.
  class IconNode final : public Node {
  public:
    IconNode(FileInfo info, fs::path path, ThumbnailService* thumbnails, float size)
        : m_info(std::move(info)), m_path(std::move(path)), m_thumbnails(thumbnails) {
      m_icon = static_cast<FileIcon*>(addChild(std::make_unique<FileIcon>(size)));
      setSize(size, size);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (!m_bound) {
        m_icon->bind(renderer, m_info, m_path, m_thumbnails);
        m_bound = true;
      }
      m_icon->layout(renderer);
    }

  private:
    FileInfo m_info;
    fs::path m_path;
    ThumbnailService* m_thumbnails;
    FileIcon* m_icon = nullptr;
    bool m_bound = false;
  };

  std::uint32_t be16(const unsigned char* p) { return (std::uint32_t{p[0]} << 8U) | p[1]; }
  std::uint32_t be32(const unsigned char* p) { return (be16(p) << 16U) | be16(p + 2); }
  std::uint32_t le16(const unsigned char* p) { return p[0] | (std::uint32_t{p[1]} << 8U); }
  std::uint32_t le24(const unsigned char* p) { return le16(p) | (std::uint32_t{p[2]} << 16U); }

  std::int64_t birthNs(const fs::path& path) {
    struct statx st{};
    if (::statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &st) != 0 || (st.stx_mask & STATX_BTIME) == 0) {
      return 0;
    }
    return static_cast<std::int64_t>(st.stx_btime.tv_sec) * 1'000'000'000 + st.stx_btime.tv_nsec;
  }
} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> DetailsPane::imageSize(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::vector<unsigned char> b(kHeaderBytes);
  in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
  const auto n = static_cast<std::size_t>(in.gcount());
  const auto at = [&](std::size_t i, std::size_t len) { return i + len <= n; };
  if (at(0, 24) && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
    return std::pair{be32(&b[16]), be32(&b[20])};
  }
  if (at(0, 10) && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {
    return std::pair{le16(&b[6]), le16(&b[8])};
  }
  if (at(0, 30) && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[12] == 'V' &&
      b[13] == 'P' && b[14] == '8') {
    if (b[15] == ' ') {
      return std::pair{le16(&b[26]) & 0x3FFFU, le16(&b[28]) & 0x3FFFU};
    }
    if (b[15] == 'L') {
      const std::uint32_t v = b[21] | (std::uint32_t{b[22]} << 8U) | (std::uint32_t{b[23]} << 16U) | (static_cast<std::uint32_t>(b[24]) << 24U);
      return std::pair{(v & 0x3FFFU) + 1, ((v >> 14U) & 0x3FFFU) + 1};
    }
    if (b[15] == 'X') {
      return std::pair{le24(&b[24]) + 1, le24(&b[27]) + 1};
    }
  }
  if (at(0, 4) && b[0] == 0xFF && b[1] == 0xD8) {
    std::size_t i = 2;
    while (at(i, 9) && b[i] == 0xFF) {
      const unsigned char marker = b[i + 1];
      // SOF0-SOF15 carry the size, except DHT (C4), JPG (C8) and DAC (CC).
      if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
        return std::pair{be16(&b[i + 7]), be16(&b[i + 5])};
      }
      i += 2 + be16(&b[i + 2]);
    }
  }
  return std::nullopt;
}

DetailsPane::DetailsPane(ThumbnailService* thumbnails, Actions actions)
    : m_thumbnails(thumbnails), m_actions(std::move(actions)) {}

std::unique_ptr<Node> DetailsPane::build(float scale) {
  m_scale = scale;
  // The 0.5 px sep hairline on the left edge (mockup .info border-left), then the content column.
  auto pane = ui::row({.align = FlexAlign::Stretch});
  pane->addChild(ui::column({.fill = colorSpecFromRole(ColorRole::Separator), .width = std::max(1.0F, 0.5F * scale)}));
  auto root = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * scale, .padding = Tokens::spaceP16 * scale,
                          .width = kPaneWidth * scale});
  m_root = static_cast<Flex*>(pane->addChild(std::move(root)));
  fill();
  return pane;
}

void DetailsPane::update(const fs::path& folder, std::vector<fs::path> selection) {
  if (folder == m_folder && selection == m_selection) {
    return;
  }
  m_folder = folder;
  m_selection = std::move(selection);
  fill();
}

void DetailsPane::fill() {
  if (m_root == nullptr) {
    return;
  }
  while (!m_root->children().empty()) {
    m_root->removeChild(m_root->children().back().get());
  }
  const float s = m_scale;
  const auto text = [&](std::string t, float size, FontWeight weight, ColorRole role, int lines) {
    return ui::label({.text = std::move(t), .fontSize = size, .fontWeight = weight, .color = colorSpecFromRole(role),
                      .maxLines = lines, .ellipsize = TextEllipsize::Middle});
  };
  const auto kv = [&](const char* key, std::string value) {
    auto row = ui::row({.align = FlexAlign::Start});
    row->addChild(ui::label({.text = std::string(key), .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .minWidth = kKeyWidth * s, .maxWidth = kKeyWidth * s}));
    row->addChild(ui::label({.text = std::move(value), .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                             .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    m_root->addChild(std::move(row));
  };
  const float previewW = (kPaneWidth - 2.0F * Tokens::spaceP16) * s;
  const auto preview = [&](const fs::path& path, const std::optional<FileInfo>& info) {
    const bool image = info && info->contentType.starts_with("image/");
    if (image) {
      m_root->addChild(pictureNode(path.string(), previewW, kPreviewHeight * s, Tokens::radiusR8 * s));
      return;
    }
    auto card = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                            .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8 * s,
                            .height = kPreviewHeight * s});
    if (info) {
      card->addChild(std::make_unique<IconNode>(*info, path, m_thumbnails, kPreviewIcon * s));
    }
    m_root->addChild(std::move(card));
  };

  if (m_selection.size() > 1) {
    std::uint64_t total = 0;
    for (const fs::path& p : m_selection) {
      if (const auto info = statFile(p); info && !info->isDir) {
        total += info->size;
      }
    }
    preview(m_selection.front(), statFile(m_selection.front()));
    m_root->addChild(text(std::format("{} items", m_selection.size()), Tokens::typeHeadline.size * s,
                          FontWeight::SemiBold, ColorRole::Label1, 1));
    m_root->addChild(text(formatFileSize(total), Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2, 1));
  } else {
    const fs::path path = m_selection.empty() ? m_folder : m_selection.front();
    const auto info = statFile(path);
    preview(path, info);
    auto names = ui::column({.align = FlexAlign::Stretch});
    names->addChild(text(info ? info->displayName : path.filename().string(), Tokens::typeHeadline.size * s,
                         FontWeight::SemiBold, ColorRole::Label1, 2));
    std::string detail = info ? fileKindLabel(*info) : std::string();
    if (info && info->isDir) {
      std::error_code ec;
      std::size_t count = 0;
      for (auto it = fs::directory_iterator(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        ++count;
      }
      detail += std::format(" · {} item{}", count, count == 1 ? "" : "s");
      if (struct statvfs v{}; m_selection.empty() && ::statvfs(path.c_str(), &v) == 0) {
        detail += std::format(" · {} free", formatFileSize(static_cast<std::uint64_t>(v.f_bavail) * v.f_frsize));
      }
    } else if (info) {
      detail += " · " + formatFileSize(info->size);
    }
    names->addChild(text(detail, Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2, 2));
    m_root->addChild(std::move(names));
    if (info && !info->isDir) {
      if (const auto size = info->contentType.starts_with("image/") ? imageSize(path) : std::nullopt) {
        kv("Dimensions", std::format("{} × {}", size->first, size->second));
        kLog.debug("details {} {}x{}", path.filename().string(), size->first, size->second);
      }
    }
    if (const std::int64_t born = birthNs(path); born != 0) {
      kv("Created", formatFileDate(born));
    }
    if (info) {
      kv("Modified", formatFileDate(info->mtimeNs));
    }
    kv("Where", path.parent_path().filename().string() + " › " + path.filename().string());
    if (const auto tags = readTags(path); !tags.empty()) {
      const auto defs = loadTagConfig();
      auto row = ui::row({.align = FlexAlign::Center});
      row->addChild(ui::label({.text = std::string("Tags"), .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(ColorRole::Label2), .minWidth = kKeyWidth * s, .maxWidth = kKeyWidth * s}));
      auto dots = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
      for (const std::string& tag : tags) {
        std::uint32_t rgb = 0x8E8E93; // an unknown tag shows grey
        for (const TagDef& d : defs) {
          rgb = d.name == tag ? d.rgb : rgb;
        }
        dots->addChild(ui::column({.fill = ColorSpec{.role = std::nullopt, .fixed = rgbHex(rgb), .alpha = 1.0F}, .radius = Tokens::spaceP4 * s, .width = Tokens::spaceP8 * s,
                                   .height = Tokens::spaceP8 * s}));
      }
      row->addChild(std::move(dots));
      m_root->addChild(std::move(row));
    }
  }
  if (!m_selection.empty()) {
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    buttons->addChild(ui::button({.text = std::string("Open"), .fontSize = Style::fontSizeBody * s,
                                  .variant = ButtonVariant::Primary, .onClick = [this] { m_actions.open(); }}));
    if (m_selection.size() == 1 && !fs::is_directory(m_selection.front())) {
      buttons->addChild(ui::button({.text = std::string("Open With…"), .fontSize = Style::fontSizeBody * s,
                                    .variant = ButtonVariant::Default, .onClick = [this] { m_actions.openWith(); }}));
    }
    m_root->addChild(std::move(buttons));
  }
  m_root->markLayoutDirty();
}

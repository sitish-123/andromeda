#include "details_pane.h"

#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/files/media_info.h"
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
#include <thread>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("details");
  constexpr float kPreviewHeight = 160.0F;
  constexpr float kKeyWidth = 88.0F;    // mockup .kv span:first-child
  constexpr float kPreviewIcon = 96.0F;

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


  std::int64_t birthNs(const fs::path& path) {
    struct statx st{};
    if (::statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &st) != 0 || (st.stx_mask & STATX_BTIME) == 0) {
      return 0;
    }
    return static_cast<std::int64_t>(st.stx_btime.tv_sec) * 1'000'000'000 + st.stx_btime.tv_nsec;
  }
} // namespace

struct DetailsPane::Facts {
  fs::path folder;
  std::vector<fs::path> selection;
  std::optional<FileInfo> info; // the selected item, or the folder
  std::uint64_t total = 0;      // several selected: their file bytes
  std::size_t count = 0;        // a folder: its entries
  std::optional<std::uint64_t> free;
  MediaInfo media;
  std::int64_t born = 0;
  std::vector<std::string> tags;
  std::vector<TagDef> tagDefs;

  static Facts read(fs::path folder, std::vector<fs::path> selection) {
    Facts f{.folder = std::move(folder), .selection = std::move(selection)};
    if (f.selection.size() > 1) {
      for (const fs::path& p : f.selection) {
        if (const auto info = statFile(p); info && !info->isDir) {
          f.total += info->size;
        }
      }
      f.info = statFile(f.selection.front());
      return f;
    }
    const fs::path path = f.selection.empty() ? f.folder : f.selection.front();
    f.info = statFile(path);
    if (f.info && f.info->isDir) {
      std::error_code ec;
      for (auto it = fs::directory_iterator(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        ++f.count;
      }
      if (struct statvfs v{}; f.selection.empty() && ::statvfs(path.c_str(), &v) == 0) {
        f.free = static_cast<std::uint64_t>(v.f_bavail) * v.f_frsize;
      }
    } else if (f.info) {
      const std::string& type = f.info->contentType;
      if (type.starts_with("image/") || type.starts_with("video/") || type.starts_with("audio/")) {
        f.media = readMediaInfo(path);
      }
    }
    f.born = birthNs(path);
    f.tags = readTags(path);
    if (!f.tags.empty()) {
      f.tagDefs = loadTagConfig();
    }
    return f;
  }
};

DetailsPane::DetailsPane(ThumbnailService* thumbnails, Actions actions)
    : m_thumbnails(thumbnails), m_actions(std::move(actions)) {}

DetailsPane::~DetailsPane() { *m_alive = false; }

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
  read();
}

void DetailsPane::read() {
  // One reader at a time: a held arrow key reads the newest selection once the current read is in, and a hung
  // network mount blocks one thread, not one per key press. The old content stays until the new facts are read.
  if (m_reading) {
    return;
  }
  m_reading = true;
  std::thread([this, folder = m_folder, selection = m_selection, alive = std::weak_ptr<bool>(m_alive)] {
    auto facts = std::make_shared<const Facts>(Facts::read(folder, selection));
    DeferredCall::callLater([this, facts = std::move(facts), alive] {
      if (alive.expired()) {
        return;
      }
      m_reading = false;
      if (facts->folder != m_folder || facts->selection != m_selection) {
        read(); // the selection moved on while this one was read
        return;
      }
      m_facts = facts;
      fill();
      if (m_actions.changed) {
        m_actions.changed();
      }
    });
  }).detach();
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
    // Mockup .kv: 12/16 type, so rows sit 28 apart with the 12 gap.
    auto row = ui::row({.align = FlexAlign::Center, .minHeight = Tokens::spaceP16 * s});
    row->addChild(ui::label({.text = std::string(key), .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .minWidth = kKeyWidth * s, .maxWidth = kKeyWidth * s}));
    row->addChild(ui::label({.text = std::move(value), .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                             .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    m_root->addChild(std::move(row));
  };
  const float previewW = (kPaneWidth - 2.0F * Tokens::spaceP16) * s;
  const auto preview = [&](const fs::path& path, const std::optional<FileInfo>& info) {
    const bool image = info && info->contentType.starts_with("image/");
    if (image) {
      m_root->addChild(pictureNode(path.string(), previewW, kPreviewHeight * s, Tokens::radiusR12 * s)); // mockup .info .prev
      return;
    }
    auto card = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                            .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR12 * s,
                            .height = kPreviewHeight * s});
    if (info) {
      card->addChild(std::make_unique<IconNode>(*info, path, m_thumbnails, kPreviewIcon * s));
    }
    m_root->addChild(std::move(card));
  };

  if (m_facts == nullptr || m_facts->folder != m_folder || m_facts->selection != m_selection) {
    m_root->markLayoutDirty();
    return; // read on a worker; fill() runs again when it is in
  }
  const Facts& f = *m_facts;
  if (m_selection.size() > 1) {
    preview(m_selection.front(), f.info);
    m_root->addChild(text(std::format("{} items", m_selection.size()), Tokens::typeHeadline.size * s,
                          FontWeight::SemiBold, ColorRole::Label1, 1));
    m_root->addChild(text(formatFileSize(f.total), Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2, 1));
  } else {
    const fs::path path = m_selection.empty() ? m_folder : m_selection.front();
    const auto& info = f.info;
    preview(path, info);
    auto names = ui::column({.align = FlexAlign::Stretch});
    names->addChild(text(info ? info->displayName : path.filename().string(), Tokens::typeTitle3.size * s,
                         FontWeight::SemiBold, ColorRole::Label1, 2));
    std::string detail = info ? fileKindLabel(*info) : std::string();
    if (info && info->isDir) {
      detail += std::format(" · {} item{}", f.count, f.count == 1 ? "" : "s");
      if (f.free) {
        detail += std::format(" · {} free", formatFileSize(*f.free));
      }
    } else if (info) {
      detail += " · " + formatFileSize(info->size);
    }
    names->addChild(text(detail, Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2, 2));
    m_root->addChild(std::move(names));
    const MediaInfo& media = f.media;
    if (media.size) {
      kv("Dimensions", std::format("{} × {}", media.size->first, media.size->second));
      kLog.debug("details {} {}x{}", path.filename().string(), media.size->first, media.size->second);
    }
    if (media.seconds) {
      kv("Duration", formatDuration(*media.seconds));
    }
    if (!media.artist.empty()) {
      kv("Artist", media.artist);
    }
    if (f.born != 0) {
      kv("Created", formatFileDate(f.born));
    }
    if (info) {
      kv("Modified", formatFileDate(info->mtimeNs));
    }
    kv("Where", path.parent_path().parent_path().filename().string() + " › " + path.parent_path().filename().string());
    if (!f.tags.empty()) {
      auto row = ui::row({.align = FlexAlign::Center});
      row->addChild(ui::label({.text = std::string("Tags"), .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(ColorRole::Label2), .minWidth = kKeyWidth * s, .maxWidth = kKeyWidth * s}));
      auto dots = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
      for (const std::string& tag : f.tags) {
        std::uint32_t rgb = 0x8E8E93; // an unknown tag shows grey
        for (const TagDef& d : f.tagDefs) {
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
    // Mockup .btn: 32 high r8 rectangles, fill_2 with no outline, Open in the accent.
    const auto button = [&](const char* label, ButtonVariant variant, std::function<void()> onClick) {
      Button::ButtonPalette palette = Button::defaultPalette(variant);
      palette.borderWidth = 0.0F;
      return ui::button({.text = std::string(label), .fontSize = Style::fontSizeBody * s, .variant = variant,
                         .customPalette = palette, .paddingH = Tokens::spaceP12 * s, .radius = Tokens::radiusR8 * s,
                         .height = Tokens::sizeControlM * s, .onClick = std::move(onClick)});
    };
    buttons->addChild(button("Open", ButtonVariant::Primary, [this] { m_actions.open(); }));
    if (m_selection.size() == 1 && !(f.info && f.info->isDir)) {
      buttons->addChild(button("Open With…", ButtonVariant::Default, [this] { m_actions.openWith(); }));
    }
    if (m_actions.share) {
      Button::ButtonPalette palette = Button::defaultPalette(ButtonVariant::Default);
      palette.borderWidth = 0.0F;
      Button* share = nullptr;
      buttons->addChild(ui::button({.out = &share, .glyph = std::string("share"), .glyphSize = Tokens::sizeSymbol * s,
                                    .variant = ButtonVariant::Default, .customPalette = palette,
                                    .paddingH = Tokens::spaceP12 * s, .radius = Tokens::radiusR8 * s, // mockup .btn
                                    .height = Tokens::sizeControlM * s}));
      share->setOnClick([this, share] {
        float x = 0.0F;
        float y = 0.0F;
        Node::mapToScene(share, 0.0F, share->height(), x, y);
        m_actions.share(x, y);
      });
    }
    m_root->addChild(std::move(buttons));
  }
  m_root->markLayoutDirty();
}

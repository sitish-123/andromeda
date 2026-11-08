#include "conflict_sheet.h"

#include "core/files/file_info.h"
#include "design_tokens.h"
#include "details_pane.h"
#include "portal_sheets.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <format>

namespace fs = std::filesystem;

namespace {
  constexpr float kPreviewHeight = 136.0F; // mockup .cf-card .pv
  constexpr float kThumb = 40.0F;          // mockup .cf-head .thumb
  constexpr float kBadgeHeight = 20.0F;
  constexpr std::int64_t kNsPerDay = 86'400'000'000'000;

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

  std::string ageWords(std::int64_t ns) {
    const std::int64_t days = ns / kNsPerDay;
    if (days >= 14) {
      return std::format("{} weeks", days / 7);
    }
    if (days >= 1) {
      return std::format("{} day{}", days, days == 1 ? "" : "s");
    }
    const std::int64_t hours = ns / (kNsPerDay / 24);
    return hours >= 1 ? std::format("{} hour{}", hours, hours == 1 ? "" : "s") : std::string("a moment");
  }
} // namespace

std::string ConflictSheet::keepBothName(const fs::path& target) {
  std::error_code ec;
  const bool dir = fs::is_directory(target, ec);
  const std::string name = target.filename().string();
  const auto dot = name.rfind('.');
  const bool split = !dir && dot != std::string::npos && dot > 0;
  const std::string stem = split ? name.substr(0, dot) : name;
  const std::string ext = split ? name.substr(dot) : std::string();
  for (std::uint32_t n = 1; n < 1000; ++n) {
    const std::string candidate = stem + (n == 1 ? std::string(" (copy)") : std::format(" (copy {})", n)) + ext;
    if (!fs::exists(target.parent_path() / candidate, ec)) {
      return candidate;
    }
  }
  return name;
}

std::string ConflictSheet::difference(std::uint64_t newSize, std::int64_t newTimeNs, std::uint64_t oldSize,
                                      std::int64_t oldTimeNs) {
  std::vector<std::string> parts;
  if (newSize != oldSize) {
    parts.push_back(std::format("{} {}", formatFileSize(newSize > oldSize ? newSize - oldSize : oldSize - newSize),
                                newSize > oldSize ? "larger" : "smaller"));
  }
  if (std::llabs(newTimeNs - oldTimeNs) >= kNsPerDay / 24) {
    parts.push_back(std::format("{} {}", ageWords(std::llabs(newTimeNs - oldTimeNs)), newTimeNs > oldTimeNs ? "newer" : "older"));
  }
  if (parts.empty()) {
    return "Both files have the same size and date.";
  }
  return std::format("The copy is {}.", parts.size() == 1 ? parts[0] : parts[0] + " and " + parts[1]);
}

ConflictSheet::ConflictSheet(Info info, ThumbnailService* thumbnails, std::function<void(Choice, bool)> answer,
                             std::function<void()> rebuild)
    : m_info(std::move(info)), m_thumbnails(thumbnails), m_answer(std::move(answer)), m_rebuild(std::move(rebuild)) {}

void ConflictSheet::choose(Choice choice) {
  if (!m_answered) {
    m_answered = true;
    m_answer(choice, m_applyToAll);
  }
}

std::unique_ptr<Node> ConflictSheet::buildMerge(float s) {
  const auto& clash = *m_info.folderClash;
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s, .padding = Tokens::spaceP24 * s});
  auto titles = ui::column({.align = FlexAlign::Stretch});
  titles->addChild(ui::label({.text = std::format("A folder “{}” is already in {}", m_info.target.filename().string(),
                                                  m_info.target.parent_path().filename().string()),
                              .fontSize = Tokens::typeHeadline.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                              .ellipsize = TextEllipsize::Middle}));
  titles->addChild(ui::label({.text = std::format("{} 1 folder · {} files", m_info.verb,
                                                  clash.newFiles + clash.sameFiles + clash.differentFiles),
                              .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                              .color = colorSpecFromRole(ColorRole::Label2)}));
  box->addChild(std::move(titles));
  // Mockup .cf-merge: three count cards; "different" in orange.
  auto counts = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
  const auto count = [&](std::uint64_t n, const char* what, bool warn) {
    auto c = ui::column({.align = FlexAlign::Start, .paddingV = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                         .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8 * s, .flexGrow = 1.0F});
    c->addChild(ui::label({.text = std::to_string(n), .fontSize = Tokens::typeTitle3.size * s,
                           .fontWeight = FontWeight::SemiBold,
                           .color = colorSpecFromRole(warn && n > 0 ? ColorRole::Warning : ColorRole::Label1)}));
    c->addChild(ui::label({.text = std::string(what), .fontSize = Style::fontSizeCaption * s,
                           .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2)}));
    counts->addChild(std::move(c));
  };
  count(clash.newFiles, "new files", false);
  count(clash.sameFiles, "same files", false);
  count(clash.differentFiles, "different", true);
  box->addChild(std::move(counts));
  if (!clash.different.empty()) {
    auto list = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s,
                            .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8 * s});
    for (const auto& [path, hint] : clash.different) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP8 * s,
                          .height = Tokens::sizeRowCompact * s});
      row->addChild(ui::label({.text = path.string(), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                               .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
      row->addChild(ui::label({.text = hint, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
      list->addChild(std::move(row));
    }
    box->addChild(std::move(list));
  }
  box->addChild(ui::label({.text = std::format("Merge adds the {} new files and skips the {} same ones.{}", clash.newFiles,
                                               clash.sameFiles,
                                               clash.differentFiles > 0
                                                   ? std::format(" It asks about the {} different ones next.", clash.differentFiles)
                                                   : std::string()),
                           .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 3}));
  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  const auto button = [&](const char* text, ButtonVariant variant, Choice choice) {
    buttons->addChild(ui::button({.text = std::string(text), .fontSize = Style::fontSizeBody * s, .variant = variant,
                                  .onClick = [this, choice] { choose(choice); }}));
  };
  button("Stop", ButtonVariant::Default, Choice::Stop);
  buttons->addChild(ui::node({.flexGrow = 1.0F}));
  button("Keep Both", ButtonVariant::Default, Choice::KeepBoth);
  // Replace on a folder merges (FileOps1 ResolveConflict), so Merge is the one action for it.
  button("Merge", ButtonVariant::Primary, Choice::Replace);
  box->addChild(std::move(buttons));
  return box;
}

std::unique_ptr<Node> ConflictSheet::build(float s) {
  if (m_info.folderClash.has_value()) {
    return buildMerge(s);
  }
  const auto source = statFile(m_info.source);
  const auto target = statFile(m_info.target);
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s, .padding = Tokens::spaceP24 * s});

  // Head: thumbnail, "“name” is already in <folder>", the job line.
  auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
  if (source) {
    head->addChild(std::make_unique<IconNode>(*source, m_info.source, m_thumbnails, kThumb * s));
  }
  auto titles = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  titles->addChild(ui::label({.text = std::format("“{}” is already in {}", m_info.target.filename().string(),
                                                  m_info.target.parent_path().filename().string()),
                              .fontSize = Tokens::typeHeadline.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                              .ellipsize = TextEllipsize::Middle}));
  titles->addChild(ui::label({.text = std::format("{} {} item{} to {}{}", m_info.verb, m_info.totalFiles,
                                                  m_info.totalFiles == 1 ? "" : "s",
                                                  m_info.target.parent_path().filename().string(),
                                                  m_info.conflictIndex > 0 && m_info.conflictTotal > 0
                                                      ? std::format(" · conflict {} of {}", m_info.conflictIndex,
                                                                    m_info.conflictTotal)
                                                      : std::string()),
                              .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                              .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
  head->addChild(std::move(titles));
  box->addChild(std::move(head));

  // The pair: the copy and the one already there; the selected card is the file that stays.
  const auto card = [&](const std::optional<FileInfo>& info, const fs::path& path, bool isNew) {
    const bool on = m_keepNew == isNew;
    auto c = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP8 * s,
                         .fill = on ? colorSpecFromRole(ColorRole::Accent, 0.12F) : colorSpecFromRole(ColorRole::Fill1),
                         .radius = Tokens::radiusR8 * 2.0F * s,
                         .border = on ? colorSpecFromRole(ColorRole::Accent) : colorSpecFromRole(ColorRole::Separator),
                         .borderWidth = on ? 2.0F * s : std::max(1.0F, 0.5F * s)});
    const bool image = info && info->contentType.starts_with("image/");
    if (image) {
      c->addChild(pictureNode(path.string(), 0.0F, kPreviewHeight * s, Tokens::radiusR8 * s));
    } else {
      auto pv = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                            .fill = colorSpecFromRole(ColorRole::Fill2), .radius = Tokens::radiusR8 * s,
                            .height = kPreviewHeight * s});
      if (info) {
        pv->addChild(std::make_unique<IconNode>(*info, path, m_thumbnails, Tokens::spaceP64 * s));
      }
      c->addChild(std::move(pv));
    }
    auto cap = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
    cap->addChild(ui::label({.text = std::string(isNew ? "COPYING" : "ALREADY THERE"), .fontSize = Style::fontSizeCaption * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label2),
                             .flexGrow = 1.0F}));
    if (source && target) {
      const auto& mine = isNew ? *source : *target;
      const auto& other = isNew ? *target : *source;
      const char* badge = mine.mtimeNs > other.mtimeNs + kNsPerDay / 24 ? "Newer"
                          : mine.size > other.size                    ? "Larger"
                                                                      : nullptr;
      if (badge != nullptr) {
        auto b = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP8 * s,
                          .fill = colorSpecFromRole(ColorRole::Success, 0.16F), .radius = kBadgeHeight / 2.0F * s,
                          .height = kBadgeHeight * s});
        b->addChild(ui::label({.text = std::string(badge), .fontSize = Style::fontSizeCaption * s,
                               .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Success)}));
        cap->addChild(std::move(b));
      }
    }
    c->addChild(std::move(cap));
    if (info) {
      std::string meta = std::format("{} · {}", formatFileSize(info->size), formatFileDate(info->mtimeNs));
      if (const auto size = image ? DetailsPane::imageSize(path) : std::nullopt) {
        meta += std::format("\n{} × {}", size->first, size->second);
      }
      c->addChild(ui::label({.text = meta, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                             .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2}));
    }
    auto area = ui::inputArea({.flexGrow = 1.0F, .onClick = [this, isNew](const InputArea::PointerData&) {
                                 m_keepNew = isNew;
                                 m_rebuild();
                               }});
    area->addChild(std::move(c));
    return area;
  };
  auto pair = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
  pair->addChild(card(source, m_info.source, true));
  auto vs = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .width = Tokens::spaceP24 * s});
  vs->addChild(ui::glyph({.glyph = "chevron-right", .glyphSize = Tokens::spaceP12 * s,
                          .color = colorSpecFromRole(ColorRole::Label3)}));
  pair->addChild(std::move(vs));
  pair->addChild(card(target, m_info.target, false));
  box->addChild(std::move(pair));

  if (source && target) {
    box->addChild(ui::label({.text = difference(source->size, source->mtimeNs, target->size, target->mtimeNs),
                             .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                             .color = colorSpecFromRole(ColorRole::Label2), .textAlign = TextAlign::Center}));
  }

  auto opt = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  // Known count: "the 3 other conflicts"; unknown (0) with several files: offered without a number.
  const std::uint64_t others = m_info.conflictTotal > m_info.conflictIndex ? m_info.conflictTotal - m_info.conflictIndex : 0;
  if (others > 0 || (m_info.conflictTotal == 0 && m_info.totalFiles > 1)) {
    opt->addChild(ui::checkbox({.checked = m_applyToAll, .onChange = [this](bool on) { m_applyToAll = on; }}));
    opt->addChild(ui::label({.text = others > 0 ? std::format("Do the same for the {} other conflict{}", others,
                                                              others == 1 ? "" : "s")
                                                : std::string("Do the same for the other conflicts"),
                             .fontSize = Style::fontSizeBody * s,
                             .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label1)}));
  }
  opt->addChild(ui::node({.flexGrow = 1.0F}));
  opt->addChild(ui::label({.text = std::format("Keep Both saves “{}”", keepBothName(m_info.target)),
                           .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                           .ellipsize = TextEllipsize::Middle}));
  box->addChild(std::move(opt));

  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  const auto button = [&](const char* text, ButtonVariant variant, Choice choice) {
    buttons->addChild(ui::button({.text = std::string(text), .fontSize = Style::fontSizeBody * s, .variant = variant,
                                  .onClick = [this, choice] { choose(choice); }}));
  };
  button("Stop", ButtonVariant::Default, Choice::Stop);
  buttons->addChild(ui::node({.flexGrow = 1.0F}));
  // The primary follows the selected card: keep the copy = Replace, keep the one there = Skip.
  button("Skip", m_keepNew ? ButtonVariant::Default : ButtonVariant::Primary, Choice::Skip);
  button("Keep Both", ButtonVariant::Default, Choice::KeepBoth);
  button("Replace", m_keepNew ? ButtonVariant::Primary : ButtonVariant::Default, Choice::Replace);
  box->addChild(std::move(buttons));
  return box;
}

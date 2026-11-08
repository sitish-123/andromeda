#include "conflict_sheet.h"

#include "core/files/media_info.h"

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
  constexpr float kSheetWidth = 576.0F; // mockup .cf
  constexpr float kMergeWidth = 456.0F; // mockup .cf-merge
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

  // A click area the size of its one child: a plain InputArea measures as zero, so a row would give it no height.
  // The preview (child 0) with a badge (child 1) pinned to its bottom-right corner, `inset` in from both edges.
  class Pinned final : public Node {
  public:
    explicit Pinned(float inset) : m_inset(inset) {}

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return children().empty() ? LayoutSize{} : children().front()->measure(renderer, constraints);
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, rect.height);
      if (children().size() < 2) {
        return;
      }
      children()[0]->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = rect.width, .height = rect.height});
      const LayoutSize badge = children()[1]->measure(renderer, LayoutConstraints{});
      children()[1]->arrange(renderer, LayoutRect{.x = rect.width - badge.width - m_inset,
                                                  .y = rect.height - badge.height - m_inset,
                                                  .width = badge.width, .height = badge.height});
    }

  private:
    float m_inset;
  };

  class CardArea final : public InputArea {
  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return children().empty() ? LayoutSize{} : children().front()->measure(renderer, constraints);
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, rect.height);
      if (!children().empty()) {
        children().front()->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = rect.width, .height = rect.height});
      }
    }
  };

  // Mockup .dlg .btn: 32 high capsules, fill_2 with no outline (the accent one for the primary action).
  std::unique_ptr<Node> capsule(const char* text, ButtonVariant variant, float s, std::function<void()> onClick) {
    Button::ButtonPalette palette = Button::defaultPalette(variant);
    palette.borderWidth = 0.0F;
    return ui::button({.text = std::string(text), .fontSize = Style::fontSizeBody * s, .variant = variant,
                       .customPalette = palette,
                       .radius = Tokens::sizeControlM * s / 2.0F, .height = Tokens::sizeControlM * s,
                       .onClick = std::move(onClick)});
  }

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
                                      std::int64_t oldTimeNs, std::optional<double> newSeconds,
                                      std::optional<double> oldSeconds) {
  std::vector<std::string> parts;
  if (newSize != oldSize) {
    parts.push_back(std::format("{} {}", formatFileSize(newSize > oldSize ? newSize - oldSize : oldSize - newSize),
                                newSize > oldSize ? "larger" : "smaller"));
  }
  if (std::llabs(newTimeNs - oldTimeNs) >= kNsPerDay / 24) {
    parts.push_back(std::format("{} {}", ageWords(std::llabs(newTimeNs - oldTimeNs)), newTimeNs > oldTimeNs ? "newer" : "older"));
  }
  if (newSeconds && oldSeconds && std::abs(*newSeconds - *oldSeconds) >= 1.0) {
    const double d = std::abs(*newSeconds - *oldSeconds);
    parts.push_back(std::format("{} {}", d < 60.0 ? std::format("{} s", std::lround(d)) : formatDuration(d),
                                *newSeconds > *oldSeconds ? "longer" : "shorter"));
  }
  if (parts.empty()) {
    return "Both files have the same size and date.";
  }
  std::string text = parts.front();
  for (std::size_t i = 1; i < parts.size(); ++i) {
    text += (i + 1 == parts.size() ? " and " : ", ") + parts[i];
  }
  return std::format("The copy is {}.", text);
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
  // Mockup §5 .cf-merge: 456 wide, padding 24, gap 16.
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s, .padding = Tokens::spaceP24 * s,
                         .width = kMergeWidth * s});
  auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
  if (const auto folder = statFile(m_info.target)) {
    head->addChild(std::make_unique<IconNode>(*folder, m_info.target, m_thumbnails, kThumb * s));
  }
  auto titles = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  titles->addChild(ui::label({.text = std::format("A folder “{}” is already in {}", m_info.target.filename().string(),
                                                  m_info.target.parent_path().filename().string()),
                              .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                              .ellipsize = TextEllipsize::Middle}));
  titles->addChild(ui::label({.text = std::format("{} 1 folder · {} files", m_info.verb,
                                                  clash.newFiles + clash.sameFiles + clash.differentFiles),
                              .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                              .color = colorSpecFromRole(ColorRole::Label2)}));
  head->addChild(std::move(titles));
  box->addChild(std::move(head));
  // Three count cards (r12, 600 17/24 + 500 12 label_2); "different" in the warning colour.
  auto counts = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
  const auto count = [&](std::uint64_t n, const char* what, bool warn) {
    auto c = ui::column({.align = FlexAlign::Start, .paddingV = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                         .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Tokens::radiusR12 * s, .flexGrow = 1.0F});
    c->addChild(ui::label({.text = std::to_string(n), .fontSize = Tokens::typeTitle2.size * s,
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
                            .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Tokens::radiusR8 * s});
    for (const auto& [path, hint] : clash.different) {
      // Mockup .trow: 32 high r8, padding 0 8, a 20 icon slot, 400 13 name.
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP8 * s,
                          .height = Tokens::sizeControlM * s});
      const fs::path inside = m_info.target / path;
      if (const auto info = statFile(inside)) {
        row->addChild(std::make_unique<IconNode>(*info, inside, m_thumbnails, Tokens::spaceP20 * s));
      }
      row->addChild(ui::label({.text = path.string(), .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1),
                               .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
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
  buttons->addChild(capsule("Stop", ButtonVariant::Default, s, [this] { choose(Choice::Stop); }));
  buttons->addChild(ui::node({.flexGrow = 1.0F}));
  buttons->addChild(capsule("Keep Both", ButtonVariant::Default, s, [this] { choose(Choice::KeepBoth); }));
  // Replace swaps the whole folder (the old one to the trash); Merge copies into it.
  buttons->addChild(capsule("Replace", ButtonVariant::Default, s, [this] { choose(Choice::Replace); }));
  buttons->addChild(capsule("Merge", ButtonVariant::Primary, s, [this] { choose(Choice::Merge); }));
  box->addChild(std::move(buttons));
  return box;
}

std::unique_ptr<Node> ConflictSheet::build(float s) {
  if (m_info.folderClash.has_value()) {
    return buildMerge(s);
  }
  const auto source = statFile(m_info.source);
  const auto target = statFile(m_info.target);
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s, .padding = Tokens::spaceP24 * s,
                         .width = kSheetWidth * s});

  // Head: thumbnail, "“name” is already in <folder>", the job line.
  auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
  if (source) {
    head->addChild(std::make_unique<IconNode>(*source, m_info.source, m_thumbnails, kThumb * s));
  }
  auto titles = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  titles->addChild(ui::label({.text = std::format("“{}” is already in {}", m_info.target.filename().string(),
                                                  m_info.target.parent_path().filename().string()),
                              .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
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
                         .fill = on ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F)
                                    : colorSpecFromRole(ColorRole::GlassCard),
                         .radius = Tokens::radiusR8 * 2.0F * s,
                         .border = on ? colorSpecFromRole(ColorRole::Accent) : colorSpecFromRole(ColorRole::GlassCardEdge),
                         .borderWidth = on ? 2.0F * s : std::max(1.0F, 0.5F * s)});
    const bool image = info && info->contentType.starts_with("image/");
    const bool media = info && (image || info->contentType.starts_with("video/") || info->contentType.starts_with("audio/"));
    const MediaInfo mediaInfo = media ? readMediaInfo(path) : MediaInfo{};
    if (image) {
      c->addChild(pictureNode(path.string(), 0.0F, kPreviewHeight * s, Tokens::radiusR8 * s));
    } else {
      auto pv = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                            .fill = colorSpecFromRole(ColorRole::Fill2), .radius = Tokens::radiusR8 * s,
                            .clipChildren = true, .height = kPreviewHeight * s});
      if (info) {
        // Videos and documents show their thumbnail as large as the preview allows.
        pv->addChild(std::make_unique<IconNode>(*info, path, m_thumbnails, (kPreviewHeight - Tokens::spaceP16) * s));
      }
      if (mediaInfo.seconds) {
        // Mockup .cf-card .pv .dur (600 10; caption 11 is the smallest type token): white on black 55 %, padding 2 4, r4, 4 in from the corner.
        auto pinned = std::make_unique<Pinned>(Tokens::spaceP4 * s);
        pinned->addChild(std::move(pv));
        auto dur = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP4 * s,
                            .fill = ColorSpec{.role = std::nullopt, .fixed = rgbHex(0x000000), .alpha = 0.55F},
                            .radius = Tokens::radiusR4 * s});
        dur->setPadding(Tokens::spaceP2 * s, Tokens::spaceP4 * s);
        dur->addChild(ui::label({.text = formatDuration(*mediaInfo.seconds), .fontSize = Style::fontSizeMini * s,
                                 .fontWeight = FontWeight::SemiBold,
                                 .color = ColorSpec{.role = std::nullopt, .fixed = rgbHex(0xFFFFFF), .alpha = 1.0F}}));
        pinned->addChild(std::move(dur));
        c->addChild(std::move(pinned));
      } else {
        c->addChild(std::move(pv));
      }
    }
    auto cap = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
    cap->addChild(ui::label({.text = std::string(isNew ? "COPYING" : "ALREADY THERE"), .fontSize = Style::fontSizeMini * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label2),
                             .flexGrow = 1.0F}));
    if (source && target) {
      const auto& mine = isNew ? *source : *target;
      const auto& other = isNew ? *target : *source;
      const char* badge = mine.mtimeNs > other.mtimeNs + kNsPerDay / 24 ? "NEWER"
                          : mine.size > other.size                    ? "LARGER"
                                                                      : nullptr;
      if (badge != nullptr) {
        auto b = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP8 * s,
                          .fill = colorSpecFromRole(ColorRole::Success, 0.16F), .radius = kBadgeHeight / 2.0F * s,
                          .height = kBadgeHeight * s});
        b->addChild(ui::label({.text = std::string(badge),
                               .fontSize = Style::fontSizeMini * s,
                               .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Success)}));
        cap->addChild(std::move(b));
      }
    }
    c->addChild(std::move(cap));
    if (info) {
      // Mockup .cf-meta: "<b>1.9 GB</b> · Today, 09:12", then "W × H" for images.
      auto meta = ui::row({.align = FlexAlign::Center});
      meta->addChild(ui::label({.text = formatFileSize(info->size), .fontSize = Style::fontSizeCaption * s,
                                .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
      meta->addChild(ui::label({.text = " · " + formatFileDate(info->mtimeNs), .fontSize = Style::fontSizeCaption * s,
                                .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2),
                                .maxLines = 1, .ellipsize = TextEllipsize::End}));
      auto lines = ui::column({.align = FlexAlign::Start});
      lines->addChild(std::move(meta));
      // Line 2: "W × H · 0:42" for images, video and audio.
      std::vector<std::string> facts;
      if (mediaInfo.size) {
        facts.push_back(std::format("{} × {}", mediaInfo.size->first, mediaInfo.size->second));
      }
      if (mediaInfo.seconds) {
        facts.push_back(formatDuration(*mediaInfo.seconds));
      }
      if (!facts.empty()) {
        lines->addChild(ui::label({.text = facts.size() == 1 ? facts[0] : facts[0] + " · " + facts[1],
                                   .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                                   .color = colorSpecFromRole(ColorRole::Label2)}));
      }
      c->addChild(std::move(lines));
    }
    auto area = std::make_unique<CardArea>();
    area->setFlexGrow(1.0F);
    area->setOnClick([this, isNew](const InputArea::PointerData&) {
      m_keepNew = isNew;
      m_rebuild();
    });
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
    const auto seconds = [](const FileInfo& f, const fs::path& p) {
      const bool media = f.contentType.starts_with("video/") || f.contentType.starts_with("audio/");
      return media ? readMediaInfo(p).seconds : std::nullopt;
    };
    box->addChild(ui::label({.text = difference(source->size, source->mtimeNs, target->size, target->mtimeNs,
                                                seconds(*source, m_info.source), seconds(*target, m_info.target)),
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
  buttons->addChild(capsule("Stop", ButtonVariant::Default, s, [this] { choose(Choice::Stop); }));
  buttons->addChild(ui::node({.flexGrow = 1.0F}));
  // The primary follows the selected card: keep the copy = Replace, keep the one there = Skip.
  buttons->addChild(capsule("Skip", m_keepNew ? ButtonVariant::Default : ButtonVariant::Primary, s,
                            [this] { choose(Choice::Skip); }));
  buttons->addChild(capsule("Keep Both", ButtonVariant::Default, s, [this] { choose(Choice::KeepBoth); }));
  buttons->addChild(capsule("Replace", m_keepNew ? ButtonVariant::Primary : ButtonVariant::Default, s,
                            [this] { choose(Choice::Replace); }));
  box->addChild(std::move(buttons));
  return box;
}

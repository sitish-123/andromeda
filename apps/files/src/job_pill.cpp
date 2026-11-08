#include "job_pill.h"

#include "design_tokens.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/countdown_ring.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <format>

namespace job_pill {
  namespace {
    constexpr float kRing = 16.0F;        // mockup .ring
    constexpr float kRingThickness = 3.0F; // the 5 px hole of the 16 ring
    constexpr float kBar = 4.0F;          // mockup .prog
    constexpr float kRowHeight = 44.0F;   // title 16 + bar 4 + detail 16 + gaps
    constexpr float kHeaderHeight = 16.0F;

    std::unique_ptr<Node> ring(float value, float s) {
      auto box = std::make_unique<Node>();
      box->setSize(kRing * s, kRing * s);
      const auto make = [&](float p, const ColorSpec& color) {
        auto r = std::make_unique<CountdownRing>();
        r->setRingSize(kRing * s);
        r->setThickness(kRingThickness * s);
        r->setProgress(p);
        r->setColor(color);
        r->setSize(kRing * s, kRing * s);
        box->addChild(std::move(r));
      };
      make(1.0F, colorSpecFromRole(ColorRole::Fill2));
      make(value, colorSpecFromRole(ColorRole::Accent));
      return box;
    }

    std::string countText(std::uint64_t n) {
      // "2 048" with a thin space, as the mockup groups thousands.
      std::string digits = std::to_string(n);
      for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), " ");
      }
      return digits;
    }
  } // namespace

  std::string title(const FileOpsClient::Job& job) {
    const std::string what = job.totalFiles == 1 && !job.current.empty() ? job.current.filename().string()
                             : std::format("{} items", job.totalFiles);
    return job.current.has_parent_path() ? std::format("{} → {}", what, job.current.parent_path().filename().string())
                                         : what;
  }

  float progress(const FileOpsClient::Job& job) {
    if (job.totalBytes > 0) {
      return static_cast<float>(static_cast<double>(job.doneBytes) / static_cast<double>(job.totalBytes));
    }
    return job.totalFiles > 0 ? static_cast<float>(job.doneFiles) / static_cast<float>(job.totalFiles) : 0.0F;
  }

  std::string detail(const FileOpsClient::Job& job, double bytesPerSecond) {
    if (job.totalBytes == 0) {
      return std::format("{} of {} files", countText(job.doneFiles), countText(job.totalFiles));
    }
    std::string out = std::format("{} of {}", formatFileSize(job.doneBytes), formatFileSize(job.totalBytes));
    if (bytesPerSecond > 0.0 && job.totalBytes > job.doneBytes) {
      const auto left = static_cast<std::uint64_t>(std::ceil(static_cast<double>(job.totalBytes - job.doneBytes) / bytesPerSecond));
      out += left < 60 ? std::format(" · {} s left", left) : std::format(" · {} min left", (left + 59) / 60);
    }
    return out;
  }

  std::unique_ptr<Node> pill(const std::vector<FileOpsClient::Job>& jobs, float s, std::function<void()> onClick,
                             Node** out) {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    for (const auto& j : jobs) {
      done += j.totalBytes > 0 ? j.doneBytes : j.doneFiles;
      total += j.totalBytes > 0 ? j.totalBytes : j.totalFiles;
    }
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP8 * s,
                        .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8 * s,
                        .height = Tokens::sizeControlM * s});
    row->addChild(ring(total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0F, s));
    row->addChild(ui::label({.text = std::to_string(jobs.size()), .fontSize = Style::fontSizeBody * s,
                             .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label1)}));
    auto area = ui::inputArea({.onClick = [onClick = std::move(onClick)](const InputArea::PointerData&) { onClick(); }});
    area->addChild(std::move(row));
    if (out != nullptr) {
      *out = area.get();
    }
    return area;
  }

  float popoverHeight(std::size_t jobs) {
    return Tokens::spaceP12 * 2.0F + kHeaderHeight + static_cast<float>(jobs) * (kRowHeight + Tokens::spaceP8);
  }

  std::unique_ptr<Node> popover(const std::vector<FileOpsClient::Job>& jobs, float s,
                                const std::function<double(const std::string& path)>& rate,
                                std::function<void(const std::string& path)> cancel) {
    auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP12 * s});
    std::uint64_t items = 0;
    bool moving = !jobs.empty();
    for (const auto& j : jobs) {
      items += j.totalFiles;
      moving = moving && j.kind == "move";
    }
    box->addChild(ui::label({.text = std::format("{} {} item{}", moving ? "Moving" : "Copying", items, items == 1 ? "" : "s"),
                             .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                             .color = colorSpecFromRole(ColorRole::Label1)}));
    for (const auto& j : jobs) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
      row->addChild(ring(progress(j), s));
      auto col = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * s, .flexGrow = 1.0F});
      col->addChild(ui::label({.text = title(j), .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                               .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                               .ellipsize = TextEllipsize::Middle}));
      auto bar = std::make_unique<ProgressBar>();
      bar->setTrackColor(colorSpecFromRole(ColorRole::Fill2));
      bar->setFillColor(colorSpecFromRole(ColorRole::Accent));
      bar->setRadius(kBar / 2.0F * s);
      bar->setProgress(progress(j));
      bar->setSize(0.0F, kBar * s);
      bar->setFlexGrow(0.0F);
      col->addChild(std::move(bar));
      col->addChild(ui::label({.text = detail(j, rate(j.path)), .fontSize = Style::fontSizeCaption * s,
                               .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2),
                               .maxLines = 1}));
      row->addChild(std::move(col));
      row->addChild(ui::button({.glyph = std::string("x"), .glyphSize = Tokens::spaceP12 * s,
                                .variant = ButtonVariant::Ghost, .tooltip = std::string("Stop"),
                                .radius = Tokens::spaceP12 * s, .width = Tokens::spaceP24 * s,
                                .height = Tokens::spaceP24 * s,
                                .onClick = [cancel, path = j.path] { cancel(path); }}));
      box->addChild(std::move(row));
    }
    return box;
  }
} // namespace job_pill

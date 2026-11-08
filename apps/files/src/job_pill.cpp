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

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace job_pill {
  namespace {
    constexpr Logger kLog("files");
    constexpr float kRing = 16.0F;        // mockup .ring
    constexpr float kRingThickness = 3.0F; // the 5 px hole of the 16 ring
    constexpr float kBar = 4.0F;          // mockup .prog
    constexpr float kRowHeight = 36.0F;   // mockup: title 16 + bar 4 + detail 16, no gaps
    constexpr float kHeaderHeight = 16.0F;

    std::unique_ptr<Node> ring(float value, float s, CountdownRing** accent = nullptr) {
      auto box = std::make_unique<Node>();
      box->setSize(kRing * s, kRing * s);
      const auto make = [&](float p, const ColorSpec& color) {
        auto r = std::make_unique<CountdownRing>();
        r->setRingSize(kRing * s);
        r->setThickness(kRingThickness * s);
        r->setProgress(p);
        r->setColor(color);
        r->setSize(kRing * s, kRing * s);
        return static_cast<CountdownRing*>(box->addChild(std::move(r)));
      };
      make(1.0F, colorSpecFromRole(ColorRole::Fill2));
      CountdownRing* arc = make(value, colorSpecFromRole(ColorRole::Accent));
      if (accent != nullptr) {
        *accent = arc;
      }
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
    return job.destination.empty() ? what : std::format("{} → {}", what, job.destination.filename().string());
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

  float combined(const std::vector<FileOpsClient::Job>& jobs) {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    for (const auto& j : jobs) {
      done += j.totalBytes > 0 ? j.doneBytes : j.doneFiles;
      total += j.totalBytes > 0 ? j.totalBytes : j.totalFiles;
    }
    return total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0F;
  }

  std::unique_ptr<Node> pill(const std::vector<FileOpsClient::Job>& jobs, float s, std::function<void()> onClick,
                             Node** out, CountdownRing** accent) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP8 * s,
                        .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8 * s,
                        .height = Tokens::sizeControlM * s});
    row->addChild(ring(combined(jobs), s, accent));
    row->addChild(ui::label({.text = std::to_string(jobs.size()), .fontSize = Style::fontSizeBody * s,
                             .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label1)}));
    // A plain InputArea keeps its own (zero) size; the pill's width follows the count text.
    class HugArea final : public InputArea {
    protected:
      void doLayout(Renderer& renderer) override {
        InputArea::doLayout(renderer);
        if (!children().empty()) {
          setSize(children().front()->width(), children().front()->height());
        }
      }
      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        InputArea::doArrange(renderer, rect);
        // E2E: where to click, as the browser's DESK_FILES_GEOMETRY lines.
        static const bool geometry = std::getenv("DESK_FILES_GEOMETRY") != nullptr;
        if (geometry) {
          float x = 0.0F;
          float y = 0.0F;
          Node::mapToScene(this, 0.0F, 0.0F, x, y);
          // Left, top, width, height: the popover hangs under the right edge (AppPopover).
          kLog.debug("geometry pill {} {} {} {}", static_cast<int>(x), static_cast<int>(y), static_cast<int>(width()),
                     static_cast<int>(height()));
        }
      }
    };
    auto area = std::make_unique<HugArea>();
    area->setOnClick([onClick = std::move(onClick)](const InputArea::PointerData&) { onClick(); });
    area->addChild(std::move(row));
    if (out != nullptr) {
      *out = area.get();
    }
    return area;
  }

  float popoverHeight(std::size_t jobs) {
    return Tokens::spaceP12 * 2.0F + kHeaderHeight + static_cast<float>(jobs) * (kRowHeight + Tokens::spaceP8);
  }

  namespace {
    // The circles a row shows: pause in Running and resume in Paused (copy, move), then cancel; none in a conflict
    // (the sheet asks), as the daemon rejects Pause there.
    struct RowButtons {
      bool pause = false;
      bool resume = false;
    };
    RowButtons rowButtons(const FileOpsClient::Job& job) {
      const bool pausable = job.kind == "copy" || job.kind == "move";
      return {.pause = pausable && job.state == "running", .resume = pausable && job.state == "paused"};
    }
    std::string detailText(const FileOpsClient::Job& job, double rate) {
      return job.state == "paused" ? std::string("Paused") : detail(job, rate);
    }
  } // namespace

  std::string layoutKey(const std::vector<FileOpsClient::Job>& jobs) {
    std::string key;
    for (const auto& j : jobs) {
      const RowButtons b = rowButtons(j);
      key += j.path + (b.pause ? ":p" : b.resume ? ":r" : ":-") + ";";
    }
    return key;
  }

  std::unique_ptr<Node> popover(const std::vector<FileOpsClient::Job>& jobs, float s,
                                const std::function<double(const std::string& path)>& rate,
                                std::function<void(const std::string& path)> cancel,
                                std::function<void(const std::string& path, bool pause)> pause,
                                std::vector<RowNodes>* rows) {
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
    if (rows != nullptr) {
      rows->clear();
    }
    const auto circle = [&](const char* glyph, const char* tooltip, std::function<void()> onClick) {
      return ui::button({.glyph = std::string(glyph), .glyphSize = Tokens::spaceP12 * s, .variant = ButtonVariant::Ghost,
                         .tooltip = std::string(tooltip), .radius = Tokens::spaceP12 * s, .width = Tokens::spaceP24 * s,
                         .height = Tokens::spaceP24 * s, .onClick = std::move(onClick)});
    };
    for (const auto& j : jobs) {
      RowNodes nodes{.path = j.path};
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
      row->addChild(ring(progress(j), s, &nodes.ring));
      auto col = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
      col->addChild(ui::label({.out = &nodes.title, .text = title(j), .fontSize = Style::fontSizeCaption * s,
                               .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label1),
                               .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
      auto bar = std::make_unique<ProgressBar>();
      bar->setTrackColor(colorSpecFromRole(ColorRole::Fill2));
      bar->setFillColor(colorSpecFromRole(ColorRole::Accent));
      bar->setRadius(kBar / 2.0F * s);
      bar->setProgress(progress(j));
      bar->setSize(0.0F, kBar * s);
      bar->setFlexGrow(0.0F);
      nodes.bar = static_cast<ProgressBar*>(col->addChild(std::move(bar)));
      col->addChild(ui::label({.out = &nodes.detail, .text = detailText(j, rate(j.path)),
                               .fontSize = Style::fontSizeMini * s, .fontWeight = FontWeight::Medium,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                               .ellipsize = TextEllipsize::End}));
      row->addChild(std::move(col));
      const RowButtons b = rowButtons(j);
      if (b.pause || b.resume) {
        row->addChild(circle(b.pause ? "player-pause" : "player-play", b.pause ? "Pause" : "Resume",
                             [pause, path = j.path, p = b.pause] { pause(path, p); }));
      }
      row->addChild(circle("x", "Stop", [cancel, path = j.path] { cancel(path); }));
      box->addChild(std::move(row));
      if (rows != nullptr) {
        rows->push_back(nodes);
      }
    }
    return box;
  }

  void update(const std::vector<RowNodes>& rows, const std::vector<FileOpsClient::Job>& jobs,
              const std::function<double(const std::string& path)>& rate) {
    for (const auto& j : jobs) {
      const auto it = std::ranges::find(rows, j.path, &RowNodes::path);
      if (it == rows.end()) {
        continue;
      }
      if (it->ring != nullptr) {
        it->ring->setProgress(progress(j));
      }
      if (it->bar != nullptr) {
        it->bar->setProgress(progress(j));
      }
      if (it->title != nullptr) {
        it->title->setText(title(j));
      }
      if (it->detail != nullptr) {
        it->detail->setText(detailText(j, rate(j.path)));
      }
    }
  }
} // namespace job_pill

#include "ui/files/this_pc.h"

#include "cursor-shape-v1-client-protocol.h"

#include "core/files/file_info.h"
#include "core/files/volumes.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>

namespace fs = std::filesystem;

namespace {
  const float& kFolderCellW = Tokens::componentFilesHomeCellWidth;  // 128
  const float& kFolderCellH = Tokens::componentFilesHomeCellHeight; // 104
  const float& kDriveH = Tokens::spaceP64;
  const float& kDriveIcon = Tokens::spaceP40;
  const float& kBar = Tokens::spaceP4;
  // Drive cards (Explorer "Devices and drives"): as many columns as fit at 280, each at most 360, so a wide window
  // gets more cards in a row instead of long stretched bars.
  const float& kDriveMinW = Tokens::componentFilesDriveCardMin;
  const float& kDriveMaxW = Tokens::componentFilesDriveCardMax;
  constexpr double kNearlyFull = 0.9;

  // Cells in rows, as many as fit at `cellW`; with `maxCellW` set they grow to fill the row up to that width.
  class CellGrid final : public Node {
  public:
    CellGrid(float cellW, float cellH, float gap, float maxCellW = 0.0F)
        : m_cellW(cellW), m_cellH(cellH), m_gap(gap), m_maxCellW(maxCellW) {}

  protected:
    LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
      const float w = constraints.hasMaxWidth ? constraints.maxWidth : m_cellW * 4.0F;
      const auto rows = static_cast<float>((children().size() + columns(w) - 1) / columns(w));
      return constraints.constrain(LayoutSize{w, rows * m_cellH + std::max(0.0F, rows - 1.0F) * m_gap});
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, rect.height);
      const std::size_t cols = columns(rect.width);
      const float fill = std::floor((rect.width - m_gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
      const float cellW = m_maxCellW > 0.0F ? std::clamp(fill, std::min(m_cellW, fill), m_maxCellW) : m_cellW;
      for (std::size_t i = 0; i < children().size(); ++i) {
        const auto col = static_cast<float>(i % cols);
        const auto row = static_cast<float>(i / cols);
        children()[i]->arrange(renderer, LayoutRect{.x = col * (cellW + m_gap), .y = row * (m_cellH + m_gap),
                                                    .width = cellW, .height = m_cellH});
      }
    }
    void doLayout(Renderer& renderer) override { doArrange(renderer, LayoutRect{x(), y(), width(), height()}); }

  private:
    [[nodiscard]] std::size_t columns(float width) const {
      return std::max<std::size_t>(1, static_cast<std::size_t>((width + m_gap) / (m_cellW + m_gap)));
    }
    float m_cellW;
    float m_cellH;
    float m_gap;
    float m_maxCellW;
  };

  // A flex cell with an input area over all of it: hover fill_1, click opens.
  class Cell final : public Flex {
  public:
    InputArea* area = nullptr;
    ColorSpec baseFill = clearColorSpec(); // the fill while not hovered

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      placeArea();
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      placeArea();
    }

  private:
    void placeArea() {
      if (area != nullptr) {
        area->setPosition(0.0F, 0.0F);
        area->setSize(width(), height());
      }
    }
  };

  // A 4 px capsule track with the used part in accent, or danger when nearly full.
  class UsageBar final : public Node {
  public:
    UsageBar(double used, bool warn, float height) : m_used(used) {
      m_track = static_cast<Box*>(addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Fill2), .radius = height / 2.0F})));
      m_level = static_cast<Box*>(addChild(ui::box({.fill = colorSpecFromRole(warn ? ColorRole::Danger : ColorRole::Accent),
                                                    .radius = height / 2.0F})));
      setSize(0.0F, height);
    }

  protected:
    LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
      return constraints.constrain(LayoutSize{constraints.hasMaxWidth ? constraints.maxWidth : 0.0F, height()});
    }
    void doLayout(Renderer& /*renderer*/) override {
      m_track->setSize(width(), height());
      m_level->setSize(std::round(width() * static_cast<float>(std::clamp(m_used, 0.0, 1.0))), height());
    }

  private:
    double m_used;
    Box* m_track = nullptr;
    Box* m_level = nullptr;
  };

  // The folder's drawn icon; it binds at the first layout, when a renderer is there.
  class FolderArt final : public Node {
  public:
    FolderArt(fs::path path, float size) : m_path(std::move(path)) {
      m_icon = static_cast<FileIcon*>(addChild(std::make_unique<FileIcon>(size)));
      setSize(size, size);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (!m_bound) {
        FileInfo info = statFile(m_path).value_or(FileInfo{.name = m_path.filename().string(), .isDir = true});
        m_icon->bind(renderer, info, m_path, nullptr);
        m_bound = true;
      }
    }

  private:
    fs::path m_path;
    FileIcon* m_icon = nullptr;
    bool m_bound = false;
  };

  std::unique_ptr<Cell> cell(float s, std::function<void()> open) {
    auto c = std::make_unique<Cell>();
    c->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
    auto areaOwned = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, .participatesInLayout = false});
    c->area = areaOwned.get();
    c->area->setZIndex(1); // above the icon and labels added after it: the whole cell hovers, not only the text
    Cell* body = c.get();
    c->area->setOnEnter([body](const InputArea::PointerData&) { body->setFill(colorSpecFromRole(ColorRole::Fill1)); });
    c->area->setOnLeave([body] { body->setFill(body->baseFill); });
    c->area->setOnClick([open = std::move(open)](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT && open) {
        open();
      }
    });
    c->addChild(std::move(areaOwned));
    return c;
  }

  std::unique_ptr<Node> heading(const char* text, float s) {
    auto row = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP16 * s, .height = Tokens::spaceP32 * s});
    row->addChild(ui::label({.text = std::string(text), .fontSize = Style::fontSizeCaption * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label2)}));
    return row;
  }

  std::unique_ptr<Node> driveCell(const ThisPcPlace& place, std::optional<double> used, bool warn, float s,
                                  std::function<void()> open) {
    auto c = cell(s, std::move(open));
    c->setDirection(FlexDirection::Horizontal);
    c->setAlign(FlexAlign::Center);
    c->setGap(Tokens::spaceP12 * s);
    c->setPadding(0.0F, Tokens::spaceP12 * s);
    c->baseFill = colorSpecFromRole(ColorRole::Fill1);
    c->setFill(c->baseFill);
    c->addChild(std::make_unique<PlaceIcon>(place.art, kDriveIcon * s));
    auto text = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s, .flexGrow = 1.0F});
    text->addChild(ui::label({.text = place.label, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                              .ellipsize = TextEllipsize::End}));
    if (used.has_value()) {
      text->addChild(std::make_unique<UsageBar>(*used, warn, kBar * s));
    }
    text->addChild(ui::label({.text = place.caption, .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(place.warn ? ColorRole::DangerText : ColorRole::Label2), .maxLines = 1,
                              .ellipsize = TextEllipsize::End}));
    c->addChild(std::move(text));
    return c;
  }
} // namespace

std::unique_ptr<Node> buildThisPc(float scale, ThisPcOptions options) {
  const float s = scale;
  auto shared = std::make_shared<ThisPcOptions>(std::move(options));
  const auto opener = [shared](ThisPcPlace place) {
    return [shared, place = std::move(place)] {
      if (shared->open) {
        shared->open(place);
      }
    };
  };

  ScrollView* scroll = nullptr;
  auto root = ui::scrollView({.out = &scroll, .contentScale = s, .scrollbarAutoHide = true, .fillWidth = true,
                              .fillHeight = true, .flexGrow = 1.0F});
  Flex* content = scroll->content();
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Tokens::spaceP8 * s);
  content->setPadding(Tokens::spaceP16 * s, 0.0F);

  content->addChild(heading("Folders", s));
  auto folders = std::make_unique<CellGrid>(kFolderCellW * s, kFolderCellH * s, Tokens::spaceP8 * s);
  for (const ThisPcPlace& place : shared->folders) {
    auto c = cell(s, opener(place));
    c->setDirection(FlexDirection::Vertical);
    c->setAlign(FlexAlign::Center);
    c->setJustify(FlexJustify::Center);
    c->setGap(Tokens::spaceP8 * s);
    c->addChild(std::make_unique<FolderArt>(place.path, Tokens::componentGridIcon * s));
    c->addChild(ui::label({.text = place.label, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                           .ellipsize = TextEllipsize::End}));
    folders->addChild(std::move(c));
  }
  auto foldersPad = ui::row({.align = FlexAlign::Stretch, .paddingH = Tokens::spaceP16 * s});
  folders->setFlexGrow(1.0F);
  foldersPad->addChild(std::move(folders));
  content->addChild(std::move(foldersPad));

  content->addChild(heading("Devices and drives", s));
  auto drives = std::make_unique<CellGrid>(kDriveMinW * s, kDriveH * s, Tokens::spaceP8 * s, kDriveMaxW * s);
  for (const Volume& v : listVolumes()) {
    const double used = v.totalBytes > 0 ? 1.0 - (static_cast<double>(v.freeBytes) / static_cast<double>(v.totalBytes)) : 0.0;
    ThisPcPlace place{.label = v.name,
                      .path = v.mountPoint,
                      .art = v.mountPoint == "/" ? "computer" : v.removable ? "usb" : "drive",
                      .caption = std::format("{} free of {}", formatFileSize(v.freeBytes), formatFileSize(v.totalBytes))};
    drives->addChild(driveCell(place, used, used >= kNearlyFull, s, opener(place)));
  }
  for (const ThisPcPlace& place : shared->extra) {
    drives->addChild(driveCell(place, std::nullopt, false, s, opener(place)));
  }
  auto drivesPad = ui::row({.align = FlexAlign::Stretch, .paddingH = Tokens::spaceP16 * s});
  drives->setFlexGrow(1.0F);
  drivesPad->addChild(std::move(drives));
  content->addChild(std::move(drivesPad));
  return root;
}

std::unique_ptr<Node> buildPlaceSections(float scale, std::vector<PlaceSection> sections,
                                         std::function<void(const ThisPcPlace&)> open, std::optional<PlaceAction> action) {
  const float s = scale;
  auto shared = std::make_shared<std::function<void(const ThisPcPlace&)>>(std::move(open));
  ScrollView* scroll = nullptr;
  auto root = ui::scrollView({.out = &scroll, .contentScale = s, .scrollbarAutoHide = true, .fillWidth = true,
                              .fillHeight = true, .flexGrow = 1.0F});
  Flex* content = scroll->content();
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Tokens::spaceP8 * s);
  content->setPadding(Tokens::spaceP16 * s, 0.0F);
  bool first = true;
  for (PlaceSection& section : sections) {
    auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP16 * s,
                         .height = Tokens::spaceP32 * s});
    head->addChild(ui::label({.text = section.title, .fontSize = Style::fontSizeCaption * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label2)}));
    if (!section.caption.empty()) {
      head->addChild(ui::label({.text = section.caption, .fontSize = Style::fontSizeCaption * s,
                                .color = colorSpecFromRole(ColorRole::Label3)}));
    }
    head->addChild(ui::spacer());
    if (first && action.has_value()) {
      head->addChild(ui::button({.text = action->label, .fontSize = Style::fontSizeBody * s,
                                 .variant = ButtonVariant::Secondary, .radius = Tokens::sizeControlM * s / 2.0F,
                                 .height = Tokens::sizeControlM * s, .onClick = action->run}));
    }
    first = false;
    content->addChild(std::move(head));
    auto grid = std::make_unique<CellGrid>(kDriveMinW * s, kDriveH * s, Tokens::spaceP8 * s, kDriveMaxW * s);
    for (const ThisPcPlace& place : section.places) {
      grid->addChild(driveCell(place, std::nullopt, false, s, [shared, place] {
        if (*shared) {
          (*shared)(place);
        }
      }));
    }
    auto pad = ui::row({.align = FlexAlign::Stretch, .paddingH = Tokens::spaceP16 * s});
    grid->setFlexGrow(1.0F);
    pad->addChild(std::move(grid));
    content->addChild(std::move(pad));
  }
  return root;
}


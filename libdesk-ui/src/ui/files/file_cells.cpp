#include "ui/files/file_cells.h"

#include "core/files/file_info.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace {
  constexpr float kCellRadius = 8.0F; // radius set B: cards and cells 8
  constexpr float kIcon = 64.0F;
  constexpr float kGlyph = 48.0F;
  constexpr float kNameGap = 4.0F;
  constexpr float kPillPadH = 6.0F;
  constexpr float kRowInset = 4.0F;
  constexpr float kNameMin = 160.0F;
  constexpr float kDateWidth = 140.0F;
  constexpr float kSizeWidth = 80.0F;
  constexpr float kKindWidth = 120.0F;

  ColorSpec selectionTint() { return colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F); }

  float itemOpacity(const FileCellState& state) { return state.disabled ? 0.38F : state.ghost ? 0.56F : 1.0F; }
} // namespace

FileIconCell::FileIconCell(float scale) : m_scale(scale) {
  m_plate = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(kCellRadius, scale)})));
  m_icon = static_cast<Glyph*>(addChild(ui::glyph({.glyphSize = kGlyph * scale})));
  m_namePill = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(4.0F, scale)})));
  m_name = static_cast<Label*>(addChild(ui::label({
      .fontSize = Style::fontSizeCaption * scale,
      .maxLines = 2,
      .textAlign = TextAlign::Center,
      .ellipsize = TextEllipsize::Middle,
  })));
  m_ring = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(kCellRadius, scale)})));
}

void FileIconCell::bind(Renderer& renderer, const FileInfo& info, float width, float height, FileCellState state) {
  const float s = m_scale;
  setSize(width, height);
  setOpacity(itemOpacity(state));

  const float icon = kIcon * s;
  const float plateX = std::round((width - icon - 8.0F * s) / 2.0F);
  m_plate->setPosition(plateX, 0.0F);
  m_plate->setSize(icon + 8.0F * s, icon + 8.0F * s);
  m_plate->setFill(state.selected ? colorSpecFromRole(ColorRole::Fill2)
                   : state.hovered ? colorSpecFromRole(ColorRole::Fill1)
                                   : clearColorSpec());

  m_icon->setGlyph(fileTypeGlyph(info));
  m_icon->setColor(colorSpecFromRole(info.isDir ? ColorRole::AccentText : ColorRole::Label2));
  m_icon->layout(renderer);
  m_icon->setPosition(std::round((width - m_icon->width()) / 2.0F), std::round(4.0F * s + (icon - m_icon->height()) / 2.0F));

  const float nameTop = icon + 8.0F * s + kNameGap * s;
  m_name->setText(info.displayName);
  m_name->setMaxWidth(width - 2.0F * kPillPadH * s);
  m_name->setColor(colorSpecFromRole(ColorRole::Label1));
  m_name->layout(renderer);
  const float nameW = m_name->width();
  m_name->setPosition(std::round((width - nameW) / 2.0F), nameTop);
  m_namePill->setPosition(std::round((width - nameW) / 2.0F - kPillPadH * s), nameTop - 1.0F * s);
  m_namePill->setSize(nameW + 2.0F * kPillPadH * s, m_name->height() + 2.0F * s);
  m_namePill->setFill(state.selected ? selectionTint() : clearColorSpec());

  m_ring->setPosition(0.0F, 0.0F);
  m_ring->setSize(width, height);
  m_ring->setFill(clearColorSpec());
  if (state.cursor) {
    m_ring->setBorder(focusRingColorSpec(), Style::focusRingWidth * s);
  } else {
    m_ring->setBorder(clearColorSpec(), 0.0F);
  }
}

FileListRow::Columns FileListRow::columnsFor(float width, float scale) {
  // icon 16 + paddings + gaps, then Name >= 160; drop Kind, then Date.
  const float fixed = (kRowInset * 2.0F + 8.0F * 2.0F + 16.0F + 8.0F) * scale;
  Columns c{.date = kDateWidth * scale, .size = kSizeWidth * scale, .kind = kKindWidth * scale};
  const auto used = [&] { return fixed + kNameMin * scale + c.date + c.size + c.kind + 8.0F * scale * 3.0F; };
  if (used() > width) {
    c.kind = 0.0F;
  }
  if (used() > width) {
    c.date = 0.0F;
  }
  return c;
}

FileListRow::FileListRow(float scale) : m_scale(scale) {
  m_background = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(kCellRadius, scale)})));
  m_row = static_cast<Flex*>(addChild(ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingH = (kRowInset + Style::spaceSm) * scale,
  })));
  m_icon = static_cast<Glyph*>(m_row->addChild(ui::glyph({.glyphSize = 16.0F * scale})));
  m_name = static_cast<Label*>(m_row->addChild(ui::label({
      .fontSize = Style::fontSizeBody * scale,
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
      .flexGrow = 1.0F,
  })));
  const auto column = [&](TextAlign align, const char* features) {
    return static_cast<Label*>(m_row->addChild(ui::label({
        .fontSize = Style::fontSizeCaption * scale,
        .fontFeatures = std::string(features),
        .maxLines = 1,
        .textAlign = align,
        .ellipsize = TextEllipsize::End,
    })));
  };
  m_date = column(TextAlign::Start, "");
  m_size = column(TextAlign::End, "tnum");
  m_kind = column(TextAlign::Start, "");
}

void FileListRow::bind(Renderer& renderer, const FileInfo& info, float width, FileCellState state) {
  const float s = m_scale;
  const float h = std::ceil(kHeight * s);
  setSize(width, h);
  setOpacity(itemOpacity(state));
  const Columns cols = columnsFor(width, s);

  m_icon->setGlyph(fileTypeGlyph(info));
  m_icon->setColor(colorSpecFromRole(info.isDir ? ColorRole::AccentText : ColorRole::Label2));
  m_name->setText(info.displayName);
  m_name->setColor(colorSpecFromRole(ColorRole::Label1));
  const auto setColumn = [&](Label* label, float w, const std::string& text) {
    label->setVisible(w > 0.0F);
    label->setParticipatesInLayout(w > 0.0F);
    label->setMinWidth(w);
    label->setMaxWidth(w);
    label->setText(text);
    label->setColor(colorSpecFromRole(ColorRole::Label2));
  };
  setColumn(m_date, cols.date, formatFileDate(info.mtimeNs));
  setColumn(m_size, cols.size, info.isDir ? std::string("—") : formatFileSize(info.size));
  setColumn(m_kind, cols.kind, fileKindLabel(info));

  const float inset = kRowInset * s;
  m_background->setPosition(inset, 0.0F);
  m_background->setSize(std::max(0.0F, width - 2.0F * inset), h);
  m_background->setFill(state.selected ? selectionTint()
                        : state.hovered ? colorSpecFromRole(ColorRole::Fill1)
                                        : clearColorSpec());
  if (state.cursor) {
    m_background->setBorder(focusRingColorSpec(), Style::focusRingWidth * s);
  } else {
    m_background->setBorder(clearColorSpec(), 0.0F);
  }
  m_row->setSize(width, h);
  m_row->layout(renderer);
}

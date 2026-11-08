#include "ui/files/file_cells.h"

#include "core/files/file_info.h"
#include "render/core/renderer.h"
#include "render/scene/accessible.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glib.h>

namespace {
  // Radius set B (user, 2026-11-08): cards and cells 8.
  const float& kCellRadius = Tokens::radiusR8;
  const float& kIcon = Tokens::componentGridIcon;
  const float& kNameGap = Tokens::spaceP4;
  const float& kPillPadH = Tokens::spaceP4;
  const float& kPlateW = Tokens::componentFilesIconPlateWidth;
  const float& kPlateH = Tokens::componentFilesIconPlateHeight;
  const float& kPlateTop = Tokens::componentFilesIconPlateTop;
  const float& kLabelW = Tokens::componentFilesLabelWidth;
  const float& kRowInset = Tokens::componentFilesRowInset;
  const float& kColumnRowInset = Tokens::componentListCardPadding;
  const float& kRowPad = Tokens::spaceP8;
  const float& kNameMin = Tokens::componentFilesNameMin;
  const float& kDateWidth = Tokens::componentFilesColumnDate;
  const float& kSizeWidth = Tokens::componentFilesColumnSize;
  const float& kKindWidth = Tokens::componentFilesColumnKind;
  const float& kKindPad = Tokens::componentFilesColumnKindPad;
  const float& kDimensionsWidth = Tokens::componentFilesColumnDimensions;
  const float& kDurationWidth = Tokens::componentFilesColumnDuration;
  const float& kArtistWidth = Tokens::componentFilesColumnArtist;

  ColorSpec selectionTint() { return colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F); }

  // Byte range in `name` of the first hit of `query`, both compared case-folded per character.
  std::optional<std::pair<std::size_t, std::size_t>> matchRange(const std::string& name, const std::string& query) {
    if (query.empty() || !g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr)) {
      return std::nullopt;
    }
    gchar* fq = g_utf8_casefold(query.data(), static_cast<gssize>(query.size()));
    const std::string q = fq != nullptr ? fq : "";
    g_free(fq);
    std::string folded;
    std::vector<std::size_t> origin; // folded byte -> start byte of its character in `name`
    for (const char* p = name.c_str(); *p != '\0';) {
      const char* next = g_utf8_next_char(p);
      gchar* f = g_utf8_casefold(p, next - p);
      for (const char* c = f; *c != '\0'; ++c) {
        folded.push_back(*c);
        origin.push_back(static_cast<std::size_t>(p - name.c_str()));
      }
      g_free(f);
      p = next;
    }
    const std::size_t at = q.empty() ? std::string::npos : folded.find(q);
    if (at == std::string::npos) {
      return std::nullopt;
    }
    std::size_t end = at + q.size();
    while (end < origin.size() && origin[end] == origin[end - 1]) {
      ++end;
    }
    return std::pair{origin[at], end < origin.size() ? origin[end] : name.size()};
  }

  std::string escaped(std::string_view text) {
    gchar* e = g_markup_escape_text(text.data(), static_cast<gssize>(text.size()));
    std::string out = e;
    g_free(e);
    return out;
  }

  float itemOpacity(const FileCellState& state) { return state.disabled ? 0.38F : state.ghost ? 0.56F : 1.0F; }

  float iconOf(const FileCellState& state) { return state.iconSize > 0.0F ? state.iconSize : kIcon; }

  void describe(Node& cell, const FileInfo& info, const FileCellState& state) {
    Accessible& a = cell.accessibleState();
    a.role = AccessibleRole::ListItem;
    a.name = info.displayName;
    a.selected = state.selected;
    a.disabled = state.disabled;
  }
} // namespace

std::string shownFileName(const FileInfo& info, bool hideExtension) {
  if (!hideExtension || info.isDir) {
    return info.displayName;
  }
  const auto dot = info.displayName.rfind('.');
  return dot == std::string::npos || dot == 0 ? info.displayName : info.displayName.substr(0, dot);
}

float FileIconCell::cellWidth(float iconSize, float scale) {
  return ((iconSize > 0.0F ? iconSize : kIcon) + Tokens::componentGridCellWidth - kIcon) * scale;
}

float FileIconCell::cellHeight(float iconSize, float scale) {
  return ((iconSize > 0.0F ? iconSize : kIcon) + Tokens::componentGridCellHeight - kIcon) * scale;
}

FileIconCell::FileIconCell(float scale) : m_scale(scale), m_iconSize(kIcon) {
  m_ring = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(kCellRadius + Style::focusRingWidth, scale)})));
  m_plate = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(kCellRadius, scale)})));
  m_mark = static_cast<Box*>(addChild(ui::box({.radius = Tokens::spaceP2 * scale, .width = Tokens::spaceP4 * scale,
                                                .height = Tokens::spaceP20 * scale, .visible = false})));
  m_icon = static_cast<FileIcon*>(addChild(std::make_unique<FileIcon>(kIcon * scale)));
  m_namePill = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(Tokens::radiusR4, scale)})));
  m_name = static_cast<Label*>(addChild(ui::label({
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = FontWeight::Medium,
      .maxLines = 2,
      .textAlign = TextAlign::Center,
      .ellipsize = TextEllipsize::Middle,
  })));
}

void FileIconCell::bind(Renderer& renderer, const FileInfo& info, float width, float height, FileCellState state) {
  describe(*this, info, state);
  const float s = m_scale;
  setSize(width, height);
  setOpacity(itemOpacity(state));
  setTransformOrigin(width / 2.0F, height / 2.0F);
  setScale(state.pressScale);

  const float iconSize = iconOf(state);
  const float plateW = std::round((iconSize + kPlateW - kIcon) * s);
  const float plateH = std::round((iconSize + kPlateH - kIcon) * s);
  const float plateX = std::round((width - plateW) / 2.0F);
  const float plateY = std::round(kPlateTop * s);
  m_plate->setPosition(plateX, plateY);
  m_plate->setSize(plateW, plateH);
  m_plate->setFill(state.dropTarget ? selectionTint()
                   : state.selected || state.pressScale < 1.0F ? colorSpecFromRole(ColorRole::Fill2)
                   : state.hovered ? colorSpecFromRole(ColorRole::Fill1)
                                   : clearColorSpec());

  // The compare mark sits at the cell's left edge, centred on the plate.
  m_mark->setVisible(state.mark.has_value());
  if (state.mark) {
    m_mark->setFill(*state.mark);
    m_mark->setPosition(0.0F, std::round(plateY + (plateH - Tokens::spaceP20 * s) / 2.0F));
    m_mark->setSize(Tokens::spaceP4 * s, Tokens::spaceP20 * s);
  }

  if (std::abs(m_iconSize - iconSize) > 0.5F) {
    (void)removeChild(m_icon);
    m_icon = static_cast<FileIcon*>(addChild(std::make_unique<FileIcon>(iconSize * s)));
    m_iconSize = iconSize;
  }
  m_icon->bind(renderer, info, state.path, state.thumbnails, state.duration);
  m_icon->setPosition(std::round(plateX + (plateW - m_icon->width()) / 2.0F),
                      std::round(plateY + (plateH - m_icon->height()) / 2.0F));

  const float nameTop = plateY + plateH + kNameGap * s;
  m_name->setText(shownFileName(info, state.hideExtension));
  m_name->setMaxWidth(std::min(width, (iconSize + kLabelW - kIcon) * s) - 2.0F * kPillPadH * s);
  m_name->setColor(colorSpecFromRole(ColorRole::Label1));
  m_name->layout(renderer);
  const float nameW = m_name->width();
  m_name->setPosition(std::round((width - nameW) / 2.0F), nameTop);
  m_namePill->setPosition(std::round((width - nameW) / 2.0F - kPillPadH * s), nameTop);
  m_namePill->setSize(nameW + (kPillPadH + kPillPadH) * s, m_name->height());
  m_namePill->setFill(!state.selected  ? clearColorSpec()
                      : state.inactive ? colorSpecFromRole(ColorRole::Fill2)
                                       : selectionTint());

  // Focus ring: 2 px, 2 px outside the plate, only after a key (state.cursor).
  const float ring = Style::focusRingWidth * s;
  m_ring->setPosition(plateX - ring - ring, plateY - ring - ring);
  m_ring->setSize(plateW + ring * 4.0F, plateH + ring * 4.0F);
  m_ring->setFill(clearColorSpec());
  if (state.dropTarget) {
    m_ring->setBorder(colorSpecFromRole(ColorRole::Accent), ring);
  } else if (state.cursor) {
    m_ring->setBorder(focusRingColorSpec(), ring);
  } else {
    m_ring->setBorder(clearColorSpec(), 0.0F);
  }
}

Node* FileIconCell::nameNode() const noexcept { return m_name; }

Node* FileListRow::nameNode() const noexcept { return m_name; }

float FileListRow::textInset(float scale) { return (kRowInset + kRowPad) * scale; }

std::pair<float, float> FileListRow::disclosureSpan(int depth, float scale) {
  const float x = textInset(scale) + static_cast<float>(depth) * Tokens::spaceP16 * scale;
  return {x - Tokens::spaceP4 * scale, x + (Tokens::sizeSymbolInline + Tokens::spaceP4) * scale};
}

FileListRow::Columns FileListRow::columnsFor(float width, float scale, bool where, std::uint8_t extra) {
  // Row insets and paddings, then Name >= 120; drop the optional columns (last first), then Kind, then Date.
  const float fixed = 2.0F * textInset(scale);
  if (!where && extra != 0) {
    Columns c = columnsFor(1.0e6F, scale);
    const auto fits = [&] { return fixed + kNameMin * scale + c.date + c.size + c.kind + c.dimensions + c.duration + c.artist <= width; };
    c.dimensions = (extra & file_columns::kDimensions) != 0 ? kDimensionsWidth * scale : 0.0F;
    c.duration = (extra & file_columns::kDuration) != 0 ? kDurationWidth * scale : 0.0F;
    c.artist = (extra & file_columns::kArtist) != 0 ? kArtistWidth * scale : 0.0F;
    for (float* drop : {&c.artist, &c.duration, &c.dimensions, &c.kind, &c.date}) {
      if (fits()) {
        break;
      }
      *drop = 0.0F;
    }
    return c;
  }
  if (where) {
    Columns c{.date = kDateWidth * scale, .where = Tokens::componentFilesColumnWhere * scale};
    if (fixed + kNameMin * scale + c.date + c.where > width) {
      c.date = 0.0F;
    }
    return c;
  }
  Columns c{.date = kDateWidth * scale, .size = kSizeWidth * scale, .kind = kKindWidth * scale};
  const auto used = [&] { return fixed + kNameMin * scale + c.date + c.size + c.kind; };
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
  m_row = static_cast<Flex*>(addChild(ui::row({.align = FlexAlign::Center, .paddingH = kRowPad * scale})));
  auto name = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .minWidth = kNameMin * scale,
                       .clipChildren = true, .flexGrow = 1.0F});
  m_indent = static_cast<Flex*>(name->addChild(ui::row({})));
  m_disclosure = static_cast<Glyph*>(name->addChild(ui::glyph({.glyph = std::string("chevron-right"),
                                                               .glyphSize = Tokens::sizeSymbolInline * scale,
                                                               .color = colorSpecFromRole(ColorRole::Label3)})));
  m_mark = static_cast<Box*>(name->addChild(ui::box({.radius = Tokens::spaceP2 * scale, .width = Tokens::spaceP4 * scale,
                                                      .height = Tokens::spaceP20 * scale, .visible = false,
                                                      .participatesInLayout = false})));
  m_icon = static_cast<FileIcon*>(name->addChild(std::make_unique<FileIcon>(Tokens::spaceP20 * scale)));
  m_match = static_cast<Box*>(name->addChild(ui::box({.radius = Tokens::spaceP2 * scale, .visible = false,
                                                       .participatesInLayout = false})));
  m_name = static_cast<Label*>(name->addChild(ui::label({
      .fontSize = Style::fontSizeBody * scale,
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
      .flexGrow = 1.0F,
  })));
  m_row->addChild(std::move(name));
  const auto column = [&](Flex* parent, TextAlign align, const char* features) {
    return static_cast<Label*>(parent->addChild(ui::label({
        .fontSize = Style::fontSizeBody * scale,
        .fontFeatures = std::string(features),
        .maxLines = 1,
        .textAlign = align,
        .ellipsize = TextEllipsize::End,
    })));
  };
  m_where = column(m_row, TextAlign::Start, "");
  m_date = column(m_row, TextAlign::Start, "");
  m_size = column(m_row, TextAlign::End, "tnum");
  m_kindBox = static_cast<Flex*>(m_row->addChild(ui::row({.align = FlexAlign::Center})));
  m_kindBox->setPadding(0.0F, 0.0F, 0.0F, kKindPad * scale);
  m_kind = column(m_kindBox, TextAlign::Start, "");
  m_kind->setFlexGrow(1.0F);
  m_dimensions = column(m_row, TextAlign::Start, "tnum");
  m_duration = column(m_row, TextAlign::End, "tnum");
  // Duration is right aligned like Size; Artist starts one column gap after it.
  m_artistBox = static_cast<Flex*>(m_row->addChild(ui::row({.align = FlexAlign::Center})));
  m_artistBox->setPadding(0.0F, 0.0F, 0.0F, kKindPad * scale);
  m_artist = column(m_artistBox, TextAlign::Start, "");
  m_artist->setFlexGrow(1.0F);
  m_chevron = static_cast<Glyph*>(m_row->addChild(ui::glyph({.glyph = std::string("chevron-right"),
                                                            .glyphSize = Tokens::sizeSymbolInline * scale,
                                                            .color = colorSpecFromRole(ColorRole::Label3)})));
}

void FileListRow::bind(Renderer& renderer, const FileInfo& info, float width, FileCellState state) {
  describe(*this, info, state);
  const float s = m_scale;
  const float h = std::ceil(Tokens::sizeRowCompact * s);
  setSize(width, h);
  setOpacity(itemOpacity(state));
  const Columns cols = state.nameOnly ? Columns{} : columnsFor(width, s, !state.where.empty(), state.extraColumns);

  m_icon->bind(renderer, info, state.path, state.thumbnails);
  // Mockup .hit b: the matched text 500 on an accent 20 % r2 pill.
  const std::string shown = shownFileName(info, state.hideExtension);
  const auto hit = matchRange(shown, state.match);
  const std::string_view full = shown;
  m_name->setUseMarkup(hit.has_value());
  m_name->setText(!hit ? shown
                       : escaped(full.substr(0, hit->first)) + "<span weight=\"500\">" +
                             escaped(full.substr(hit->first, hit->second - hit->first)) + "</span>" +
                             escaped(full.substr(hit->second)));
  m_name->setColor(colorSpecFromRole(ColorRole::Label1));
  // Mockup .disc: 12 wide, label_3; files keep the slot so names line up; 16 indent per level.
  using Disclosure = FileCellState::Disclosure;
  const bool disc = state.disclosure != Disclosure::None && !state.nameOnly;
  const float indent = disc ? static_cast<float>(std::max(0, state.depth)) * Tokens::spaceP16 * s : 0.0F;
  // The name row's gap sits after the indent box; a zero indent drops the box from layout.
  m_indent->setVisible(indent > 0.0F);
  m_indent->setParticipatesInLayout(indent > 0.0F);
  m_indent->setMinWidth(std::max(0.0F, indent - Tokens::spaceP8 * s));
  m_indent->setMaxWidth(std::max(0.0F, indent - Tokens::spaceP8 * s));
  m_disclosure->setVisible(disc);
  m_disclosure->setParticipatesInLayout(disc);
  m_disclosure->setOpacity(state.disclosure == Disclosure::Leaf ? 0.0F : 1.0F);
  m_disclosure->setGlyph(state.disclosure == Disclosure::Expanded ? "chevron-down" : "chevron-right");
  m_mark->setVisible(state.mark.has_value());
  m_mark->setParticipatesInLayout(state.mark.has_value());
  if (state.mark) {
    m_mark->setFill(*state.mark);
  }
  const auto setColumn = [&](Node* node, Label* label, float w, const std::string& text) {
    node->setVisible(w > 0.0F);
    node->setParticipatesInLayout(w > 0.0F);
    if (auto* flex = dynamic_cast<Flex*>(node)) {
      flex->setMinWidth(w);
      flex->setMaxWidth(w);
    } else {
      label->setMinWidth(w);
      label->setMaxWidth(w);
    }
    label->setText(text);
    label->setColor(colorSpecFromRole(ColorRole::Label2));
  };
  // Until the background stat reaches this row, Date and Size stay blank (FB-SPEED).
  setColumn(m_date, m_date, cols.date, info.pendingStat ? std::string() : formatFileDate(info.mtimeNs));
  setColumn(m_size, m_size, cols.size, info.isDir ? std::string("—") : info.pendingStat ? std::string() : formatFileSize(info.size));
  setColumn(m_where, m_where, cols.where, state.where);
  setColumn(m_kindBox, m_kind, cols.kind, fileKindLabel(info));
  m_kind->setMaxWidth(std::max(0.0F, cols.kind - kKindPad * s));
  setColumn(m_dimensions, m_dimensions, cols.dimensions, state.dimensions);
  setColumn(m_duration, m_duration, cols.duration, state.duration);
  setColumn(m_artistBox, m_artist, cols.artist, state.artist);
  m_artist->setMaxWidth(std::max(0.0F, cols.artist - kKindPad * s));
  const bool chevron = state.nameOnly && info.isDir;
  m_chevron->setVisible(chevron);
  m_chevron->setParticipatesInLayout(chevron);

  const float inset = (state.nameOnly ? kColumnRowInset : kRowInset) * s;
  m_background->setPosition(inset, 0.0F);
  m_background->setSize(std::max(0.0F, width - inset - inset), h);
  m_background->setFill(state.selected && state.inactive ? colorSpecFromRole(ColorRole::Fill2)
                        : state.selected || state.dropTarget ? selectionTint()
                        : state.hovered ? colorSpecFromRole(ColorRole::Fill1)
                                        : clearColorSpec());
  if (state.dropTarget) {
    m_background->setBorder(colorSpecFromRole(ColorRole::Accent), Style::focusRingWidth * s);
  } else if (state.cursor) {
    m_background->setBorder(focusRingColorSpec(), Style::focusRingWidth * s);
  } else {
    m_background->setBorder(clearColorSpec(), 0.0F);
  }
  m_row->setPosition(inset, 0.0F);
  m_row->setSize(std::max(0.0F, width - inset - inset), h);
  m_row->layout(renderer);

  bool pill = false;
  if (hit) {
    TextFont font = m_name->font();
    const float size = m_name->fontSize();
    const float before = renderer.measureText(full.substr(0, hit->first), size, font).width;
    font.weight = FontWeight::Medium;
    const float matchW = renderer.measureText(full.substr(hit->first, hit->second - hit->first), size, font).width;
    const float total = renderer.measureText(m_name->text(), size, m_name->font(), 0.0F, 0, TextAlign::Start,
                                             TextEllipsize::End, true).width;
    // A middle ellipsis keeps the first half of the name in place.
    const float room = m_name->width();
    pill = total <= room + 0.5F || before + matchW <= room / 2.0F;
    if (pill) {
      const float lineH = Tokens::spaceP16 * s;
      m_match->setPosition(m_name->x() + m_name->textBaseX() + before, std::round(m_name->y() + (m_name->height() - lineH) / 2.0F));
      m_match->setSize(matchW, lineH);
      m_match->setFill(colorSpecFromRole(ColorRole::Accent, 0.20F));
    }
  }
  m_match->setVisible(pill);
}

FileGroupHeader::FileGroupHeader(float scale) : m_scale(scale) {
  m_title = static_cast<Label*>(addChild(ui::label({.fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::SemiBold,
                                                   .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1})));
  m_count = static_cast<Label*>(addChild(ui::label({.fontSize = Style::fontSizeCaption * scale, .fontWeight = FontWeight::Medium,
                                                   .fontFeatures = std::string("tnum"),
                                                   .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1})));
  m_rule = static_cast<Box*>(addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator)})));
}

void FileGroupHeader::bind(Renderer& renderer, const std::string& title, std::size_t count, float width, float height,
                           float inset) {
  const float s = m_scale;
  setSize(width, height);
  Accessible& a = accessibleState();
  a.role = AccessibleRole::Label;
  a.name = title;
  m_title->setText(title);
  m_title->setMaxWidth(std::max(1.0F, width - 2.0F * inset - Tokens::spaceP48 * s));
  m_title->layout(renderer);
  m_count->setText(std::to_string(count));
  m_count->layout(renderer);
  const float y = std::round((height - m_title->height()) / 2.0F);
  m_title->setPosition(inset, y);
  m_count->setPosition(inset + m_title->width() + Tokens::spaceP8 * s,
                       std::round(y + (m_title->height() - m_count->height()) / 2.0F));
  const float rule = Style::borderWidth * s;
  m_rule->setPosition(inset, height - rule);
  m_rule->setSize(std::max(0.0F, width - inset - inset), rule);
}

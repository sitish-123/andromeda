#include "file_row.h"

#include "files/file_info.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cmath>
#include <ctime>
#include <format>
#include <linux/input-event-codes.h>

namespace {
  constexpr float kRowHeight = 32.0F;
  constexpr float kSizeWidth = 80.0F;
  constexpr float kKindWidth = 120.0F;
  constexpr float kDateWidth = 140.0F;
  constexpr std::uint32_t kDoubleClickMs = 400;
  std::uint32_t g_modifiers = 0;
} // namespace

std::string formatSize(std::uint64_t bytes) {
  static constexpr std::array<const char*, 5> kUnits{"bytes", "KB", "MB", "GB", "TB"};
  if (bytes < 1000) {
    return std::format("{} {}", bytes, bytes == 1 ? "byte" : "bytes");
  }
  auto value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1000.0 && unit + 1 < kUnits.size()) {
    value /= 1000.0;
    ++unit;
  }
  return value >= 10.0 ? std::format("{:.0f} {}", value, kUnits[unit]) : std::format("{:.1f} {}", value, kUnits[unit]);
}

std::string formatModified(std::int64_t mtimeNs) {
  const std::time_t t = static_cast<std::time_t>(mtimeNs / 1'000'000'000);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  std::tm today{};
  ::localtime_r(&t, &local);
  ::localtime_r(&now, &today);
  std::array<char, 64> buf{};
  const char* fmt = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday ? "Today, %H:%M"
      : local.tm_year == today.tm_year                                             ? "%d %b, %H:%M"
                                                                                   : "%d %b %Y";
  std::strftime(buf.data(), buf.size(), fmt, &local);
  return buf.data();
}

const char* glyphForType(const FileInfo& info) {
  if (info.isDir) {
    return "folder";
  }
  const std::string& t = info.contentType;
  if (t.starts_with("image/")) {
    return "photo";
  }
  if (t.starts_with("video/")) {
    return "movie";
  }
  if (t.starts_with("audio/")) {
    return "music";
  }
  if (t == "application/pdf") {
    return "file-type-pdf";
  }
  if (t.starts_with("text/")) {
    return "file-text";
  }
  if (t.find("zip") != std::string::npos || t.find("tar") != std::string::npos || t.find("compressed") != std::string::npos) {
    return "file-zip";
  }
  return "file";
}

void FileRow::setModifiers(std::uint32_t modifiers) noexcept { g_modifiers = modifiers; }

float FileRow::height(float scale) noexcept { return std::ceil(kRowHeight * scale); }

FileRow::FileRow(float scale, Callbacks callbacks) : m_scale(scale), m_callbacks(std::move(callbacks)) {
  setPropagateEvents(true);
  setOnPress([this](const InputArea::PointerData& data) {
    if (!data.pressed) {
      return;
    }
    if (data.button == BTN_RIGHT) {
      if (m_callbacks.menu) {
        m_callbacks.menu(m_index, data.sceneX, data.sceneY);
      }
      return;
    }
    if (data.button != BTN_LEFT) {
      return;
    }
    const bool second = m_lastClickMs != 0 && data.time - m_lastClickMs < kDoubleClickMs;
    m_lastClickMs = second ? 0 : data.time;
    if (second && m_callbacks.open) {
      m_callbacks.open(m_index);
    } else if (m_callbacks.select) {
      m_callbacks.select(m_index, g_modifiers);
    }
  });

  m_background = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadiusSm(scale)})));
  m_row = static_cast<Flex*>(addChild(ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingH = Style::spaceSm * scale,
  })));
  m_icon = static_cast<Glyph*>(m_row->addChild(ui::glyph({.glyphSize = 16.0F * scale})));
  m_name = static_cast<Label*>(m_row->addChild(ui::label({
      .fontSize = Style::fontSizeBody * scale,
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
      .flexGrow = 1.0F,
  })));
  const auto column = [&](float width, TextAlign align) {
    return static_cast<Label*>(m_row->addChild(ui::label({
        .fontSize = Style::fontSizeCaption * scale,
        .width = width * scale,
        .maxLines = 1,
        .textAlign = align,
        .ellipsize = TextEllipsize::End,
    })));
  };
  m_size = column(kSizeWidth, TextAlign::End);
  m_kind = column(kKindWidth, TextAlign::Start);
  m_date = column(kDateWidth, TextAlign::Start);
}

void FileRow::bind(
    Renderer& renderer, const FileInfo& info, std::size_t index, float width, bool selected, bool hovered
) {
  if (m_index != index) {
    m_lastClickMs = 0;
  }
  m_index = index;
  m_icon->setGlyph(glyphForType(info));
  m_name->setText(info.displayName);
  m_size->setText(info.isDir ? "--" : formatSize(info.size));
  m_kind->setText(info.isDir ? "Folder" : info.contentType);
  m_date->setText(formatModified(info.mtimeNs));

  const ColorSpec clear = clearColorSpec();
  const ColorSpec fg = colorSpecFromRole(selected ? ColorRole::OnAccent : ColorRole::Label1);
  const ColorSpec sub = colorSpecFromRole(selected ? ColorRole::OnAccent : ColorRole::Label2);
  m_background->setFill(selected ? colorSpecFromRole(ColorRole::Accent) : hovered ? colorSpecFromRole(ColorRole::Fill1) : clear);
  m_icon->setColor(selected ? fg : colorSpecFromRole(info.isDir ? ColorRole::AccentText : ColorRole::Label2));
  m_name->setColor(fg);
  m_size->setColor(sub);
  m_kind->setColor(sub);
  m_date->setColor(sub);

  const float h = height(m_scale);
  setSize(width, h);
  m_background->setSize(width, h);
  m_row->setSize(width, h);
  layout(renderer);
}

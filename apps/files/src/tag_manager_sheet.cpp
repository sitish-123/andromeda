#include "tag_manager_sheet.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

namespace {
  constexpr Logger kLog("tags");
  constexpr float kWidth = 400.0F;   // FILES-UI-SPEC §17: narrower than the 480 dialog, wider than mockup's 356
  constexpr float kRow = 40.0F;
  constexpr float kRowWidth = kWidth - 2.0F * 24.0F - 2.0F * 4.0F; // dialog padding 24, card padding 4
  constexpr std::size_t kRowsShown = 6; // more rows scroll
  constexpr float kDot = 12.0F;
  constexpr float kSwatch = 20.0F;
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;

  ColorSpec rgbSpec(std::uint32_t rgb) { return ColorSpec{.role = std::nullopt, .fixed = rgbHex(rgb), .alpha = 1.0F}; }

  std::string nameError(const std::string& name) {
    const bool blank = name.find_first_not_of(" \t") == std::string::npos;
    const bool control = std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    return blank || control || name.find(',') != std::string::npos ? "A tag name can’t be empty or contain a comma."
                                                                    : std::string();
  }

  std::string trimmed(std::string name) {
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    return name;
  }
} // namespace

TagManagerSheet::TagManagerSheet(Host host) : m_host(std::move(host)), m_tags(loadTagConfig()) {
  for (std::string& name : m_host.unlisted(m_tags)) {
    m_tags.push_back({.name = std::move(name), .rgb = kColors[std::size(kColors) - 1], .sidebar = true});
  }
  m_draft = m_tags.empty() ? std::string() : m_tags.front().name;
  kLog.debug("tag manager open, {} tags", m_tags.size());
}

std::size_t TagManagerSheet::find(const std::string& name) const {
  const auto it = std::ranges::find(m_tags, name, &TagDef::name);
  return it == m_tags.end() ? m_tags.size() : static_cast<std::size_t>(it - m_tags.begin());
}

void TagManagerSheet::save() {
  if (!saveTagConfig(m_tags)) {
    kLog.warn("cannot write {}", tagConfigPath().string());
  }
  m_host.configChanged();
}

void TagManagerSheet::rebuildLater() {
  DeferredCall::callLater([rebuild = m_host.rebuild] { rebuild(); });
}

void TagManagerSheet::commitDraft() {
  if (m_selected < m_tags.size() && m_draft != m_tags[m_selected].name) {
    if (!rename(m_tags[m_selected].name, m_draft)) {
      m_draft = m_tags[m_selected].name; // a refused name goes back to the stored one
    }
  }
}

bool TagManagerSheet::select(const std::string& name) {
  const std::size_t index = find(name);
  if (index == m_tags.size()) {
    return false;
  }
  if (index != m_selected) {
    commitDraft();
    m_selected = index;
    m_draft = m_tags[index].name;
    m_error.clear();
    m_confirmDelete = false;
  }
  rebuildLater();
  return true;
}

bool TagManagerSheet::rename(const std::string& old, const std::string& rawNow) {
  const std::size_t index = find(old);
  const std::string now = trimmed(rawNow);
  if (index == m_tags.size() || now == old) {
    return false;
  }
  m_error = nameError(now);
  if (m_error.empty() && find(now) != m_tags.size()) {
    m_error = std::format("There is already a tag named “{}”.", now);
  }
  if (!m_error.empty()) {
    kLog.debug("tag rename refused: {}", m_error);
    rebuildLater();
    return false;
  }
  kLog.info("tag rename {} -> {}", old, now);
  m_tags[index].name = now;
  m_retired.push_back(old);
  std::erase(m_retired, now);
  if (index == m_selected) {
    m_draft = now;
  }
  save();
  m_host.renameOnFiles(old, now);
  rebuildLater();
  return true;
}

bool TagManagerSheet::move(const std::string& name, int delta) {
  const std::size_t index = find(name);
  const auto target = static_cast<std::ptrdiff_t>(index) + delta;
  if (index == m_tags.size() || target < 0 || target >= static_cast<std::ptrdiff_t>(m_tags.size())) {
    return false;
  }
  commitDraft();
  const TagDef moved = m_tags[index];
  m_tags.erase(m_tags.begin() + static_cast<std::ptrdiff_t>(index));
  m_tags.insert(m_tags.begin() + target, moved);
  m_selected = static_cast<std::size_t>(target);
  m_draft = moved.name;
  kLog.info("tag move {} to {}", moved.name, target);
  save();
  rebuildLater();
  return true;
}

bool TagManagerSheet::setSidebar(const std::string& name, bool shown) {
  const std::size_t index = find(name);
  if (index == m_tags.size()) {
    return false;
  }
  m_tags[index].sidebar = shown;
  kLog.info("tag sidebar {} {}", name, shown ? "on" : "off");
  save();
  rebuildLater();
  return true;
}

bool TagManagerSheet::setColor(const std::string& name, std::uint32_t rgb) {
  const std::size_t index = find(name);
  if (index == m_tags.size()) {
    return false;
  }
  m_tags[index].rgb = rgb & 0xffffffU;
  kLog.info("tag color {} #{:06x}", name, m_tags[index].rgb);
  save();
  rebuildLater();
  return true;
}

bool TagManagerSheet::remove(const std::string& name) {
  const std::size_t index = find(name);
  if (index == m_tags.size()) {
    return false;
  }
  kLog.info("tag delete {}", name);
  m_retired.push_back(name);
  m_tags.erase(m_tags.begin() + static_cast<std::ptrdiff_t>(index));
  m_selected = m_tags.empty() ? 0 : std::min(index, m_tags.size() - 1);
  m_draft = m_tags.empty() ? std::string() : m_tags[m_selected].name;
  m_error.clear();
  m_confirmDelete = false;
  save();
  m_host.removeFromFiles(name);
  rebuildLater();
  return true;
}

void TagManagerSheet::addTag() {
  commitDraft();
  std::string name = "New Tag";
  for (int n = 2; find(name) != m_tags.size(); ++n) {
    name = std::format("New Tag {}", n);
  }
  // The first Finder colour no tag uses yet, else grey.
  std::uint32_t rgb = kColors[std::size(kColors) - 1];
  for (const std::uint32_t c : kColors) {
    if (std::ranges::none_of(m_tags, [&](const TagDef& t) { return t.rgb == c; })) {
      rgb = c;
      break;
    }
  }
  m_tags.push_back({.name = name, .rgb = rgb, .sidebar = true});
  std::erase(m_retired, name);
  m_selected = m_tags.size() - 1;
  m_draft = name;
  m_error.clear();
  m_confirmDelete = false;
  m_focusName = true;
  kLog.info("tag new {}", name);
  save();
  rebuildLater();
}

void TagManagerSheet::done() {
  commitDraft();
  if (!m_error.empty()) {
    return;
  }
  DeferredCall::callLater([close = m_host.close] { close(); });
}

void TagManagerSheet::indexChanged() {
  bool added = false;
  for (std::string& name : m_host.unlisted(m_tags)) {
    if (!std::ranges::contains(m_retired, name)) {
      m_tags.push_back({.name = std::move(name), .rgb = kColors[std::size(kColors) - 1], .sidebar = true});
      added = true;
    }
  }
  if (added) {
    rebuildLater();
    return;
  }
  for (const auto& [name, label] : m_counts) {
    const std::size_t n = m_host.count(name);
    label->setText(n == 1 ? "1 item" : std::format("{} items", n));
  }
}

std::unique_ptr<Node> TagManagerSheet::buildRow(std::size_t index, float s) {
  const TagDef& tag = m_tags[index];
  const bool selected = index == m_selected;
  auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingH = Tokens::spaceP12 * s,
                      .fill = selected ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F)
                                       : clearColorSpec(),
                      .radius = Tokens::radiusR4 * s, .width = kRowWidth * s, .height = kRow * s});
  row->addChild(ui::checkbox({.checked = tag.sidebar, .scale = s,
                              .onChange = [this, name = tag.name](bool on) { setSidebar(name, on); }}));
  row->addChild(ui::box({.fill = rgbSpec(tag.rgb), .radius = kDot / 2.0F * s, .width = kDot * s, .height = kDot * s}));
  row->addChild(ui::label({.text = tag.name, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                           .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
  const std::size_t n = m_host.count(tag.name);
  Label* count = nullptr;
  row->addChild(ui::label({.out = &count, .text = n == 1 ? std::string("1 item") : std::format("{} items", n),
                           .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::Label3)}));
  m_counts[tag.name] = count;
  return ui::inputArea({.propagateEvents = true, .width = kRowWidth * s, .height = kRow * s,
                        .onClick = [this, name = tag.name](const InputArea::PointerData&) { select(name); }},
                       std::move(row));
}

std::unique_ptr<Node> TagManagerSheet::buildEditor(float s) {
  auto editor = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
  if (m_selected >= m_tags.size()) {
    return editor;
  }
  const TagDef& tag = m_tags[m_selected];
  const auto fieldRow = [&](const char* label) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    // A label alone does not keep its width: the cell holds the field column in place.
    auto cell = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .width = kFieldLabel * s});
    if (*label != '\0') {
      cell->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeCaption * s,
                                .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2)}));
    }
    row->addChild(std::move(cell));
    return row;
  };

  auto name = fieldRow("Name");
  name->addChild(ui::input({.value = m_draft, .placeholder = std::string("Tag name"), .fontSize = Style::fontSizeBody * s,
                            .controlHeight = Style::controlHeight * s, .invalid = !m_error.empty(), .flexGrow = 1.0F,
                            .onChange = [this](const std::string& v) { m_draft = v; },
                            .onSubmit = [this](const std::string&) { commitDraft(); },
                            .configure = [this](Input& input) {
                              if (std::exchange(m_focusName, false) && m_host.focus) {
                                input.selectAll();
                                m_host.focus(input);
                              }
                            }}));
  editor->addChild(std::move(name));

  auto colors = fieldRow("Colour");
  auto swatches = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  for (const std::uint32_t c : kColors) {
    const bool on = c == tag.rgb;
    auto swatch = ui::box({.fill = rgbSpec(c), .border = on ? colorSpecFromRole(ColorRole::Label1) : clearColorSpec(),
                           .borderWidth = on ? 2.0F * s : 0.0F, .radius = kSwatch / 2.0F * s, .width = kSwatch * s,
                           .height = kSwatch * s});
    swatches->addChild(ui::inputArea({.width = kSwatch * s, .height = kSwatch * s,
                                      .onClick = [this, name = tag.name, c](const InputArea::PointerData&) {
                                        setColor(name, c);
                                      }},
                                     std::move(swatch)));
  }
  colors->addChild(std::move(swatches));
  editor->addChild(std::move(colors));

  if (!m_error.empty()) {
    auto error = fieldRow("");
    error->addChild(ui::label({.text = m_error, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                               .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 2, .flexGrow = 1.0F}));
    editor->addChild(std::move(error));
  }

  auto actions = fieldRow("");
  if (m_confirmDelete) {
    const std::size_t n = m_host.count(tag.name);
    actions->addChild(ui::label({.text = n == 0 ? std::format("Delete “{}”?", tag.name)
                                                : std::format("Remove “{}” from {}?", tag.name,
                                                              n == 1 ? std::string("1 item") : std::format("{} items", n)),
                                 .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                                 .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                                 .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
    actions->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this] {
                                    m_confirmDelete = false;
                                    rebuildLater();
                                  }}));
    actions->addChild(ui::button({.text = std::string("Delete"), .variant = ButtonVariant::Destructive,
                                  .onClick = [this, name = tag.name] {
                                    DeferredCall::callLater([this, name] { remove(name); });
                                  }}));
  } else {
    actions->addChild(ui::button({.glyph = std::string("chevron-up"), .enabled = m_selected > 0,
                                  .variant = ButtonVariant::Default, .tooltip = std::string("Move Up"),
                                  .onClick = [this, name = tag.name] { move(name, -1); }}));
    actions->addChild(ui::button({.glyph = std::string("chevron-down"), .enabled = m_selected + 1 < m_tags.size(),
                                  .variant = ButtonVariant::Default, .tooltip = std::string("Move Down"),
                                  .onClick = [this, name = tag.name] { move(name, 1); }}));
    actions->addChild(ui::spacer());
    actions->addChild(ui::button({.text = std::string("Delete Tag…"), .variant = ButtonVariant::Default, .onClick = [this] {
                                    m_confirmDelete = true;
                                    rebuildLater();
                                  }}));
  }
  editor->addChild(std::move(actions));
  return editor;
}

std::unique_ptr<Node> TagManagerSheet::build(float scale) {
  const float s = scale;
  m_counts.clear();
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = kWidth * s});

  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = std::string("Tags"), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  auto card = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s,
                          .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Tokens::radiusR8 * s,
                          .border = colorSpecFromRole(ColorRole::GlassCardEdge), .borderWidth = std::max(1.0F, 0.5F * s)});
  if (m_tags.empty()) {
    card->addChild(ui::label({.text = std::string("No tags yet. New Tag adds one."), .fontSize = Style::fontSizeBody * s,
                              .color = colorSpecFromRole(ColorRole::Label2), .height = kRow * s}));
  } else {
    auto scroll = ui::scrollView({.contentScale = s, .scrollbarAutoHide = true,
                                  .height = static_cast<float>(std::min(m_tags.size(), kRowsShown)) * kRow * s});
    scroll->content()->setGap(0.0F);
    scroll->content()->setAlign(FlexAlign::Stretch);
    for (std::size_t i = 0; i < m_tags.size(); ++i) {
      scroll->content()->addChild(buildRow(i, s));
    }
    card->addChild(std::move(scroll));
  }
  body->addChild(std::move(card));
  body->addChild(buildEditor(s));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string("New Tag"), .glyph = std::string("plus"), .variant = ButtonVariant::Default,
                               .onClick = [this] { DeferredCall::callLater([this] { addTag(); }); }}));
  footer->addChild(ui::spacer());
  footer->addChild(ui::button({.text = std::string("Done"), .variant = ButtonVariant::Primary,
                               .onClick = [this] { done(); }}));
  sheet->addChild(std::move(footer));
  return sheet;
}

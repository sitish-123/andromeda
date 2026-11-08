#include "batch_rename_sheet.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files");
  constexpr std::size_t kPreviewRows = 200; // the list shows this many; the summary counts all
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;
  // Each name column of the preview: the dialog minus padding, row padding, gaps, the arrow and the info symbol.
  constexpr float kNameColumn = 168.0F;
  constexpr float kSummaryWidth = 220.0F; // the footer left of Cancel and "Rename 24"; the conflict line wraps
} // namespace

BatchRenameSheet::BatchRenameSheet(std::vector<fs::path> sources, std::vector<BatchRenameItem> items, std::set<std::string> existing,
                                   Rename rename, std::function<void()> close, std::function<void()> rebuild)
    : m_sources(std::move(sources)), m_items(std::move(items)), m_existing(std::move(existing)), m_rename(std::move(rename)),
      m_close(std::move(close)), m_rebuild(std::move(rebuild)) {
  m_rule.find.clear();
}

std::unique_ptr<Node> BatchRenameSheet::build(float scale) {
  m_scale = scale;
  const float s = scale;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({
      .text = std::format("Rename {} Items", m_items.size()),
      .fontSize = Tokens::typeTitle3.size * s,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
  }));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  body->addChild(ui::segmented({
      .options = std::vector<ui::SegmentedOption>{{.label = "Replace"}, {.label = "Add"}, {.label = "Format"},
                                                  {.label = "Case"}, {.label = "Regex"}},
      .selectedIndex = static_cast<std::size_t>(m_rule.mode),
      .scale = s,
      .equalSegmentWidths = true,
      .onChange =
          [this](std::size_t index) {
            m_rule.mode = static_cast<BatchRenameRule::Mode>(index);
            // The fields differ per mode: build the sheet again after this click returns.
            DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
          },
  }));
  body->addChild(buildFields(s));

  auto keep = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  keep->addChild(ui::checkbox({.checked = m_rule.keepExtension, .scale = s, .onChange = [this](bool on) {
                                 m_rule.keepExtension = on;
                                 updatePreview();
                               }}));
  keep->addChild(ui::label({.text = std::string("Keep extensions"), .fontSize = Style::fontSizeBody * s,
                            .color = colorSpecFromRole(ColorRole::Label1)}));
  body->addChild(std::move(keep));

  auto scroll = ui::scrollView({.contentScale = s, .scrollbarAutoHide = true,
                                .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Style::scaledRadius(Tokens::radiusR8, s),
                                .height = Tokens::spaceP48 * 5.0F * s});
  scroll->content()->setGap(0.0F);
  m_list = scroll->content();
  body->addChild(std::move(scroll));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::label({.out = &m_summary, .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(ColorRole::Label2), .width = kSummaryWidth * s, .maxLines = 2}));
  footer->addChild(ui::spacer());
  footer->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  footer->addChild(ui::button({.out = &m_ok, .text = std::string("Rename"), .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLater([this] { submit(); }); }}));
  sheet->addChild(std::move(footer));
  updatePreview();
  return sheet;
}

std::unique_ptr<Node> BatchRenameSheet::buildFields(float s) {
  auto fields = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
  const auto field = [&](const char* label, std::string& value, const char* placeholder) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = kFieldLabel * s}));
    row->addChild(ui::input({.value = value, .placeholder = std::string(placeholder), .fontSize = Style::fontSizeBody * s,
                             .controlHeight = Style::controlHeight * s, .flexGrow = 1.0F,
                             .onChange = [this, &value](const std::string& v) {
                               value = v;
                               updatePreview();
                             },
                             .onSubmit = [this](const std::string&) { submit(); }}));
    fields->addChild(std::move(row));
  };
  const auto choice = [&](std::vector<ui::SegmentedOption> options, std::size_t selected, std::function<void(std::size_t)> set) {
    fields->addChild(ui::segmented({.options = std::move(options), .selectedIndex = selected, .scale = s, .compact = true,
                                    .onChange = [this, set = std::move(set)](std::size_t i) {
                                      set(i);
                                      updatePreview();
                                    }}));
  };
  switch (m_rule.mode) {
  case BatchRenameRule::Mode::Replace: {
    field("Find", m_rule.find, "Text to find");
    field("Replace with", m_rule.replacement, "");
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    row->addChild(ui::checkbox({.checked = m_rule.matchCase, .scale = s, .onChange = [this](bool on) {
                                  m_rule.matchCase = on;
                                  updatePreview();
                                }}));
    row->addChild(ui::label({.text = std::string("Match case"), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label1)}));
    fields->addChild(std::move(row));
    break;
  }
  case BatchRenameRule::Mode::Add:
    field("Text", m_rule.text, "Text to add");
    choice({{.label = "After name"}, {.label = "Before name"}}, m_rule.addBefore ? 1 : 0,
           [this](std::size_t i) { m_rule.addBefore = i == 1; });
    break;
  case BatchRenameRule::Mode::Format: {
    field("Format", m_rule.format, "{name} {n}");
    // Mockup §14 shows the tokens as chips in the field; here a chip adds its token at the end of the format.
    auto chips = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    chips->addChild(ui::row({.width = kFieldLabel * s}));
    for (const auto& [label, token] : {std::pair{"Name", "{name}"}, std::pair{"Counter", "{n}"},
                                       std::pair{"Date Modified", "{date}"}, std::pair{"Date Taken", "{taken}"}}) {
      chips->addChild(ui::button({.text = std::string(label), .fontSize = Style::fontSizeCaption * s,
                                  .variant = ButtonVariant::Tonal, .minHeight = Tokens::spaceP24 * s,
                                  .paddingH = Tokens::spaceP8 * s, .radius = Tokens::spaceP12 * s,
                                  .onClick = [this, token = std::string(token)] {
                                    m_rule.format += (m_rule.format.empty() || m_rule.format.ends_with(' ') ? "" : " ") + token;
                                    updatePreview();
                                    DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
                                  }}));
    }
    fields->addChild(std::move(chips));
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string("Start at"), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = kFieldLabel * s}));
    row->addChild(ui::stepper({.minValue = 0, .maxValue = 999999, .step = 1, .value = m_rule.start, .scale = s,
                               .onValueChanged = [this](int v) {
                                 m_rule.start = v;
                                 updatePreview();
                               }}));
    row->addChild(ui::label({.text = std::string("Digits"), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(ui::stepper({.minValue = 1, .maxValue = 9, .step = 1, .value = m_rule.digits, .scale = s,
                               .onValueChanged = [this](int v) {
                                 m_rule.digits = v;
                                 updatePreview();
                               }}));
    fields->addChild(std::move(row));
    break;
  }
  case BatchRenameRule::Mode::Case:
    choice({{.label = "lowercase"}, {.label = "UPPERCASE"}, {.label = "Title Case"}}, static_cast<std::size_t>(m_rule.caseMode),
           [this](std::size_t i) { m_rule.caseMode = static_cast<BatchRenameRule::CaseMode>(i); });
    break;
  case BatchRenameRule::Mode::Regex:
    field("Pattern", m_rule.find, "Regular expression");
    field("Replace with", m_rule.replacement, "$1 for groups");
    break;
  }
  return fields;
}

void BatchRenameSheet::updatePreview() {
  m_results = applyBatchRename(m_rule, m_items, m_existing);
  std::size_t changed = 0;
  std::size_t problems = 0;
  for (std::size_t i = 0; i < m_results.size(); ++i) {
    changed += m_results[i].name != m_items[i].name ? 1 : 0;
    problems += m_results[i].conflict || !m_results[i].error.empty() ? 1 : 0;
  }
  if (m_list != nullptr) {
    const float s = m_scale;
    while (!m_list->children().empty()) {
      (void)m_list->removeChild(m_list->children().back().get());
    }
    for (std::size_t i = 0; i < std::min(m_results.size(), kPreviewRows); ++i) {
      const BatchRenameResult& r = m_results[i];
      const bool bad = r.conflict || !r.error.empty();
      // Mockup .rn.warn: a conflict row gets a warning tint and the info symbol. Names get fixed halves: grown
      // labels ellipsize at their unstretched width.
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                          .fill = bad ? colorSpecFromRole(ColorRole::Warning, 0.14F) : clearColorSpec(),
                          .radius = Tokens::radiusR8 * s, .height = Tokens::sizeRowCompact * s});
      row->addChild(ui::label({.text = m_items[i].name, .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(ColorRole::Label2), .width = kNameColumn * s, .maxLines = 1,
                               .ellipsize = TextEllipsize::Middle}));
      row->addChild(ui::label({.text = std::string("→"), .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(ColorRole::Label3)}));
      row->addChild(ui::label({.text = bad && !r.error.empty() ? r.error : r.name, .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(!r.error.empty() ? ColorRole::DangerText : ColorRole::Label1),
                               .width = kNameColumn * s, .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
      if (bad) {
        row->addChild(ui::spacer());
        row->addChild(ui::glyph({.glyph = std::string("info-circle"), .glyphSize = Tokens::sizeSymbol * s,
                                 .color = colorSpecFromRole(ColorRole::Warning)}));
      }
      m_list->addChild(std::move(row));
    }
  }
  const std::string summary = problems > 0 ? std::format("{} can’t be renamed", problems)
                                           : std::format("{} of {} change", changed, m_items.size());
  std::size_t clashes = 0;
  for (const auto& r : m_results) {
    clashes += r.conflict ? 1 : 0;
  }
  if (m_summary != nullptr) {
    // Mockup §14: the conflict line says what to do, in the warning colour.
    m_summary->setText(clashes > 0 && clashes == problems
                           ? std::format("{} files would get the same name. Add a counter or change the format.", clashes)
                           : summary);
    m_summary->setColor(colorSpecFromRole(problems > 0 ? ColorRole::Warning : ColorRole::Label2));
  }
  if (m_ok != nullptr) {
    m_ok->setText(changed > 0 ? std::format("Rename {}", changed) : std::string("Rename"));
  }
  std::string first;
  for (std::size_t i = 0; i < std::min<std::size_t>(m_results.size(), 3); ++i) {
    first += std::format("{}{} -> {}", i > 0 ? " | " : "", m_items[i].name, m_results[i].name);
  }
  kLog.debug("batch preview {}: {}", summary, first);
  if (m_ok != nullptr) {
    m_ok->setEnabled(problems == 0 && changed > 0);
  }
}

void BatchRenameSheet::set(const std::string& field, const std::string& value) {
  using Mode = BatchRenameRule::Mode;
  if (field == "mode") {
    m_rule.mode = value == "add"       ? Mode::Add
                  : value == "format"  ? Mode::Format
                  : value == "case"    ? Mode::Case
                  : value == "pattern" ? Mode::Regex
                                       : Mode::Replace;
  } else if (field == "find") {
    m_rule.find = value;
  } else if (field == "replace") {
    m_rule.replacement = value;
  } else if (field == "text") {
    m_rule.text = value;
  } else if (field == "format") {
    m_rule.format = value;
  } else if (field == "start") {
    m_rule.start = std::atoi(value.c_str());
  } else if (field == "digits") {
    m_rule.digits = std::clamp(std::atoi(value.c_str()), 1, 9);
  }
  updatePreview();
  DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
}

void BatchRenameSheet::submit() {
  updatePreview();
  std::vector<std::pair<fs::path, std::string>> pairs;
  for (std::size_t i = 0; i < m_results.size(); ++i) {
    if (m_results[i].conflict || !m_results[i].error.empty()) {
      return;
    }
    if (m_results[i].name != m_items[i].name) {
      pairs.emplace_back(m_sources[i], m_results[i].name);
    }
  }
  if (pairs.empty()) {
    return;
  }
  m_rename(std::move(pairs));
  m_close();
}

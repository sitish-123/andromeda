#include "select_pattern_sheet.h"

#include "util/safe_regex.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <fnmatch.h>
#include <format>

namespace {
  constexpr Logger kLog("files");
  constexpr float kWidth = 360.0F; // mockup §16: 356, on the 4 grid

  std::string trimmed(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      return {};
    }
    return std::string(s.substr(first, s.find_last_not_of(" \t") - first + 1));
  }
} // namespace

SelectPatternSheet::SelectPatternSheet(std::vector<std::string> names, std::string pattern, Mode mode, Apply apply,
                                       std::function<void()> close, std::function<void()> rebuild,
                                       std::function<void(Input&)> focus)
    : m_names(std::move(names)), m_pattern(std::move(pattern)), m_mode(mode), m_apply(std::move(apply)),
      m_close(std::move(close)), m_rebuild(std::move(rebuild)), m_focus(std::move(focus)) {
  m_matches = match(m_names, m_pattern, m_mode, &m_error);
}

std::vector<std::string> SelectPatternSheet::match(const std::vector<std::string>& names, const std::string& pattern,
                                                   Mode mode, std::string* error) {
  std::vector<std::string> out;
  if (error != nullptr) {
    error->clear();
  }
  if (trimmed(pattern).empty()) {
    return out;
  }
  if (mode == Mode::Regex) {
    const auto re = SafeRegex::compile(pattern, true);
    if (!re) {
      if (error != nullptr) {
        *error = "This regular expression is not valid.";
      }
      return out;
    }
    std::ranges::copy_if(names, std::back_inserter(out), [&](const std::string& n) { return re->search(n); });
    if (re->exhausted()) {
      out.clear();
      if (error != nullptr) {
        *error = "This regular expression takes too long. Make it simpler.";
      }
    }
    return out;
  }
  // "*.jpg | *.png": any of the "|" parts, case-insensitive, matched against the whole name (Dolphin).
  std::vector<std::string> parts;
  std::string_view rest = pattern;
  while (true) {
    const auto bar = rest.find('|');
    if (std::string part = trimmed(rest.substr(0, bar)); !part.empty()) {
      parts.push_back(std::move(part));
    }
    if (bar == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(bar + 1);
  }
  std::ranges::copy_if(names, std::back_inserter(out), [&](const std::string& n) {
    return std::ranges::any_of(parts, [&](const std::string& p) { return ::fnmatch(p.c_str(), n.c_str(), FNM_CASEFOLD) == 0; });
  });
  return out;
}

void SelectPatternSheet::update() {
  m_matches = match(m_names, m_pattern, m_mode, &m_error);
  if (m_note != nullptr) {
    m_note->setText(!m_error.empty() ? m_error
                    : trimmed(m_pattern).empty()
                        ? std::string("Type a pattern.")
                        : std::format("Matches {} of {} {}.", m_matches.size(), m_names.size(),
                                      m_names.size() == 1 ? "item" : "items"));
    m_note->setColor(colorSpecFromRole(m_error.empty() ? ColorRole::Label2 : ColorRole::DangerText));
  }
  const bool any = !m_matches.empty();
  if (m_select != nullptr) {
    m_select->setEnabled(any);
  }
  if (m_deselect != nullptr) {
    m_deselect->setEnabled(any);
  }
}

void SelectPatternSheet::setPattern(std::string pattern, bool showInField) {
  m_pattern = std::move(pattern);
  update();
  if (showInField) {
    DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
  }
}

void SelectPatternSheet::setMode(Mode mode) {
  m_mode = mode;
  update();
  DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); }); // the field placeholder follows the mode
}

void SelectPatternSheet::apply(bool select) {
  update();
  if (!m_error.empty()) {
    kLog.debug("pattern refused: {}", m_error);
  }
  if (m_matches.empty()) {
    return;
  }
  m_apply(m_matches, select);
  m_close();
}

std::unique_ptr<Node> SelectPatternSheet::build(float scale) {
  const float s = scale;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = kWidth * s});
  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = std::string("Select by Pattern"), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  const bool focusField = !std::exchange(m_focused, true);
  body->addChild(ui::input({.value = m_pattern,
                            .placeholder = std::string(m_mode == Mode::Wildcard ? "*.jpg | *.png" : "^IMG_\\d+"),
                            .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                            .onChange = [this](const std::string& v) { setPattern(v); },
                            .onSubmit = [this](const std::string&) { apply(true); },
                            .configure = [this, focusField](Input& input) {
                              if (focusField && m_focus) {
                                input.selectAll();
                                m_focus(input);
                              }
                            }}));
  body->addChild(ui::segmented({.options = std::vector<ui::SegmentedOption>{{.label = "Wildcard"}, {.label = "Regex"}},
                                .selectedIndex = static_cast<std::size_t>(m_mode), .scale = s,
                                .equalSegmentWidths = true,
                                .onChange = [this](std::size_t i) { setMode(static_cast<Mode>(i)); }}));
  body->addChild(ui::label({.out = &m_note, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2}));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.out = &m_deselect, .text = std::string("Deselect"), .variant = ButtonVariant::Default,
                               .onClick = [this] { DeferredCall::callLater([this] { apply(false); }); }}));
  footer->addChild(ui::spacer());
  footer->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default,
                               .onClick = [this] { DeferredCall::callLater([close = m_close] { close(); }); }}));
  footer->addChild(ui::button({.out = &m_select, .text = std::string("Select"), .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLater([this] { apply(true); }); }}));
  sheet->addChild(std::move(footer));
  update();
  return sheet;
}

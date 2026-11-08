#include "compress_sheet.h"

#include "core/deferred_call.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <format>

namespace fs = std::filesystem;

namespace {
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;

  struct Format {
    const char* label;
    const char* id;
    const char* extension;
    bool password;
    const char* note;
  };
  constexpr std::array<Format, 3> kFormats{{
      {"ZIP", "zip", ".zip", true, "ZIP opens everywhere. File names stay visible even with a password."},
      {"7z", "7z", ".7z", true, "7z makes the smallest archive. File names stay visible even with a password."},
      {"tar.zst", "tar.zst", ".tar.zst", false, "tar.zst keeps Linux permissions and links. It takes no password."},
  }};

  // "Holiday photos" for one item (a file loses its extension), "Archive" for more.
  std::string defaultName(const std::vector<fs::path>& sources) {
    if (sources.size() != 1) {
      return "Archive";
    }
    const fs::path& one = sources.front();
    std::error_code ec;
    return fs::is_directory(one, ec) ? one.filename().string() : one.stem().string();
  }
} // namespace

CompressSheet::CompressSheet(std::vector<fs::path> sources, Compress compress, std::function<void()> close,
                             std::function<void()> rebuild)
    : m_sources(std::move(sources)), m_compress(std::move(compress)), m_close(std::move(close)),
      m_rebuild(std::move(rebuild)), m_name(defaultName(m_sources)) {}

fs::path CompressSheet::destination() const {
  const fs::path folder = m_sources.empty() ? fs::path() : m_sources.front().parent_path();
  return folder / (m_name + kFormats[m_format].extension);
}

std::unique_ptr<Node> CompressSheet::build(float scale) {
  const float s = scale;
  const Format& format = kFormats[m_format];
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({
      .text = m_sources.size() == 1 ? std::format("Compress “{}”", m_sources.front().filename().string())
                                    : std::format("Compress {} Items", m_sources.size()),
      .fontSize = Tokens::typeTitle3.size * s,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
  }));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  const auto fieldRow = [&](const char* label) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = kFieldLabel * s}));
    return row;
  };

  auto name = fieldRow("Name");
  name->addChild(ui::input({.value = m_name, .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                            .flexGrow = 1.0F,
                            .onChange = [this](const std::string& v) {
                              m_name = v;
                              update();
                            },
                            .onSubmit = [this](const std::string&) { submit(); }}));
  name->addChild(ui::label({.text = std::string(format.extension), .fontSize = Style::fontSizeBody * s,
                            .color = colorSpecFromRole(ColorRole::Label2)}));
  body->addChild(std::move(name));

  auto formats = fieldRow("Format");
  std::vector<ui::SegmentedOption> options;
  for (const Format& f : kFormats) {
    options.push_back({.label = f.label});
  }
  formats->addChild(ui::segmented({.options = std::move(options), .selectedIndex = m_format, .scale = s, .compact = true,
                                   .onChange = [this](std::size_t index) {
                                     m_format = index;
                                     if (!kFormats[index].password) {
                                       m_password.clear();
                                     }
                                     // The password row and the extension follow the format.
                                     DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
                                   }}));
  body->addChild(std::move(formats));

  if (format.password) {
    auto password = fieldRow("Password");
    password->addChild(ui::input({.value = m_password, .placeholder = std::string("Optional"),
                                  .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                                  .passwordMode = true, .flexGrow = 1.0F,
                                  .onChange = [this](const std::string& v) { m_password = v; },
                                  .onSubmit = [this](const std::string&) { submit(); }}));
    body->addChild(std::move(password));
  }
  body->addChild(ui::label({.out = &m_note, .text = std::string(format.note), .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::Label2)}));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  footer->addChild(ui::button({.out = &m_ok, .text = std::string("Compress"), .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLater([this] { submit(); }); }}));
  sheet->addChild(std::move(footer));
  update();
  return sheet;
}

void CompressSheet::update() {
  const bool valid = !m_name.empty() && m_name != "." && m_name != ".." && m_name.find('/') == std::string::npos;
  if (m_ok != nullptr) {
    m_ok->setEnabled(valid && !m_sources.empty());
  }
  if (m_note != nullptr) {
    m_note->setText(valid ? kFormats[m_format].note : "A name can’t be empty or contain “/”.");
  }
}

void CompressSheet::submit() {
  const bool valid = !m_name.empty() && m_name != "." && m_name != ".." && m_name.find('/') == std::string::npos;
  if (!valid || m_sources.empty()) {
    return;
  }
  m_compress(destination(), kFormats[m_format].id, kFormats[m_format].password ? m_password : std::string());
  m_close();
}

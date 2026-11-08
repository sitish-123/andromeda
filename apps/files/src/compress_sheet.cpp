#include "compress_sheet.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include "ui/files/file_format.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files");
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;

  struct Format {
    const char* label;
    const char* id;
    const char* extension;
    bool password;
    const char* note;
    // Guessed packed size per source byte, for text and for other files that are not already compressed.
    double textRatio;
    double otherRatio;
  };
  constexpr std::array<Format, 3> kFormats{{
      {"ZIP", "zip", ".zip", true, "ZIP opens everywhere. File names stay visible even with a password.", 0.35, 0.65},
      {"7z", "7z", ".7z", true, "7z makes the smallest archive. File names stay visible even with a password.", 0.25,
       0.55},
      {"tar.zst", "tar.zst", ".tar.zst", false, "tar.zst keeps Linux permissions and links. It takes no password.",
       0.35, 0.65},
  }};

  // Decimal sizes, as formatFileSize shows them; 4 GB stays under the FAT32 file limit (4 GiB - 1).
  struct Split {
    const char* label;
    const char* id;
    std::uint64_t bytes;
  };
  constexpr std::array<Split, 6> kSplits{{
      {"Don’t Split", "none", 0},
      {"10 MB", "10m", 10'000'000},
      {"25 MB (Email)", "25m", 25'000'000},
      {"100 MB", "100m", 100'000'000},
      {"700 MB (CD)", "700m", 700'000'000},
      {"4 GB (FAT32)", "4g", 4'000'000'000},
  }};

  constexpr std::array<std::string_view, 33> kPackedExtensions = {
      "jpg", "jpeg", "png", "gif", "webp", "heic", "avif", "mp4", "mkv", "mov", "webm",
      "avi", "mp3", "m4a", "aac", "ogg", "opus", "flac", "zip", "7z",  "gz",  "xz",
      "zst", "bz2", "rar", "jar", "apk", "deb", "rpm", "pdf", "docx", "xlsx", "epub"};
  constexpr std::array<std::string_view, 24> kTextExtensions = {
      "txt", "md", "csv", "json", "xml", "html", "css", "js",   "ts",   "c",   "cpp", "h",
      "hpp", "rs", "py",  "sh",   "log", "svg",  "toml", "yaml", "yml", "ini", "conf", "tex"};

  void add(CompressSheet::Totals& totals, const fs::path& file, std::uint64_t size) {
    std::string ext = file.extension().string();
    ext.erase(0, std::min<std::size_t>(ext.size(), 1));
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (std::ranges::find(kPackedExtensions, ext) != kPackedExtensions.end()) {
      totals.packed += size;
    } else if (std::ranges::find(kTextExtensions, ext) != kTextExtensions.end()) {
      totals.text += size;
    } else {
      totals.other += size;
    }
  }

  CompressSheet::Totals measure(const std::vector<fs::path>& sources) {
    CompressSheet::Totals totals;
    std::error_code ec;
    for (const fs::path& source : sources) {
      if (fs::is_regular_file(fs::symlink_status(source, ec))) {
        add(totals, source, fs::file_size(source, ec));
        continue;
      }
      for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
          add(totals, it->path(), it->file_size(ec));
        }
      }
      ec.clear();
    }
    return totals;
  }

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
      m_rebuild(std::move(rebuild)), m_name(defaultName(m_sources)) {
  std::thread([sources = m_sources, alive = std::weak_ptr<bool>(m_alive), this] {
    Totals totals = measure(sources);
    DeferredCall::callLater([alive, totals, this] {
      if (alive.expired()) {
        return;
      }
      m_totals = totals;
      update();
    });
  }).detach();
}

std::string CompressSheet::estimate() const {
  if (!m_totals) {
    return "Estimating…";
  }
  const Format& format = kFormats[m_format];
  const auto bytes = static_cast<std::uint64_t>(static_cast<double>(m_totals->packed) +
                                                static_cast<double>(m_totals->text) * format.textRatio +
                                                static_cast<double>(m_totals->other) * format.otherRatio);
  const std::uint64_t volume = kSplits[m_split].bytes;
  if (volume == 0 || bytes <= volume) {
    return std::format("About {}", formatFileSize(bytes));
  }
  return std::format("About {} in {} parts", formatFileSize(bytes), (bytes + volume - 1) / volume);
}

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
    // A label alone does not keep its width: the cell holds the field column in place (mockup .frow .fl).
    auto cell = ui::row({.align = FlexAlign::Center, .width = kFieldLabel * s});
    cell->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                              .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(std::move(cell));
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
  formats->addChild(ui::segmented({.options = std::move(options), .selectedIndex = m_format, .scale = s,
                                   .equalSegmentWidths = true, .flexGrow = 1.0F,
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

  auto split = fieldRow("Split");
  std::vector<std::string> splits;
  for (const Split& option : kSplits) {
    splits.emplace_back(option.label);
  }
  split->addChild(ui::select({.options = std::move(splits), .selectedIndex = m_split, .fontSize = Style::fontSizeBody * s,
                              .controlHeight = Style::controlHeight * s,
                              .onSelectionChanged = [this](std::size_t index, std::string_view) {
                                m_split = index;
                                update();
                              }}));
  split->addChild(ui::row({.flexGrow = 1.0F}));
  split->addChild(ui::label({.out = &m_estimate, .text = estimate(), .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
  body->addChild(std::move(split));
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
  if (m_estimate != nullptr) {
    m_estimate->setText(estimate());
  }
  kLog.debug("compress estimate {}", estimate());
}

void CompressSheet::set(const std::string& field, const std::string& value) {
  if (field == "name") {
    m_name = value;
  } else if (field == "format") {
    for (std::size_t i = 0; i < kFormats.size(); ++i) {
      if (value == kFormats[i].id) {
        m_format = i;
      }
    }
    if (!kFormats[m_format].password) {
      m_password.clear();
    }
  } else if (field == "password" && kFormats[m_format].password) {
    m_password = value;
  } else if (field == "split") {
    for (std::size_t i = 0; i < kSplits.size(); ++i) {
      if (value == kSplits[i].id) {
        m_split = i;
      }
    }
  }
  update();
  DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
}

void CompressSheet::submit() {
  const bool valid = !m_name.empty() && m_name != "." && m_name != ".." && m_name.find('/') == std::string::npos;
  if (!valid || m_sources.empty()) {
    return;
  }
  m_compress(destination(), kFormats[m_format].id, kFormats[m_format].password ? m_password : std::string(),
             kSplits[m_split].bytes);
  m_close();
}

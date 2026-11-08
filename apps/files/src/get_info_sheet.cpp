#include "get_info_sheet.h"

#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/text_clipboard.h"
#include "files/file_ops_client.h"
#include "permission_access.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <format>
#include <gio/gio.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;
  constexpr std::uint64_t kCountPostEvery = 4096; // entries between live size updates

  std::int64_t ns(const statx_timestamp& t) { return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec; }

  std::string where(const fs::path& parent) {
    std::string text = parent.string();
    const char* home = g_get_home_dir();
    if (home != nullptr && text.starts_with(home)) {
      text = "Home" + text.substr(std::string_view(home).size());
    }
    std::string out;
    for (const fs::path& part : fs::path(text)) {
      if (part == "/") {
        continue;
      }
      out += (out.empty() ? "" : " › ") + part.string();
    }
    return out.empty() ? "/" : out;
  }

  std::string userName(std::uint32_t uid) {
    const passwd* pw = ::getpwuid(uid);
    std::string name = pw != nullptr ? pw->pw_name : std::to_string(uid);
    return uid == ::getuid() ? name + " (you)" : name;
  }

  std::string groupName(std::uint32_t gid) {
    const group* gr = ::getgrgid(gid);
    return gr != nullptr ? gr->gr_name : std::to_string(gid);
  }

  std::string defaultApp(const std::string& contentType) {
    GAppInfo* app = g_app_info_get_default_for_type(contentType.c_str(), FALSE);
    if (app == nullptr) {
      return "None";
    }
    std::string name = g_app_info_get_display_name(app);
    g_object_unref(app);
    return name;
  }

  std::string normalizedHex(std::string text) {
    std::erase_if(text, [](unsigned char c) { return std::isspace(c) != 0; });
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }
} // namespace

GetInfoSheet::GetInfoSheet(std::vector<fs::path> items, FileOpsClient* ops, std::function<void()> close,
                           std::function<void()> rebuild, std::function<void()> invalidate)
    : m_ops(ops), m_close(std::move(close)), m_rebuild(std::move(rebuild)), m_invalidate(std::move(invalidate)) {
  for (fs::path& path : items) {
    m_items.push_back({.path = std::move(path)});
  }
  load();
  startCount();
}

GetInfoSheet::~GetInfoSheet() {
  *m_alive = false;
  m_counter.request_stop();
  if (m_ops != nullptr) {
    for (const Sum* sum : {&m_sha, &m_md5}) {
      if (sum->state == Sum::State::Running && !sum->job.empty()) {
        m_ops->cancel(sum->job);
      }
    }
  }
}

void GetInfoSheet::later(std::function<void()> fn) {
  DeferredCall::callLater([alive = std::weak_ptr<bool>(m_alive), fn = std::move(fn)] {
    if (auto a = alive.lock(); a && *a) {
      fn();
    }
  });
}

void GetInfoSheet::load() {
  for (Item& item : m_items) {
    struct statx st{};
    if (::statx(AT_FDCWD, item.path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &st) != 0) {
      continue;
    }
    item.mode = st.stx_mode & 07777;
    item.uid = st.stx_uid;
    item.gid = st.stx_gid;
    item.isDir = S_ISDIR(st.stx_mode);
    item.isRegular = S_ISREG(st.stx_mode);
    item.size = item.isDir ? 0 : st.stx_size;
    item.createdNs = (st.stx_mask & STATX_BTIME) != 0 ? ns(st.stx_btime) : 0;
    item.modifiedNs = ns(st.stx_mtime);
    item.openedNs = ns(st.stx_atime);
  }
}

void GetInfoSheet::startCount() {
  std::vector<fs::path> dirs;
  for (const Item& item : m_items) {
    if (item.isDir) {
      dirs.push_back(item.path);
    }
  }
  if (dirs.empty()) {
    m_countDone = true;
    return;
  }
  m_counter = std::jthread([this, dirs = std::move(dirs), alive = std::weak_ptr<bool>(m_alive)](std::stop_token stop) {
    const auto post = [&] {
      if (!m_countPosted.exchange(true)) {
        later([this] {
          m_countPosted = false;
          updateSize();
        });
      }
    };
    for (const fs::path& dir : dirs) {
      std::error_code ec;
      for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (stop.stop_requested()) {
          return;
        }
        const auto status = it->symlink_status(ec);
        if (!ec && fs::is_regular_file(status)) {
          m_countBytes += it->file_size(ec);
        }
        if (++m_countItems % kCountPostEvery == 0) {
          post();
        }
      }
    }
    m_countDone = true;
    post();
  });
}

std::string GetInfoSheet::sizeText() const {
  std::uint64_t bytes = m_countBytes;
  for (const Item& item : m_items) {
    bytes += item.size;
  }
  const bool dirs = std::ranges::any_of(m_items, [](const Item& i) { return i.isDir; });
  if (!dirs) {
    return formatFileSize(bytes);
  }
  const std::uint64_t items = m_countItems;
  std::string text = std::format("{} · {} {}", formatFileSize(bytes), items, items == 1 ? "item" : "items");
  return m_countDone ? text : "Counting… " + text;
}

void GetInfoSheet::updateSize() {
  if (m_size != nullptr) {
    m_size->setText(sizeText());
    m_invalidate();
  }
}

std::unique_ptr<Node> GetInfoSheet::build(float scale) {
  const float s = scale;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  std::size_t files = 0;
  std::size_t folders = 0;
  for (const Item& item : m_items) {
    (item.isDir ? folders : files) += 1;
  }
  std::string title = m_items.size() == 1 ? m_items.front().path.filename().string() : std::format("{} Items", m_items.size());
  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = std::move(title), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
                              .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  if (m_items.size() > 1) {
    body->addChild(ui::label({.text = std::format("{} {}, {} {}", files, files == 1 ? "file" : "files", folders,
                                                  folders == 1 ? "folder" : "folders"),
                              .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2)}));
  }
  body->addChild(buildGeneral(s));
  body->addChild(buildPermissions(s));
  if (m_items.size() == 1 && m_items.front().isRegular) {
    body->addChild(buildChecksums(s));
  }
  if (!m_error.empty()) {
    body->addChild(ui::label({.text = m_error, .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(ColorRole::DangerText)}));
  }
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string("Done"), .variant = ButtonVariant::Primary, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  sheet->addChild(std::move(footer));
  return sheet;
}

namespace {
  std::unique_ptr<Flex> section(const char* caption, float s) {
    auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
    box->addChild(ui::label({.text = std::string(caption), .fontSize = Style::fontSizeBody * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
    return box;
  }

  std::unique_ptr<Flex> field(const std::string& label, float s) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = label, .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2),
                             .width = kFieldLabel * s, .maxLines = 1, .ellipsize = TextEllipsize::End}));
    return row;
  }

  std::unique_ptr<Label> value(std::string text, float s, Label** out = nullptr) {
    return ui::label({.out = out, .text = std::move(text), .fontSize = Style::fontSizeBody * s,
                      .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::Middle,
                      .flexGrow = 1.0F});
  }
} // namespace

std::unique_ptr<Node> GetInfoSheet::buildGeneral(float s) {
  auto box = section("General", s);
  const auto shared = [&](auto get, auto text) -> std::string {
    const auto first = get(m_items.front());
    const bool same = std::ranges::all_of(m_items, [&](const Item& i) { return get(i) == first; });
    return same ? text(first) : std::string("Mixed");
  };
  if (m_items.size() == 1) {
    const auto info = statFile(m_items.front().path);
    auto kind = field("Kind", s);
    kind->addChild(value(info ? fileKindLabel(*info) : std::string("Unknown"), s));
    box->addChild(std::move(kind));
    if (info && !info->isDir) {
      auto open = field("Open With", s);
      open->addChild(value(defaultApp(info->contentType), s));
      box->addChild(std::move(open));
    }
  }
  auto size = field("Size", s);
  size->addChild(value(sizeText(), s, &m_size));
  box->addChild(std::move(size));
  auto at = field("Where", s);
  at->addChild(value(shared([](const Item& i) { return i.path.parent_path(); }, where), s));
  box->addChild(std::move(at));
  const auto date = [&](const char* label, std::int64_t Item::*member) {
    auto row = field(label, s);
    row->addChild(value(shared([member](const Item& i) { return i.*member; },
                               [](std::int64_t t) { return t == 0 ? std::string("Unknown") : formatFileDate(t); }),
                        s));
    box->addChild(std::move(row));
  };
  date("Created", &Item::createdNs);
  date("Modified", &Item::modifiedNs);
  date("Opened", &Item::openedNs);
  return box;
}

std::unique_ptr<Node> GetInfoSheet::buildPermissions(float s) {
  auto box = section("Permissions", s);
  std::vector<std::uint32_t> modes;
  for (const Item& item : m_items) {
    modes.push_back(item.mode);
  }
  const Item& first = m_items.front();
  const bool oneOwner = std::ranges::all_of(m_items, [&](const Item& i) { return i.uid == first.uid; });
  const bool oneGroup = std::ranges::all_of(m_items, [&](const Item& i) { return i.gid == first.gid; });
  const std::array<std::string, 3> names{oneOwner ? userName(first.uid) : std::string("Owner"),
                                         oneGroup ? groupName(first.gid) : std::string("Group"), std::string("Everyone")};
  for (int cls = 0; cls < 3; ++cls) {
    const auto common = commonAccess(modes, static_cast<PermClass>(cls));
    std::vector<ui::SegmentedOption> options{{.label = "No access"}, {.label = "Read only"}, {.label = "Read and write"}};
    std::size_t selected = common.has_value() ? static_cast<std::size_t>(*common) : 3;
    if (selected >= 3) {
      options.push_back({.label = common.has_value() ? accessLabel(*common) : "Mixed"});
      selected = 3;
    }
    auto row = field(names[static_cast<std::size_t>(cls)], s);
    row->addChild(ui::segmented({.options = std::move(options), .selectedIndex = selected, .scale = s, .compact = true,
                                 .onChange = [this, cls](std::size_t index) {
                                   if (index < 3) {
                                     setAccess(cls, static_cast<int>(index));
                                   }
                                 }}));
    box->addChild(std::move(row));
  }
  const auto check = [&](const char* label, bool checked, std::function<void(bool)> set) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    row->addChild(ui::checkbox({.checked = checked, .scale = s, .onChange = std::move(set)}));
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label1)}));
    box->addChild(std::move(row));
  };
  if (std::ranges::none_of(m_items, [](const Item& i) { return i.isDir; })) {
    const bool runs = std::ranges::all_of(m_items, [](const Item& i) { return (i.mode & 0111) != 0; });
    check("Allow running as a program", runs, [this](bool on) { setRun(on); });
  } else {
    check("Apply to enclosed items", m_recursive, [this](bool on) { m_recursive = on; });
  }
  if (m_items.size() > 1) {
    box->addChild(ui::label({.text = std::format("A change here applies to all {} items.", m_items.size()),
                             .fontSize = Style::fontSizeCaption * s, .color = colorSpecFromRole(ColorRole::Label2)}));
  }
  return box;
}

void GetInfoSheet::setAccess(int cls, int access) {
  const auto [mode, mask] = accessBits(static_cast<PermClass>(cls), static_cast<Access>(access));
  if (m_ops == nullptr) {
    return;
  }
  std::vector<fs::path> paths;
  for (Item& item : m_items) {
    paths.push_back(item.path);
    item.mode = (item.mode & ~mask) | (mode & mask);
  }
  m_ops->setPermissions(paths, mode, mask, m_recursive, [this](const std::string&, const std::string& error) {
    if (!error.empty()) {
      m_error = error;
      load();
      later(m_rebuild);
    }
  });
  later(m_rebuild);
}

void GetInfoSheet::setRun(bool allow) {
  if (m_ops == nullptr) {
    return;
  }
  // Each file gets x where it reads: one job per distinct mode keeps the rule exact.
  for (Item& item : m_items) {
    const auto [mode, mask] = runBits(item.mode, allow);
    item.mode = (item.mode & ~mask) | (mode & mask);
    m_ops->setPermissions({item.path}, mode, mask, false, [this](const std::string&, const std::string& error) {
      if (!error.empty()) {
        m_error = error;
        load();
        later(m_rebuild);
      }
    });
  }
  later(m_rebuild);
}

std::unique_ptr<Node> GetInfoSheet::buildChecksums(float s) {
  auto box = section("Checksum", s);
  const auto row = [&](const char* label, Sum& sum, const char* algorithm) {
    auto r = field(label, s);
    switch (sum.state) {
    case Sum::State::Idle:
      r->addChild(value(std::string(), s));
      r->addChild(ui::button({.text = std::string("Show"), .variant = ButtonVariant::Default,
                              .onClick = [this, &sum, algorithm] { runChecksum(sum, algorithm); }}));
      break;
    case Sum::State::Running:
      r->addChild(value(std::string("Computing…"), s));
      r->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this, &sum] {
                                if (m_ops != nullptr && !sum.job.empty()) {
                                  m_ops->cancel(sum.job);
                                }
                              }}));
      break;
    case Sum::State::Done:
      r->addChild(value(sum.hex, s));
      r->addChild(ui::button({.text = std::string("Copy"), .variant = ButtonVariant::Default, .onClick = [&sum] {
                                if (TextClipboard* clipboard = Input::textClipboard()) {
                                  clipboard->setClipboardText(sum.hex);
                                }
                              }}));
      break;
    case Sum::State::Failed:
      r->addChild(ui::label({.text = sum.error, .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 1, .flexGrow = 1.0F}));
      r->addChild(ui::button({.text = std::string("Retry"), .variant = ButtonVariant::Default,
                              .onClick = [this, &sum, algorithm] { runChecksum(sum, algorithm); }}));
      break;
    }
    box->addChild(std::move(r));
  };
  row("SHA-256", m_sha, "sha256");
  row("MD5", m_md5, "md5");
  box->addChild(ui::input({.value = m_compare, .placeholder = std::string("Paste a checksum to compare"),
                           .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                           .onChange = [this](const std::string& v) {
                             m_compare = v;
                             updateCompare();
                           }}));
  box->addChild(ui::label({.out = &m_compareResult, .fontSize = Style::fontSizeCaption * s,
                           .color = colorSpecFromRole(ColorRole::Label2)}));
  updateCompare();
  return box;
}

void GetInfoSheet::updateCompare() {
  if (m_compareResult == nullptr) {
    return;
  }
  const std::string want = normalizedHex(m_compare);
  std::string text;
  if (!want.empty()) {
    if (m_sha.state == Sum::State::Done && want == m_sha.hex) {
      text = "Matches the SHA-256 checksum.";
    } else if (m_md5.state == Sum::State::Done && want == m_md5.hex) {
      text = "Matches the MD5 checksum.";
    } else if (m_sha.state == Sum::State::Done || m_md5.state == Sum::State::Done) {
      text = "Does not match.";
    } else {
      text = want.size() == 32 ? "Show the MD5 checksum to compare." : "Show the SHA-256 checksum to compare.";
    }
  }
  m_compareResult->setText(text);
  m_invalidate();
}

void GetInfoSheet::runChecksum(Sum& sum, const char* algorithm) {
  if (m_ops == nullptr) {
    return;
  }
  sum = {.state = Sum::State::Running, .job = {}, .hex = {}, .error = {}};
  const std::weak_ptr<bool> alive = m_alive;
  m_ops->checksum(
      m_items.front().path, algorithm,
      [this, alive, &sum](const std::string& job, const std::string&) {
        if (!alive.expired()) {
          sum.job = job;
        }
      },
      [this, alive, &sum](const std::string& hex, const std::string& error) {
        if (alive.expired()) {
          return;
        }
        sum.state = hex.empty() ? (error == "cancelled" ? Sum::State::Idle : Sum::State::Failed) : Sum::State::Done;
        sum.hex = hex;
        sum.error = error;
        later(m_rebuild);
      });
  later(m_rebuild);
}

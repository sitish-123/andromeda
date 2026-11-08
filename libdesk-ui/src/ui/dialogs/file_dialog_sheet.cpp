#include "ui/dialogs/file_dialog_sheet.h"

#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/input/key_modifiers.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/files/file_browser.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <glib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  const float& kSidebar = Tokens::componentSidebarWidth;
  const float& kHeader = Tokens::sizeControlXl; // rulebook 9.12 app header 48
  const float& kFooter = Tokens::spaceP64;

  fs::path startDirectory(const fs::path& preferred) {
    std::error_code ec;
    fs::path p = preferred;
    if (!p.empty() && !fs::is_directory(p, ec)) {
      p = p.parent_path();
    }
    if (!p.empty() && fs::is_directory(p, ec)) {
      return p;
    }
    return g_get_home_dir();
  }

  std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }
} // namespace

DirectoryWatcher& FileDialogSheet::watcher() {
  static DirectoryWatcher watcher;
  return watcher;
}

FileDialogSheet::FileDialogSheet(ThumbnailService* thumbnails) : m_thumbnails(thumbnails) {}

FileDialogSheet::~FileDialogSheet() { *m_alive = false; }

std::unique_ptr<Node> FileDialogSheet::releaseRoot() {
  m_rootPtr = m_root.get();
  return std::move(m_root);
}

void FileDialogSheet::create() {
  m_options = FileDialog::currentOptions();
  const float s = m_scale;
  FileBrowser::Host host{
      .invalidate =
          [this] {
            if (m_host != nullptr) {
              m_host->requestLayout();
            }
          },
      .rebuild = {},
      .openFile = [this](const fs::path& path) { openFile(path); },
      .changed = [this] { updateControls(); },
      .modifiers = {},
      .menu = {},
  };
  FileBrowser::Options options{
      .scale = s,
      .view = m_options.defaultViewMode == FileDialogViewMode::Grid ? FileBrowser::View::Icons : FileBrowser::View::List,
      .showHidden = m_options.showHiddenFiles,
      .extraPlaces = {},
      .enabled = {},
      .thumbnails = m_thumbnails,
  };
  if (m_options.mode == FileDialogMode::SelectFolder) {
    options.enabled = [](const FileInfo& info) { return info.isDir; };
  } else if (!m_options.extensions.empty()) {
    std::vector<std::string> exts;
    for (const std::string& e : m_options.extensions) {
      exts.push_back(lower(e.starts_with('.') ? e : "." + e));
    }
    options.enabled = [exts](const FileInfo& info) {
      const std::string ext = lower(fs::path(info.displayName).extension().string());
      return std::ranges::contains(exts, ext);
    };
  }
  m_browser = std::make_unique<FileBrowser>(watcher(), *m_animations, std::move(host), std::move(options));

  auto root = ui::row({.align = FlexAlign::Stretch});
  auto sidebar = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::SolidThin),
                             .clipChildren = true, .width = kSidebar * s});
  sidebar->addChild(m_browser->buildSidebar(kSidebar * s));
  root->addChild(std::move(sidebar));
  root->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));

  auto main = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  auto header = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingH = Tokens::spaceP8 * s, .height = kHeader * s});
  std::size_t slot = 0;
  for (const auto& [glyph, back] : {std::pair{"chevron-left", true}, std::pair{"chevron-right", false}}) {
    header->addChild(ui::button({
        .out = &m_nav[slot++],
        .glyph = std::string(glyph),
        .glyphSize = Tokens::sizeSymbol * s,
        .variant = ButtonVariant::Ghost,
        .radius = Style::scaledRadiusSm(s),
        .width = Tokens::sizeControlM * s,
        .height = Tokens::sizeControlM * s,
        .onClick = [this, back] { back ? m_browser->back() : m_browser->forward(); },
    }));
  }
  header->addChild(ui::label({
      .out = &m_title,
      .fontSize = Tokens::typeTitle2.size * s,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
      .flexGrow = 1.0F,
  }));
  header->addChild(m_browser->buildHeaderAccessory());
  main->addChild(std::move(header));
  main->addChild(m_browser->buildContent());

  auto footer = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                         .height = kFooter * s});
  if (m_options.mode == FileDialogMode::Save) {
    footer->addChild(ui::label({.text = std::string("Save As"), .fontSize = Style::fontSizeBody * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
    footer->addChild(ui::input({
        .out = &m_name,
        .value = m_options.defaultFilename,
        .placeholder = i18n::tr("ui.dialogs.file.filename-placeholder"),
        .fontSize = Style::fontSizeBody * s,
        .controlHeight = Style::controlHeight * s,
        .width = Tokens::componentFilesNameField * s,
        .onChange = [this](const std::string&) { updateControls(); },
        .onSubmit = [this](const std::string&) { submit(); },
    }));
  }
  footer->addChild(ui::spacer());
  footer->addChild(ui::button({
      .text = i18n::tr("common.actions.cancel"),
      .variant = ButtonVariant::Secondary,
      .minWidth = 92.0F * s,
      .onClick =
          [this] {
            DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
              if (!alive.expired() && m_host != nullptr) {
                m_host->cancel();
              }
            });
          },
  }));
  const std::string okText = m_options.mode == FileDialogMode::Save           ? "Save"
      : m_options.mode == FileDialogMode::SelectFolder                         ? "Choose"
                                                                               : "Open";
  footer->addChild(ui::button({
      .out = &m_ok,
      .text = okText,
      .variant = ButtonVariant::Primary,
      .minWidth = 92.0F * s,
      .onClick =
          [this] {
            DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
              if (!alive.expired()) {
                submit();
              }
            });
          },
  }));
  main->addChild(ui::separator({}));
  main->addChild(std::move(footer));
  root->addChild(std::move(main));

  m_root = std::move(root);
  if (m_animations != nullptr) {
    m_root->setAnimationManager(m_animations);
  }
}

void FileDialogSheet::onOpen(std::string_view /*context*/) {
  m_browser->jump(startDirectory(m_options.startDirectory));
  updateControls();
}

void FileDialogSheet::onClose() {
  if (m_browser != nullptr && !m_browser->location().empty()) {
    FileDialog::setLastBrowsedDirectory(m_browser->location());
  }
  m_title = nullptr;
  m_name = nullptr;
  m_ok = nullptr;
  m_nav = {};
  m_rootPtr = nullptr;
}

void FileDialogSheet::layout(Renderer& renderer, float width, float height) {
  Node* r = root();
  if (r == nullptr) {
    return;
  }
  r->setSize(width, height);
  r->layout(renderer);
}

void FileDialogSheet::update(Renderer& /*renderer*/) {}

InputArea* FileDialogSheet::initialFocusArea() const {
  if (m_name != nullptr) {
    return m_name->inputArea();
  }
  return m_browser != nullptr ? m_browser->focusArea() : nullptr;
}

bool FileDialogSheet::nameFocused() const {
  return m_name != nullptr && m_host != nullptr && m_host->focusedArea() == m_name->inputArea();
}

bool FileDialogSheet::handleKey(const KeyboardEvent& event) {
  if (!event.pressed || event.preedit || m_browser == nullptr) {
    return false;
  }
  if (event.sym == XKB_KEY_Escape && m_browser->selectionCount() == 0) {
    if (m_host != nullptr) {
      m_host->cancel();
    }
    return true;
  }
  if (nameFocused()) {
    return false; // the name field takes its keys; Enter submits through onSubmit
  }
  if ((event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) && m_browser->selectionCount() == 0) {
    submit();
    return true;
  }
  return m_browser->onKey(event);
}

void FileDialogSheet::openFile(const fs::path& path) {
  if (m_options.mode == FileDialogMode::Save) {
    if (m_name != nullptr) {
      m_name->setValue(path.filename().string());
    }
    return;
  }
  if (m_options.mode == FileDialogMode::Open && m_host != nullptr) {
    FileDialog::setLastBrowsedDirectory(m_browser->location());
    m_host->accept(path);
  }
}

void FileDialogSheet::submit() {
  if (m_host == nullptr || m_browser == nullptr) {
    return;
  }
  const auto selected = m_browser->selectedPaths();
  std::error_code ec;
  switch (m_options.mode) {
  case FileDialogMode::Open:
    if (selected.size() == 1) {
      if (fs::is_directory(selected.front(), ec)) {
        m_browser->go(selected.front());
        return;
      }
      FileDialog::setLastBrowsedDirectory(m_browser->location());
      m_host->accept(selected.front());
    }
    return;
  case FileDialogMode::Save: {
    const std::string name = m_name != nullptr ? m_name->value() : std::string();
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") {
      return;
    }
    FileDialog::setLastBrowsedDirectory(m_browser->location());
    m_host->accept(m_browser->location() / name);
    return;
  }
  case FileDialogMode::SelectFolder:
    FileDialog::setLastBrowsedDirectory(m_browser->location());
    m_host->accept(selected.size() == 1 && fs::is_directory(selected.front(), ec) ? selected.front()
                                                                                    : m_browser->location());
    return;
  }
}

void FileDialogSheet::updateControls() {
  if (m_browser == nullptr) {
    return;
  }
  if (m_title != nullptr) {
    m_title->setText(m_options.title.empty() ? m_browser->title() : m_options.title);
  }
  if (m_nav[0] != nullptr) {
    m_nav[0]->setEnabled(m_browser->canBack());
    m_nav[1]->setEnabled(m_browser->canForward());
  }
  if (m_ok != nullptr) {
    std::error_code ec;
    const auto selected = m_browser->selectedPaths();
    bool ok = true;
    if (m_options.mode == FileDialogMode::Open) {
      ok = selected.size() == 1;
    } else if (m_options.mode == FileDialogMode::Save) {
      ok = m_name != nullptr && !m_name->value().empty();
    }
    m_ok->setEnabled(ok);
  }
  if (m_options.mode == FileDialogMode::Save && m_name != nullptr) {
    const auto rows = m_browser->selectedRows();
    if (rows.size() == 1 && !rows.front()->isDir) {
      m_name->setValue(rows.front()->displayName);
    }
  }
  if (m_host != nullptr) {
    m_host->requestLayout();
    m_host->requestRedraw();
  }
}

#include "app/app_window.h"
#include "core/log.h"
#include "files_app.h"
#include "files_window.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <glib.h>

namespace fs = std::filesystem;

namespace {
  bool isDir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
  }
} // namespace

namespace {
  constexpr Logger kLog("picker");
  constexpr float kFooterHeight = 52.0F;

  std::string uriFor(const fs::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = uri != nullptr ? uri : "";
    g_free(uri);
    return out;
  }
} // namespace

void FilesWindow::setPicker(std::uint64_t id, PickerPortal::Request request) {
  Picker picker{.id = id, .request = std::move(request), .name = {}, .filter = {}, .confirmReplace = {}};
  picker.name = picker.request.currentName;
  if (!picker.request.filters.empty()) {
    picker.filter = picker.request.currentFilter.value_or(0);
  }
  m_picker = std::move(picker);
  applyPickerFilter();
  rebuild();
}

std::optional<std::uint64_t> FilesWindow::pickerId() const noexcept {
  return m_picker.has_value() ? std::optional{m_picker->id} : std::nullopt;
}

void FilesWindow::applyPickerFilter() {
  if (m_view == nullptr || !m_picker.has_value()) {
    return;
  }
  const bool directory = m_picker->request.directory;
  if (directory || !m_picker->filter.has_value() || *m_picker->filter >= m_picker->request.filters.size()) {
    m_view->setPredicate(directory ? [](const FileInfo& info) { return info.isDir; } : std::function<bool(const FileInfo&)>{});
    return;
  }
  m_view->setPredicate([filter = m_picker->request.filters[*m_picker->filter]](const FileInfo& info) {
    return info.isDir || PickerPortal::matches(filter, info.name, info.contentType);
  });
}

void FilesWindow::setPickerName(std::string name) {
  if (m_picker.has_value()) {
    m_picker->name = std::move(name);
    rebuild();
  }
}

void FilesWindow::setPickerFilter(std::size_t index) {
  if (m_picker.has_value() && index < m_picker->request.filters.size()) {
    m_picker->filter = index;
    applyPickerFilter();
  }
}

void FilesWindow::pickerCancel() { pickerAnswer({.response = 1, .uris = {}, .filter = {}}); }

void FilesWindow::pickerAccept() {
  if (!m_picker.has_value() || m_inTrash) {
    return;
  }
  const auto& request = m_picker->request;
  const auto selected = selectedPaths();
  PickerPortal::Answer answer{.response = 0, .uris = {}, .filter = m_picker->filter};
  using Kind = PickerPortal::Request::Kind;
  if (request.kind == Kind::Open && !request.directory) {
    if (selected.size() == 1 && isDir(selected.front())) {
      go(selected.front());
      return;
    }
    for (const fs::path& path : selected) {
      if (!isDir(path)) {
        answer.uris.push_back(uriFor(path));
      }
    }
    if (answer.uris.empty()) {
      return;
    }
    if (!request.multiple) {
      answer.uris.resize(1);
    }
  } else if (request.kind == Kind::Open) {
    for (const fs::path& path : selected) {
      answer.uris.push_back(uriFor(path));
    }
    if (answer.uris.empty()) {
      answer.uris.push_back(uriFor(m_location));
    }
    if (!request.multiple) {
      answer.uris.resize(1);
    }
  } else if (request.kind == Kind::Save) {
    const std::string& name = m_picker->name;
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") {
      showToast("Enter a file name", false);
      return;
    }
    const fs::path target = m_location / name;
    if (isDir(target)) {
      go(target);
      return;
    }
    std::error_code ec;
    if (fs::exists(target, ec) && m_picker->confirmReplace != name) {
      m_picker->confirmReplace = name;
      showToast(std::format("“{}” already exists. Save again to replace it.", name), false);
      return;
    }
    answer.uris.push_back(uriFor(target));
  } else {
    const fs::path folder = selected.size() == 1 && isDir(selected.front()) ? selected.front() : m_location;
    for (const fs::path& file : request.files) {
      answer.uris.push_back(uriFor(folder / file.filename()));
    }
  }
  pickerAnswer(std::move(answer));
}

void FilesWindow::pickerAnswer(PickerPortal::Answer answer) {
  if (!m_picker.has_value()) {
    return;
  }
  const std::uint64_t id = m_picker->id;
  m_picker.reset();
  kLog.debug("answer {} with {} uris", answer.response, answer.uris.size());
  m_app.pickerAnswered(id, answer);
  m_app.closeWindow(m_window);
}

std::unique_ptr<Node> FilesWindow::buildPickerFooter(float s) {
  const auto& request = m_picker->request;
  using Kind = PickerPortal::Request::Kind;
  auto footer = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                         .height = kFooterHeight * s});
  if (request.kind == Kind::Save) {
    footer->addChild(ui::input({
        .value = m_picker->name,
        .placeholder = std::string("Name"),
        .fontSize = Style::fontSizeBody * s,
        .width = 240.0F * s,
        .onChange = [this](const std::string& text) {
          if (m_picker.has_value()) {
            m_picker->name = text;
          }
        },
        .onSubmit = [this](const std::string&) { pickerAccept(); },
    }));
  }
  if (!request.directory && request.filters.size() > 1) {
    std::vector<std::string> names;
    for (const auto& filter : request.filters) {
      names.push_back(filter.name);
    }
    footer->addChild(ui::select({
        .options = std::move(names),
        .selectedIndex = m_picker->filter,
        .fontSize = Style::fontSizeBody * s,
        .onSelectionChanged = [this](std::size_t index, std::string_view) { setPickerFilter(index); },
    }));
  }
  footer->addChild(ui::label({
      .text = m_toast,
      .fontSize = Style::fontSizeCaption * s,
      .color = colorSpecFromRole(ColorRole::Label2),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
      .flexGrow = 1.0F,
  }));
  footer->addChild(ui::button({.text = std::string("Cancel"), .onClick = [this] { pickerCancel(); }}));
  const std::string accept = !request.acceptLabel.empty() ? request.acceptLabel
      : request.kind == Kind::Open                         ? (request.directory ? "Select" : "Open")
                                                           : "Save";
  footer->addChild(ui::button({.text = accept, .variant = ButtonVariant::Primary, .onClick = [this] { pickerAccept(); }}));
  return footer;
}

#include "app/app_window.h"
#include "core/log.h"
#include "files_app.h"
#include "files_window.h"
#include "tags.h"
#include "ui/builders.h"
#include "ui/files/file_browser.h"
#include "render/core/color.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "design_tokens.h"

#include <format>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <glib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {
  bool isDir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
  }
} // namespace

namespace {
  constexpr Logger kLog("picker");

  // The last folder each app saved to or opened from (mockup §6: "the last folder is remembered per app").
  fs::path pickerStatePath() { return fs::path(g_get_user_state_dir()) / "andromeda" / "files" / "picker.json"; }

  nlohmann::json loadPickerState() {
    std::ifstream in(pickerStatePath());
    nlohmann::json state = nlohmann::json::parse(in, nullptr, false);
    return state.is_object() ? state : nlohmann::json::object();
  }

  // Per app: {"last": folder, "recent": [newest first, at most kRecentFolders]}.
  constexpr std::size_t kRecentFolders = 5;

  std::vector<fs::path> recentFoldersFor(const std::string& appId) {
    std::vector<fs::path> out;
    if (appId.empty()) {
      return out;
    }
    const nlohmann::json state = loadPickerState();
    const auto it = state.find(appId);
    if (it == state.end()) {
      return out;
    }
    if (it->is_string()) {
      out.emplace_back(it->get<std::string>());
    } else if (it->is_object() && it->contains("recent") && (*it)["recent"].is_array()) {
      for (const auto& folder : (*it)["recent"]) {
        if (folder.is_string()) {
          out.emplace_back(folder.get<std::string>());
        }
      }
    }
    return out;
  }

  fs::path lastFolderFor(const std::string& appId) {
    const auto recent = recentFoldersFor(appId);
    return recent.empty() ? fs::path() : recent.front();
  }


  // Why a save name cannot be used, or "" when it can (mockup §25 inline errors).
  std::string nameError(const std::string& name) {
    if (name.empty()) {
      return "Enter a name.";
    }
    if (name.find('/') != std::string::npos) {
      return "A name cannot contain “/”.";
    }
    if (name == "." || name == "..") {
      return "This name is reserved.";
    }
    if (name.size() > 255) {
      return "The name is too long.";
    }
    return {};
  }

  // "Choose a Folder for “Steam”" (mockup §13): the asking app's own name from its desktop file.
  std::string appName(const std::string& appId) {
    if (appId.empty()) {
      return {};
    }
    GDesktopAppInfo* info = g_desktop_app_info_new((appId + ".desktop").c_str());
    if (info == nullptr) {
      return {};
    }
    std::string name = g_app_info_get_display_name(G_APP_INFO(info));
    g_object_unref(info);
    return name;
  }

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
  picker.choices = picker.request.choices;
  if (!picker.request.filters.empty()) {
    picker.filter = picker.request.currentFilter.value_or(0);
  }
  using Kind = PickerPortal::Request::Kind;
  if (picker.request.title.empty() && picker.request.directory) {
    const std::string app = appName(picker.request.appId);
    m_window.setTitle(app.empty() ? std::string("Choose a Folder") : std::format("Choose a Folder for “{}”", app));
  }
  const bool openFiles = picker.request.kind == Kind::Open && !picker.request.directory;
  const bool callerFolder = !picker.request.currentFolder.empty();
  m_picker = std::move(picker);
  m_browser->setExtraPlaces({}); // a picker returns files, never Trash items
  applyPickerFilter();
  // Start: the caller's folder, else where this app was last, else Recents for Open (mockup §6), else home.
  std::error_code ec;
  if (!callerFolder) {
    if (const fs::path last = lastFolderFor(m_picker->request.appId); !last.empty() && fs::is_directory(last, ec)) {
      go(last);
    } else if (openFiles) {
      goRecents();
    }
  }
  rebuild();
}

void FilesWindow::rememberPickerFolder() const {
  if (!m_picker.has_value() || m_picker->request.appId.empty() || location().empty()) {
    return;
  }
  nlohmann::json state = loadPickerState();
  std::vector<fs::path> recent = recentFoldersFor(m_picker->request.appId);
  std::erase(recent, location());
  recent.insert(recent.begin(), location());
  recent.resize(std::min(recent.size(), kRecentFolders));
  nlohmann::json list = nlohmann::json::array();
  for (const fs::path& folder : recent) {
    list.push_back(folder.string());
  }
  state[m_picker->request.appId] = {{"last", location().string()}, {"recent", list}};
  std::error_code ec;
  fs::create_directories(pickerStatePath().parent_path(), ec);
  const fs::path tmp = pickerStatePath().string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << state.dump(2);
  }
  fs::rename(tmp, pickerStatePath(), ec);
}

std::string FilesWindow::pickerPrimaryLabel() const {
  const auto& request = m_picker->request;
  using Kind = PickerPortal::Request::Kind;
  if (!request.acceptLabel.empty()) {
    return request.acceptLabel;
  }
  if (request.kind != Kind::Open) {
    return "Save";
  }
  if (request.directory) {
    return "Select";
  }
  std::size_t files = 0;
  for (const fs::path& path : selectedPaths()) {
    files += isDir(path) ? 0 : 1;
  }
  return files > 1 ? std::format("Open {} Items", files) : std::string("Open");
}

std::optional<std::uint64_t> FilesWindow::pickerId() const noexcept {
  return m_picker.has_value() ? std::optional{m_picker->id} : std::nullopt;
}

void FilesWindow::applyPickerFilter() {
  if (!m_picker.has_value()) {
    return;
  }
  const bool directory = m_picker->request.directory;
  if (directory || !m_picker->filter.has_value() || *m_picker->filter >= m_picker->request.filters.size()) {
    m_browser->setEnabledPredicate(directory ? [](const FileInfo& info) { return info.isDir; } : std::function<bool(const FileInfo&)>{});
    return;
  }
  m_browser->setEnabledPredicate([filter = m_picker->request.filters[*m_picker->filter]](const FileInfo& info) {
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
  if (!m_picker.has_value() || inTrash()) {
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
      answer.uris.push_back(uriFor(location()));
    }
    if (!request.multiple) {
      answer.uris.resize(1);
    }
  } else if (request.kind == Kind::Save) {
    const std::string& name = m_picker->name;
    if (!nameError(name).empty()) {
      rebuild(); // the error shows under the field
      return;
    }
    const fs::path target = location() / name;
    if (isDir(target)) {
      go(target);
      return;
    }
    std::error_code ec;
    if (fs::exists(target, ec) && m_picker->confirmReplace != name) {
      m_picker->confirmReplace = name; // the footer asks Replace / Cancel in place (mockup §13)
      rebuild();
      return;
    }
    answer.uris.push_back(uriFor(target));
  } else {
    const fs::path folder = selected.size() == 1 && isDir(selected.front()) ? selected.front() : location();
    for (const fs::path& file : request.files) {
      answer.uris.push_back(uriFor(folder / file.filename()));
    }
  }
  answer.choices = m_picker->choices;
  if (request.kind == Kind::Save && !m_picker->tags.empty()) {
    m_app.tagWhenWritten(location() / m_picker->name, m_picker->tags);
  }
  rememberPickerFolder();
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
  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP12 * s,
                            .paddingH = Tokens::spaceP12 * s});
  // Save: the name row with its inline error (mockup §25), or the in-place replace question.
  if (request.kind == Kind::Save) {
    const std::string error = m_picker->name == request.currentName && m_picker->name.empty() ? std::string()
                                                                                             : nameError(m_picker->name);
    const bool replacing = !m_picker->confirmReplace.empty() && m_picker->confirmReplace == m_picker->name;
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string("Save As"), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = Tokens::componentFilesFieldLabel * s}));
    const std::size_t dot = m_picker->name.rfind('.');
    const std::size_t stem = dot == std::string::npos || dot == 0 ? m_picker->name.size() : dot;
    const bool focusName = !m_picker->nameFocused;
    m_picker->nameFocused = true;
    row->addChild(ui::input({
        .value = m_picker->name,
        .placeholder = std::string("Name"),
        .fontSize = Style::fontSizeBody * s,
        .invalid = !error.empty(),
        .flexGrow = 1.0F,
        .onChange =
            [this](const std::string& text) {
              if (m_picker.has_value()) {
                const bool hadError = !nameError(m_picker->name).empty();
                m_picker->name = text;
                if (hadError != !nameError(text).empty() || !m_picker->confirmReplace.empty()) {
                  m_picker->confirmReplace.clear();
                  rebuild();
                }
              }
            },
        .onSubmit = [this](const std::string&) { pickerAccept(); },
        .configure =
            [this, focusName, stem](Input& input) {
              if (focusName) {
                input.selectRange(0, stem); // the name without its extension, ready to type over
                m_window.input().setFocus(input.inputArea());
              }
            },
    }));
    column->addChild(std::move(row));
    if (!error.empty() || replacing) {
      auto note = ui::row({});
      note->addChild(ui::node({.width = Tokens::componentFilesFieldLabel * s + Tokens::spaceP12 * s}));
      note->addChild(ui::label({
          .text = replacing ? std::format("“{}” already exists. Replace it?", m_picker->name) : error,
          .fontSize = Style::fontSizeCaption * s,
          .color = colorSpecFromRole(replacing ? ColorRole::Label2 : ColorRole::DangerText),
          .maxLines = 2,
      }));
      column->addChild(std::move(note));
    }
  }
  // Portal choices: a pop per option list, a check box per boolean (mockup §25 "row of pops above the buttons").
  if (!m_picker->choices.empty()) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    for (std::size_t i = 0; i < m_picker->choices.size(); ++i) {
      const PickerPortal::Choice& choice = m_picker->choices[i];
      if (choice.options.empty()) {
        row->addChild(ui::checkbox({.checked = choice.value == "true", .onChange = [this, i](bool on) {
                                      if (m_picker.has_value() && i < m_picker->choices.size()) {
                                        m_picker->choices[i].value = on ? "true" : "false";
                                      }
                                    }}));
        row->addChild(ui::label({.text = choice.label, .fontSize = Style::fontSizeBody * s,
                                 .color = colorSpecFromRole(ColorRole::Label1)}));
        continue;
      }
      row->addChild(ui::label({.text = choice.label, .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
      std::vector<std::string> names;
      std::size_t selected = 0;
      for (std::size_t k = 0; k < choice.options.size(); ++k) {
        names.push_back(choice.options[k].second);
        selected = choice.options[k].first == choice.value ? k : selected;
      }
      row->addChild(ui::select({
          .options = std::move(names),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * s,
          .onSelectionChanged =
              [this, i](std::size_t index, std::string_view) {
                if (m_picker.has_value() && i < m_picker->choices.size() && index < m_picker->choices[i].options.size()) {
                  m_picker->choices[i].value = m_picker->choices[i].options[index].first;
                }
              },
      }));
    }
    column->addChild(std::move(row));
  }
  // Buttons: filter pop left, Cancel, the accent primary (mockup §6 footer).
  auto footer = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
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
  footer->addChild(ui::node({.flexGrow = 1.0F}));
  footer->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Style::fontSizeBody * s,
                               .variant = ButtonVariant::Default, .onClick = [this] { pickerCancel(); }}));
  const bool replacing = request.kind == Kind::Save && !m_picker->confirmReplace.empty() &&
                         m_picker->confirmReplace == m_picker->name;
  const bool blocked = request.kind == Kind::Save && !nameError(m_picker->name).empty();
  footer->addChild(ui::button({
      .text = replacing ? std::string("Replace") : pickerPrimaryLabel(),
      .fontSize = Style::fontSizeBody * s,
      .enabled = !blocked,
      .variant = replacing ? ButtonVariant::Destructive : ButtonVariant::Primary,
      .onClick = [this] { pickerAccept(); },
      .configure = [this](Button& button) { m_pickerPrimary = &button; },
  }));
  column->addChild(std::move(footer));
  return column;
}

void FilesWindow::expandPicker() {
  if (!m_picker.has_value() || m_picker->expanded) {
    return;
  }
  m_picker->expanded = true;
  m_picker->nameFocused = false;
  // The expanded size is the Open picker's: the compact Save sheet keeps its own small size for next time.
  m_window.setStateKey("picker");
  m_window.requestSize(kPickerWidth, kPickerHeight);
  m_window.setSidebarVisible(true);
  rebuild();
}

std::unique_ptr<Node> FilesWindow::buildSaveCompact(float s) {
  // Mockup §6 "Save Image": label column 48 (field_label), rows gap 12, buttons right; the chevron grows the browser.
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                         .flexGrow = 1.0F});
  const auto labelled = [&](const char* text) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string(text), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = Tokens::componentFilesFieldLabel * s}));
    return row;
  };
  // Name row and its error / replace note come from the shared footer builder; only its buttons are dropped here.
  auto footer = buildPickerFooter(s);
  // Tags: one dot per sidebar tag; a click toggles it (applied after the app writes the file).
  {
    auto row = labelled("Tags");
    for (const TagDef& tag : loadTagConfig()) {
      if (!tag.sidebar) {
        continue;
      }
      const bool on = std::ranges::find(m_picker->tags, tag.name) != m_picker->tags.end();
      const float d = Tokens::spaceP12 * s;
      row->addChild(ui::button({
          .fontSize = Style::fontSizeCaption * s,
          .variant = ButtonVariant::Ghost,
          .tooltip = tag.name,
          .width = d + Tokens::spaceP8 * s,
          .height = d + Tokens::spaceP8 * s,
          .onClick =
              [this, name = tag.name] {
                if (!m_picker.has_value()) {
                  return;
                }
                if (std::erase(m_picker->tags, name) == 0) {
                  m_picker->tags.push_back(name);
                }
                rebuild();
              },
          .configure =
              [d, on, rgb = tag.rgb](Button& button) {
                auto dot = ui::box({});
                dot->setSize(d, d);
                dot->setRadius(d / 2.0F);
                dot->setFill(rgbHex(rgb));
                if (on) {
                  dot->setBorder(colorSpecFromRole(ColorRole::Label1), d / 6.0F);
                }
                button.addChild(std::move(dot));
              },
      }));
    }
    footer->insertChildAt(footer->children().size() - 1, std::move(row)); // above the buttons
  }
  // Where: this app's recent folders, then the places; the chevron expands the full browser.
  {
    auto row = labelled("Where");
    std::vector<fs::path> folders = recentFoldersFor(m_picker->request.appId);
    for (const GUserDirectory dir : {G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOWNLOAD,
                                     G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_VIDEOS}) {
      if (const char* p = g_get_user_special_dir(dir); p != nullptr && std::ranges::find(folders, fs::path(p)) == folders.end()) {
        folders.emplace_back(p);
      }
    }
    if (std::ranges::find(folders, location()) == folders.end()) {
      folders.insert(folders.begin(), location());
    }
    std::vector<std::string> names;
    std::size_t current = 0;
    for (std::size_t i = 0; i < folders.size(); ++i) {
      names.push_back(folders[i] == fs::path(g_get_home_dir()) ? std::string("Home") : folders[i].filename().string());
      current = folders[i] == location() ? i : current;
    }
    row->addChild(ui::select({
        .options = std::move(names),
        .selectedIndex = current,
        .fontSize = Style::fontSizeBody * s,
        .flexGrow = 1.0F,
        .onSelectionChanged =
            [this, folders](std::size_t index, std::string_view) {
              if (index < folders.size()) {
                go(folders[index]);
                rebuild();
              }
            },
    }));
    row->addChild(ui::button({.glyph = std::string("chevron-down"), .variant = ButtonVariant::Default,
                              .tooltip = std::string("Show all folders"), .onClick = [this] { expandPicker(); }}));
    footer->insertChildAt(footer->children().size() - 1, std::move(row));
  }
  box->addChild(std::move(footer));
  return box;
}

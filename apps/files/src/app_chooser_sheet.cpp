#include "app_chooser_sheet.h"

#include "design_tokens.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "render/scene/input_area.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/files/type_ahead.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

namespace fs = std::filesystem;

namespace {
  // An app icon from a file path: it loads at the first layout, when a renderer is there (like PlaceIcon).
  class AppIconNode final : public Node {
  public:
    AppIconNode(std::string path, float size) : m_path(std::move(path)), m_size(size) {
      m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
      m_image->setSize(size, size);
      setSize(size, size);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      const int px = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
      if (px != m_loadedPx && !m_path.empty()) {
        m_image->setSourceFile(renderer, m_path, px);
        m_loadedPx = px;
      }
      m_image->setSize(m_size, m_size);
      m_image->layout(renderer);
    }

  private:
    std::string m_path;
    float m_size;
    Image* m_image = nullptr;
    int m_loadedPx = 0;
  };

  std::string withoutDesktop(std::string id) {
    if (id.ends_with(".desktop")) {
      id.resize(id.size() - std::string_view(".desktop").size());
    }
    return id;
  }

  std::string iconOf(GAppInfo* info) {
    GIcon* icon = g_app_info_get_icon(info);
    if (icon == nullptr) {
      return {};
    }
    if (G_IS_FILE_ICON(icon)) {
      gchar* path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));
      std::string out = path != nullptr ? path : "";
      g_free(path);
      return out;
    }
    if (G_IS_THEMED_ICON(icon)) {
      for (const gchar* const* name = g_themed_icon_get_names(G_THEMED_ICON(icon)); name != nullptr && *name != nullptr;
           ++name) {
        if (std::string file = iconFileFor(*name); !file.empty()) {
          return file;
        }
      }
    }
    return {};
  }

  AppChooserSheet::App appFor(GAppInfo* info) {
    return {.id = withoutDesktop(g_app_info_get_id(info) != nullptr ? g_app_info_get_id(info) : ""),
            .name = g_app_info_get_display_name(info) != nullptr ? g_app_info_get_display_name(info) : "",
            .icon = iconOf(info)};
  }

  std::optional<AppChooserSheet::App> appById(const std::string& id) {
    GDesktopAppInfo* info = g_desktop_app_info_new((id + ".desktop").c_str());
    if (info == nullptr) {
      return std::nullopt;
    }
    AppChooserSheet::App app = appFor(G_APP_INFO(info));
    g_object_unref(info);
    return app;
  }
} // namespace

std::string iconFileFor(const std::string& icon) {
  if (icon.empty()) {
    return {};
  }
  std::error_code ec;
  if (icon.starts_with('/')) {
    return fs::exists(icon, ec) ? icon : std::string();
  }
  // The sizes a 24-48 px row looks sharp with at 1x-2x, then any scalable art, then pixmaps.
  std::vector<fs::path> roots;
  if (const char* home = g_get_user_data_dir()) {
    roots.emplace_back(fs::path(home) / "icons");
  }
  for (const gchar* const* dir = g_get_system_data_dirs(); dir != nullptr && *dir != nullptr; ++dir) {
    roots.emplace_back(fs::path(*dir) / "icons");
  }
  static constexpr std::array kSizes{"scalable", "96x96", "64x64", "128x128", "48x48", "256x256", "32x32"};
  for (const fs::path& root : roots) {
    for (const char* size : kSizes) {
      for (const char* ext : {".svg", ".png"}) {
        const fs::path file = root / "hicolor" / size / "apps" / (icon + ext);
        if (fs::exists(file, ec)) {
          return file.string();
        }
      }
    }
  }
  for (const char* ext : {".svg", ".png", ".xpm"}) {
    const fs::path file = fs::path("/usr/share/pixmaps") / (icon + ext);
    if (fs::exists(file, ec)) {
      return file.string();
    }
  }
  return {};
}

std::vector<AppChooserSheet::App> AppChooserSheet::allApps() {
  std::vector<App> out;
  GList* list = g_app_info_get_all();
  for (GList* l = list; l != nullptr; l = l->next) {
    auto* info = G_APP_INFO(l->data);
    if (g_app_info_should_show(info)) {
      out.push_back(appFor(info));
    }
  }
  g_list_free_full(list, g_object_unref);
  std::ranges::sort(out, [](const App& a, const App& b) { return TypeAhead::fold(a.name) < TypeAhead::fold(b.name); });
  return out;
}

AppChooserSheet::AppChooserSheet(AppChooserPortal::ChooseRequest request,
                                 std::function<void(std::uint32_t, const std::string&)> answer,
                                 std::function<void()> rebuild)
    : m_request(std::move(request)), m_answer(std::move(answer)), m_rebuild(std::move(rebuild)) {
  if (!m_request.contentType.empty()) {
    if (GAppInfo* def = g_app_info_get_default_for_type(m_request.contentType.c_str(), FALSE)) {
      m_default = withoutDesktop(g_app_info_get_id(def) != nullptr ? g_app_info_get_id(def) : "");
      g_object_unref(def);
    }
  }
  reloadRecommended();
  m_all = allApps();
  m_selected = !m_request.lastChoice.empty()      ? m_request.lastChoice
      : !m_default.empty()                         ? m_default
      : !m_recommended.empty()                     ? m_recommended.front().id
                                                   : std::string();
}

void AppChooserSheet::reloadRecommended() {
  m_recommended.clear();
  for (const std::string& id : m_request.choices) {
    if (auto app = appById(withoutDesktop(id))) {
      m_recommended.push_back(std::move(*app));
    }
  }
  // No list from the caller: the apps that declare this type.
  if (m_recommended.empty() && !m_request.contentType.empty()) {
    GList* list = g_app_info_get_recommended_for_type(m_request.contentType.c_str());
    for (GList* l = list; l != nullptr; l = l->next) {
      m_recommended.push_back(appFor(G_APP_INFO(l->data)));
    }
    g_list_free_full(list, g_object_unref);
  }
  // The default first.
  std::ranges::stable_partition(m_recommended, [this](const App& a) { return a.id == m_default; });
}

void AppChooserSheet::updateChoices(const std::vector<std::string>& choices) {
  m_request.choices = choices;
  reloadRecommended();
  if (m_rebuild) {
    m_rebuild();
  }
}

void AppChooserSheet::select(const std::string& id) {
  m_selected = id;
  m_rebuild();
}

void AppChooserSheet::cancel() {
  if (!m_answered) {
    m_answered = true;
    m_answer(1, {});
  }
}

void AppChooserSheet::open() {
  if (m_answered || m_selected.empty()) {
    return;
  }
  if (m_alwaysUse && !m_request.contentType.empty()) {
    if (GDesktopAppInfo* info = g_desktop_app_info_new((m_selected + ".desktop").c_str())) {
      GError* error = nullptr;
      if (!g_app_info_set_as_default_for_type(G_APP_INFO(info), m_request.contentType.c_str(), &error)) {
        g_clear_error(&error);
      }
      g_object_unref(info);
    }
  }
  m_answered = true;
  m_answer(0, m_selected);
}

std::unique_ptr<Node> AppChooserSheet::build(float s) {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                         .flexGrow = 1.0F});
  const std::string title = m_request.filename.empty() ? std::string("Open With")
                                                       : std::format("Open “{}” with", m_request.filename);
  box->addChild(ui::label({.text = title, .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                           .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                           .ellipsize = TextEllipsize::Middle}));
  const float rowH = Tokens::sizeRowCompact * s + Tokens::spaceP8 * s;
  const auto card = [&](const std::vector<App>& apps, bool markDefault) {
    auto list = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s,
                            .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8 * s});
    for (const App& app : apps) {
      const bool selected = app.id == m_selected;
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingH = Tokens::spaceP8 * s,
                          .fill = selected ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F) : ColorSpec{},
                          .radius = Tokens::radiusR8 * s, .height = rowH});
      row->addChild(std::make_unique<AppIconNode>(app.icon, Tokens::spaceP24 * s));
      row->addChild(ui::label({.text = app.name, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::Medium,
                               .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                               .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
      if (markDefault && app.id == m_default) {
        row->addChild(ui::label({.text = std::string("Default"), .fontSize = Style::fontSizeCaption * s,
                                 .color = colorSpecFromRole(ColorRole::Label2)}));
      }
      auto area = ui::inputArea({.onClick = [this, id = app.id](const InputArea::PointerData& p) {
                                   // A second click on the same row within the double-click time opens it.
                                   const bool twice = m_selected == id && p.time - m_lastClickTime < kDoubleClickMs;
                                   m_lastClickTime = p.time;
                                   if (twice) {
                                     open();
                                     return;
                                   }
                                   m_selected = id;
                                   if (m_rebuild) {
                                     m_rebuild();
                                   }
                                 }});
      area->addChild(std::move(row));
      list->addChild(std::move(area));
    }
    return list;
  };
  if (!m_recommended.empty()) {
    box->addChild(ui::label({.text = std::string("Recommended"), .fontSize = Style::fontSizeCaption * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label2)}));
    box->addChild(card(m_recommended, true));
  }
  box->addChild(ui::input({
      .value = m_search,
      .placeholder = std::string("All apps"),
      .fontSize = Style::fontSizeBody * s,
      .onChange =
          [this](const std::string& text) {
            m_search = text;
            if (m_rebuild) {
              m_rebuild();
            }
          },
      .onSubmit = [this](const std::string&) { open(); },
  }));
  std::vector<App> rest;
  const std::string needle = TypeAhead::fold(m_search);
  for (const App& app : m_all) {
    const bool recommended = std::ranges::any_of(m_recommended, [&](const App& r) { return r.id == app.id; });
    if (!recommended && (needle.empty() || TypeAhead::fold(app.name).find(needle) != std::string::npos)) {
      rest.push_back(app);
    }
  }
  if (rest.size() > kAllAppsShown) {
    rest.resize(kAllAppsShown);
  }
  auto scroll = ui::scrollView({.flexGrow = 1.0F});
  scroll->content()->addChild(card(rest, false));
  box->addChild(std::move(scroll));
  if (m_recommended.empty() && rest.empty()) {
    box->addChild(ui::label({.text = std::string("No app can open this file"), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
  }
  if (!m_request.contentType.empty() && !m_selected.empty()) {
    gchar* desc = g_content_type_get_description(m_request.contentType.c_str());
    const std::string type = desc != nullptr ? desc : m_request.contentType;
    g_free(desc);
    std::string name = m_selected;
    for (const auto* list : {&m_recommended, &m_all}) {
      for (const App& app : *list) {
        name = app.id == m_selected ? app.name : name;
      }
    }
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    row->addChild(ui::checkbox({.checked = m_alwaysUse, .onChange = [this](bool on) { m_alwaysUse = on; }}));
    row->addChild(ui::label({.text = std::format("Always use {} for {} files", name, type),
                             .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1),
                             .maxLines = 1, .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
    box->addChild(std::move(row));
  }
  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
  buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Default, .onClick = [this] {
                                  if (!m_answered) {
                                    m_answered = true;
                                    m_answer(1, {});
                                  }
                                }}));
  buttons->addChild(ui::button({.text = std::string("Open"), .fontSize = Style::fontSizeBody * s,
                                .enabled = !m_selected.empty(), .variant = ButtonVariant::Primary,
                                .onClick = [this] { open(); }}));
  box->addChild(std::move(buttons));
  return box;
}

AccessSheet::AccessSheet(AppChooserPortal::AccessRequest request,
                         std::function<void(std::uint32_t, const std::vector<AppChooserPortal::AccessChoice>&)> answer)
    : m_request(std::move(request)), m_answer(std::move(answer)) {}

std::unique_ptr<Node> AccessSheet::build(float s) {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                         .flexGrow = 1.0F});
  if (const std::string icon = iconFileFor(m_request.icon); !icon.empty()) {
    box->addChild(std::make_unique<AppIconNode>(icon, Tokens::spaceP48 * s));
  }
  box->addChild(ui::label({.text = m_request.title, .fontSize = Tokens::typeTitle3.size * s,
                           .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
                           .maxLines = 3}));
  if (!m_request.subtitle.empty()) {
    box->addChild(ui::label({.text = m_request.subtitle, .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 4}));
  }
  if (!m_request.body.empty()) {
    box->addChild(ui::label({.text = m_request.body, .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 8}));
  }
  for (std::size_t i = 0; i < m_request.choices.size(); ++i) {
    const auto& choice = m_request.choices[i];
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    if (choice.options.empty()) {
      row->addChild(ui::checkbox({.checked = choice.value == "true", .onChange = [this, i](bool on) {
                                    m_request.choices[i].value = on ? "true" : "false";
                                  }}));
      row->addChild(ui::label({.text = choice.label, .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label1)}));
    } else {
      row->addChild(ui::label({.text = choice.label, .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
      std::vector<std::string> names;
      std::size_t selected = 0;
      for (std::size_t k = 0; k < choice.options.size(); ++k) {
        names.push_back(choice.options[k].second);
        selected = choice.options[k].first == choice.value ? k : selected;
      }
      row->addChild(ui::select({.options = std::move(names), .selectedIndex = selected,
                                .fontSize = Style::fontSizeBody * s,
                                .onSelectionChanged = [this, i](std::size_t index, std::string_view) {
                                  if (index < m_request.choices[i].options.size()) {
                                    m_request.choices[i].value = m_request.choices[i].options[index].first;
                                  }
                                }}));
    }
    box->addChild(std::move(row));
  }
  box->addChild(ui::node({.flexGrow = 1.0F}));
  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
  buttons->addChild(ui::button({.text = m_request.denyLabel, .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Default, .onClick = [this] { answer(false); }}));
  buttons->addChild(ui::button({.text = m_request.grantLabel, .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Primary, .onClick = [this] { answer(true); }}));
  box->addChild(std::move(buttons));
  return box;
}

void AccessSheet::answer(bool grant) {
  if (!m_answered) {
    m_answered = true;
    m_answer(grant ? 0 : 1, grant ? m_request.choices : std::vector<AppChooserPortal::AccessChoice>{});
  }
}

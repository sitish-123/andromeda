#include "shell/settings/settings_content_plugins.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  namespace {
    std::unique_ptr<Label>
    makeLabel(std::string_view text, float fontSize, ColorRole role, FontWeight weight = FontWeight::Normal) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .fontWeight = weight,
          .color = colorSpecFromRole(role),
      });
    }

    bool pluginEnabled(const scripting::PluginStatus& plugin, const SettingsPluginsContext& ctx) {
      if (ctx.config == nullptr) {
        return plugin.enabled;
      }
      return std::ranges::contains(ctx.config->plugins.enabled, plugin.id);
    }

    std::string pluginSourceDisplayName(std::string_view source) {
      if (source == "official") {
        return "Official";
      }
      if (source == "community") {
        return "Community";
      }
      return std::string(source);
    }

    int pluginSourceOrder(std::string_view source) {
      if (source == "official") {
        return 0;
      }
      if (source == "community") {
        return 1;
      }
      return 2;
    }

    bool pluginSourceLess(std::string_view a, std::string_view b) {
      const int aOrder = pluginSourceOrder(a);
      const int bOrder = pluginSourceOrder(b);
      if (aOrder != bOrder) {
        return aOrder < bOrder;
      }
      return a < b;
    }

    // ── Per-plugin settings editor ─────────────────────────────────────────

    std::string valueAsString(const WidgetSettingValue& value) {
      if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
      }
      if (const auto* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return std::to_string(*d);
      }
      return {};
    }

    std::vector<std::string> valueAsStringList(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::vector<std::string>>(&value)) {
        return *v;
      }
      return {};
    }

    WidgetSettingStringMap valueAsStringMap(const WidgetSettingValue& value) {
      if (const auto* map = std::get_if<WidgetSettingStringMap>(&value)) {
        return *map;
      }
      return {};
    }

    bool valueAsBool(const WidgetSettingValue& value) {
      if (const auto* b = std::get_if<bool>(&value)) {
        return *b;
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i != 0;
      }
      return false;
    }

    std::int64_t valueAsInt(const WidgetSettingValue& value) {
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i;
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(std::llround(*d));
      }
      return 0;
    }

    double valueAsDouble(const WidgetSettingValue& value) {
      if (const auto* d = std::get_if<double>(&value)) {
        return *d;
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
      }
      return 0.0;
    }

    // Current value for a plugin setting: the override if present, else the manifest default.
    WidgetSettingValue
    pluginSettingValue(const Config& cfg, const std::string& pluginId, const WidgetSettingSpec& spec) {
      const auto pluginIt = cfg.plugins.pluginSettings.find(pluginId);
      if (pluginIt != cfg.plugins.pluginSettings.end()) {
        const auto keyIt = pluginIt->second.find(spec.schema.key);
        if (keyIt != pluginIt->second.end()) {
          return keyIt->second;
        }
      }
      return spec.schema.defaultValue;
    }

    bool pluginSettingVisible(
        const Config& cfg, const std::string& pluginId, const WidgetSettingSpec& spec,
        const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      if (!spec.visibleWhen.has_value()) {
        return true;
      }
      const auto currentString = [&](const std::string& key) -> std::string {
        const auto depIt =
            std::ranges::find_if(allSpecs, [&](const WidgetSettingSpec& s) { return s.schema.key == key; });
        if (depIt == allSpecs.end()) {
          return {};
        }
        return valueAsString(pluginSettingValue(cfg, pluginId, *depIt));
      };
      const auto matches = [&](const WidgetSettingVisibilityCondition& cond) {
        if (cond.nonEmpty) {
          const auto depIt =
              std::ranges::find_if(allSpecs, [&](const WidgetSettingSpec& s) { return s.schema.key == cond.key; });
          if (depIt == allSpecs.end()) {
            return false;
          }
          const WidgetSettingValue value = pluginSettingValue(cfg, pluginId, *depIt);
          if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
            return !list->empty();
          }
          if (const auto* str = std::get_if<std::string>(&value)) {
            return !str->empty();
          }
          return false;
        }
        const std::string value = currentString(cond.key);
        return std::ranges::contains(cond.values, value);
      };
      // Visible when any `any` alternative matches (or none declared) AND every `all` condition matches.
      const auto& vis = *spec.visibleWhen;
      const bool anyOk = vis.any.empty() || std::ranges::any_of(vis.any, matches);
      const bool allOk = std::ranges::all_of(vis.all, matches);
      return anyOk && allOk;
    }

    std::unique_ptr<Node> pluginSettingControl(
        SettingsControlFactory& factory, const WidgetSettingSpec& spec, const WidgetSettingValue& value,
        const std::vector<std::string>& path
    ) {
      switch (spec.control) {
      case WidgetControlKind::Bool: {
        std::optional<bool> clearWhenValue;
        if (const auto* defaultBool = std::get_if<bool>(&spec.schema.defaultValue)) {
          clearWhenValue = *defaultBool;
        }
        return factory.makeToggle(valueAsBool(value), true, path, clearWhenValue);
      }
      case WidgetControlKind::Int: {
        const double minValue = spec.schema.minValue.value_or(0.0);
        const double maxValue = spec.schema.maxValue.value_or(100.0);
        return factory.makeSlider(
            static_cast<double>(valueAsInt(value)), minValue, maxValue, spec.schema.step.value_or(1.0), path,
            /*integerValue=*/true
        );
      }
      case WidgetControlKind::Double: {
        const double minValue = spec.schema.minValue.value_or(0.0);
        const double maxValue = spec.schema.maxValue.value_or(1.0);
        return factory.makeSlider(
            valueAsDouble(value), minValue, maxValue, spec.schema.step.value_or(1.0), path, false
        );
      }
      case WidgetControlKind::Select: {
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (const auto& option : spec.options) {
          options.push_back(
              SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
          );
        }
        SelectSetting selectSetting{std::move(options), valueAsString(value)};
        selectSetting.segmented = spec.segmented;
        if (spec.schema.type == noctalia::config::schema::WidgetSettingType::Bool) {
          selectSetting.valueType = SelectValueType::Boolean;
        }
        if (const auto* defaultString = std::get_if<std::string>(&spec.schema.defaultValue)) {
          selectSetting.clearOnEmpty = defaultString->empty();
        }
        return factory.makeSelect(selectSetting, path);
      }
      case WidgetControlKind::ColorSpec: {
        ColorSpecPickerSetting pickerSetting;
        pickerSetting.selectedValue = valueAsString(value);
        pickerSetting.allowNone = spec.advanced;
        pickerSetting.allowCustomColor = spec.allowCustomColor;
        return factory.makeColorSpecPicker(pickerSetting, path);
      }
      case WidgetControlKind::StringList:
      case WidgetControlKind::StringMap:
        return nullptr;
      case WidgetControlKind::File:
      case WidgetControlKind::Folder:
        return factory.makePathBrowse(
            TextSetting{
                .value = valueAsString(value),
                .placeholder = {},
                .width = 190.0F,
                .browseMode = spec.control == WidgetControlKind::Folder ? TextSettingBrowseMode::SelectFolder
                                                                        : TextSettingBrowseMode::OpenFile,
                .browseFileExtensions = spec.extensions,
                .browseFallbackDirectory = {},
            },
            path
        );
      case WidgetControlKind::Glyph: {
        const std::string currentValue = valueAsString(value);
        auto textNode = factory.makeText(currentValue, {}, path);
        return ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * factory.scale(),
            },
            std::move(textNode),
            ui::button({
                .glyph = "apps",
                .glyphSize = Style::fontSizeBody * factory.scale(),
                .variant = ButtonVariant::Default,
                .minWidth = Style::controlHeight * factory.scale(),
                .minHeight = Style::controlHeight * factory.scale(),
                .paddingV = Style::spaceXs * factory.scale(),
                .paddingH = Style::spaceSm * factory.scale(),
                .radius = Style::scaledRadiusMd(factory.scale()),
                .onClick = [setOverride = factory.context().setOverride, path, currentValue] {
                  GlyphPickerDialogOptions options;
                  if (!currentValue.empty()) {
                    options.initialGlyph = currentValue;
                  }
                  (void)GlyphPickerDialog::open(
                      std::move(options), [setOverride, path](std::optional<GlyphPickerResult> result) {
                        if (result.has_value()) {
                          setOverride(path, result->name);
                        }
                      }
                  );
                },
            })
        );
      }
      case WidgetControlKind::String:
      default:
        return factory.makeText(valueAsString(value), {}, path);
      }
    }

  } // namespace

  bool pluginHasSettings(const scripting::PluginManifest& manifest) {
    if (!manifest.settings.empty()) {
      return true;
    }
    return std::ranges::any_of(manifest.entries, [](const scripting::PluginEntry& entry) {
      return entry.kind == scripting::PluginEntryKind::Panel && !entry.settings.empty();
    });
  }

  void buildPluginSettingsEditor(
      Flex& body, const Config& cfg, SettingsControlFactory& factory, const std::string& pluginId,
      const scripting::PluginManifest& manifest, bool showAdvanced, float scale
  ) {
    scripting::PluginTranslationCatalog translations;
    if (const auto pluginDir = scripting::PluginRegistry::instance().findPluginDir(pluginId)) {
      translations.load(*pluginDir);
    }
    std::vector<WidgetSettingSpec> specs = settings::manifestSettingSpecs(manifest.settings, &translations);
    for (const auto& entry : manifest.entries) {
      if (entry.kind != scripting::PluginEntryKind::Panel) {
        continue;
      }
      for (const auto& shellSpec : settings::pluginPanelShellSettingSpecs(entry)) {
        if (std::ranges::any_of(specs, [&](const WidgetSettingSpec& existing) {
              return existing.schema.key == shellSpec.schema.key;
            })) {
          continue;
        }
        specs.push_back(shellSpec);
      }
      for (const auto& panelSpec : settings::manifestSettingSpecs(entry.settings, &translations)) {
        if (scripting::isPanelShellSettingKey(entry.id, panelSpec.schema.key)) {
          continue;
        }
        if (std::ranges::any_of(specs, [&](const WidgetSettingSpec& existing) {
              return existing.schema.key == panelSpec.schema.key;
            })) {
          continue;
        }
        specs.push_back(panelSpec);
      }
    }
    bool rendered = false;
    for (const auto& spec : specs) {
      if (spec.advanced && !showAdvanced) {
        continue;
      }
      if (!pluginSettingVisible(cfg, pluginId, spec, specs)) {
        continue;
      }
      const std::vector<std::string> path = {"plugin_settings", pluginId, spec.schema.key};
      const WidgetSettingValue value = pluginSettingValue(cfg, pluginId, spec);
      SettingEntry entry{
          .section = SettingsSection::Bar,
          .group = "plugin-settings",
          .title = spec.literalLabel,
          .subtitle = spec.literalDescription,
          .path = path,
          .control = TextSetting{},
          .advanced = spec.advanced,
          .searchText = {},
      };
      if (spec.control == WidgetControlKind::StringList) {
        factory.makeListBlock(body, entry, ListSetting{.items = valueAsStringList(value)});
      } else if (spec.control == WidgetControlKind::StringMap) {
        factory.makeStringMapBlock(
            body, entry,
            StringMapSetting{
                .entries = valueAsStringMap(value),
                .keyPlaceholder = i18n::tr("settings.widgets.map-placeholders.key"),
                .valuePlaceholder = i18n::tr("settings.widgets.map-placeholders.value"),
            }
        );
      } else {
        factory.makeRow(body, entry, pluginSettingControl(factory, spec, value, path));
      }
      rendered = true;
    }
    if (!rendered) {
      body.addChild(makeLabel(
          i18n::tr("settings.plugins.settings.empty"), Style::fontSizeCaption * scale, ColorRole::Label2
      ));
    }
  }

  void addSettingsPlugins(Flex& content, SettingsPluginsContext ctx) {
    if (ctx.selectedSection != "plugins") {
      return;
    }
    using namespace page_rows;
    const float scale = ctx.scale;

    if (ctx.config != nullptr && ctx.config->shell.offlineMode) {
      content.addChild(makeOfflineModeNotice(scale, i18n::tr("settings.window.offline-mode-notice.plugins")));
    }

    std::vector<scripting::PluginStatus> plugins;
    for (const auto& plugin : ctx.plugins) {
      if (plugin.materialized || plugin.enabled) {
        plugins.push_back(plugin);
      }
    }
    std::ranges::sort(plugins, [](const auto& a, const auto& b) {
      if (!StringUtils::equalsInsensitive(a.name, b.name)) {
        return std::ranges::lexicographical_compare(a.name, b.name, [](char x, char y) {
          return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
      }
      return a.source != b.source ? pluginSourceLess(a.source, b.source) : a.id < b.id;
    });
    const int updatesAvailable =
        static_cast<int>(std::ranges::count_if(plugins, &scripting::PluginStatus::updateAvailable));

    Flex* installed = addSettingsGroupCard(content, i18n::tr("settings.plugins.installed"), scale);
    if (plugins.empty()) {
      auto r = row(scale);
      r->addChild(title(i18n::tr(ctx.pluginsLoading ? "settings.plugins.loading" : "settings.plugins.plugins.empty"), scale));
      if (ctx.pluginsLoading) {
        r->addChild(ui::spinner({.spinnerSize = Tokens::sizeSymbol * scale, .spinning = true}));
      }
      installed->addChild(std::move(r));
    }
    for (const auto& plugin : plugins) {
      const bool enabled = pluginEnabled(plugin, ctx);
      std::string sub = pluginSourceDisplayName(plugin.source.empty() ? std::string("local") : plugin.source);
      sub += " · " + (plugin.version.empty() ? std::string("?") : plugin.version);
      if (!plugin.compatible) {
        sub += " · " + i18n::tr("settings.plugins.plugins.incompatible-plugin-api");
      } else if (plugin.updateAvailable) {
        sub += " · " + (plugin.availableVersion.empty()
                                 ? i18n::tr("settings.plugins.plugins.update-available")
                                 : i18n::tr("settings.plugins.plugins.update-to", "version", plugin.availableVersion));
      } else if (plugin.heldBack) {
        sub += " · " + i18n::tr("settings.plugins.plugins.held-back");
      } else if (plugin.deprecated) {
        sub += " · " + i18n::tr("settings.plugins.plugins.deprecated");
      }
      Flex* r = addTwoLineRow(*installed, plugin.icon.empty() ? std::string_view("puzzle") : std::string_view(plugin.icon),
                              plugin.name, std::move(sub), scale);
      if (ctx.isEnabling && ctx.isEnabling(plugin.id)) {
        r->addChild(ui::spinner({.spinnerSize = Tokens::sizeSymbol * scale, .spinning = true}));
      } else {
        r->addChild(ui::toggle({
            .checked = enabled,
            .enabled = enabled || plugin.compatible,
            .scale = scale,
            .onChange = [cb = ctx.setEnabled, id = plugin.id](bool on) { cb(id, on); },
        }));
      }
      if (ctx.onConfigure) {
        r->addChild(chevronButton(plugin.name, scale, [cb = ctx.onConfigure, id = plugin.id] { cb(id); }));
      }
    }
    if (updatesAvailable > 0 && ctx.updateAll) {
      addActionRow(*installed, i18n::tr("settings.plugins.plugins.update-all", "count", std::to_string(updatesAvailable)),
                   true, scale, ctx.updateAll);
    }
    if (ctx.openStore) {
      addActionRow(*installed, i18n::tr("settings.plugins.get"), true, scale, ctx.openStore);
    }
    addSettingsRowSeparators(*installed, scale);

    Flex* sources = addSettingsGroupCard(content, i18n::tr("settings.plugins.sources.title"), scale);
    for (const auto& source : ctx.sources) {
      auto r = labelRow("world", pluginSourceDisplayName(source.name), scale);
      r->addChild(valueText(source.kind == PluginSourceKind::Git ? i18n::tr("settings.plugins.sources.kind.git")
                                                                 : i18n::tr("settings.plugins.sources.kind.path"),
                            scale));
      if (source.enabled && source.kind == PluginSourceKind::Git && ctx.updateSource) {
        r->addChild(iconButton("refresh", i18n::tr("settings.plugins.sources.update"), scale,
                               [cb = ctx.updateSource, name = source.name] { cb(name); }));
      }
      r->addChild(ui::toggle({.checked = source.enabled, .scale = scale,
                              .onChange = [cb = ctx.setSourceEnabled, source](bool on) { cb(source, on); }}));
      if (ctx.editSource) {
        r->addChild(chevronButton(i18n::tr("settings.plugins.sources.edit"), scale,
                                  [cb = ctx.editSource, source] { cb(source); }));
      }
      sources->addChild(std::move(r));
    }
    const bool hasGitSource =
        std::ranges::any_of(ctx.sources, [](const PluginSourceConfig& s) { return s.kind == PluginSourceKind::Git && s.enabled; });
    if (hasGitSource && ctx.setAutoUpdate) {
      std::vector<std::string> modes;
      std::optional<std::size_t> selected;
      for (const auto& opt : kPluginAutoUpdateModes) {
        if (opt.value == ctx.autoUpdateMode) {
          selected = modes.size();
        }
        modes.push_back(i18n::tr(opt.labelKey));
      }
      auto r = labelRow("refresh", i18n::tr("settings.plugins.sources.auto-update"), scale);
      r->addChild(ui::select({
          .options = std::move(modes),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Tokens::sizeControlS * scale,
          .glyphSize = Tokens::sizeSymbolInline * scale,
          .onSelectionChanged = [cb = ctx.setAutoUpdate](std::size_t index, std::string_view) {
            if (index < std::size(kPluginAutoUpdateModes)) {
              cb(kPluginAutoUpdateModes[index].value);
            }
          },
      }));
      sources->addChild(std::move(r));
    }
    if (ctx.addSource) {
      addActionRow(*sources, i18n::tr("settings.plugins.sources.add-row"), !ctx.sources.empty(), scale, ctx.addSource);
    }
    addSettingsRowSeparators(*sources, scale);
    addFootnote(*sources,
                i18n::tr(ctx.sources.size() > 1 ? "settings.plugins.footnote-precedence" : "settings.plugins.footnote"),
                scale);
  }

} // namespace settings

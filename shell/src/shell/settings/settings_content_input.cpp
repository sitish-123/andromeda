#include "shell/settings/settings_content_input.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace settings {

  namespace {

    constexpr float kSelectWidth = 220.0F;
    constexpr float kSliderWidth = 200.0F;

    std::vector<std::string> splitOptions(std::string_view options) {
      std::vector<std::string> tokens;
      for (std::size_t start = 0; start <= options.size();) {
        const std::size_t comma = options.find(',', start);
        std::string_view token = options.substr(start, comma - start);
        if (!token.empty()) {
          tokens.emplace_back(token);
        }
        if (comma == std::string_view::npos) {
          break;
        }
        start = comma + 1;
      }
      return tokens;
    }

    std::string joinOptions(const std::vector<std::string>& tokens) {
      std::string out;
      for (const std::string& token : tokens) {
        if (!out.empty()) {
          out += ',';
        }
        out += token;
      }
      return out;
    }

    // Replaces whichever token belongs to `group` (a "group:option" prefix) with `chosen`, or drops it when empty.
    std::string withGroupOption(std::string_view current, std::string_view group, std::string_view chosen) {
      std::vector<std::string> tokens = splitOptions(current);
      const std::string prefix = std::format("{}:", group);
      std::erase_if(tokens, [&](const std::string& t) { return t.starts_with(prefix); });
      if (!chosen.empty()) {
        tokens.emplace_back(chosen);
      }
      return joinOptions(tokens);
    }

    std::string groupOptionValue(std::string_view current, std::string_view group) {
      const std::string prefix = std::format("{}:", group);
      for (const std::string& token : splitOptions(current)) {
        if (token.starts_with(prefix)) {
          return token;
        }
      }
      return {};
    }

    double toDouble(std::string_view text, double fallback) {
      double value = fallback;
      std::from_chars(text.data(), text.data() + text.size(), value);
      return value;
    }

    std::optional<std::size_t> indexOf(const std::vector<std::string>& values, std::string_view value) {
      const auto it = std::ranges::find(values, value);
      return it != values.end() ? std::optional<std::size_t>(it - values.begin()) : std::nullopt;
    }

    // A row the settings file customises carries the same reset control as registry rows: it clears the key so
    // config.toml or the built-in default applies again.
    std::unique_ptr<Flex> makeInputRow(
        std::string_view title, std::unique_ptr<Node> control, std::string_view key, const SettingsInputContext& ctx
    ) {
      const float scale = ctx.scale;
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = metrics::kRowHeight * scale, .fillWidth = true});
      row->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1), FontWeight::Medium)
      );
      row->addChild(ui::spacer());
      if (ctx.input->customized(key)) {
        row->addChild(
            ui::button({
                .glyph = "arrow-back-up",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.actions.reset-to-default"),
                .minWidth = Style::controlHeight * scale,
                .minHeight = Style::controlHeight * scale,
                .padding = Style::spaceXs * scale,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = [set = ctx.set, key = std::string(key)]() { set(key, ""); },
            })
        );
      }
      row->addChild(std::move(control));
      return row;
    }

    std::unique_ptr<Node> makeInputSelect(
        std::vector<std::string> options, std::optional<std::size_t> selected, float scale,
        std::function<void(std::size_t)> onSelect
    ) {
      return ui::select({
          .options = std::move(options),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .width = kSelectWidth * scale,
          .height = Style::controlHeight * scale,
          .onSelectionChanged = [onSelect = std::move(onSelect)](std::size_t index, std::string_view) {
            onSelect(index);
          },
      });
    }

    std::unique_ptr<Node> makeInputToggle(bool checked, float scale, std::function<void(bool)> onChange) {
      return ui::toggle({
          .checked = checked,
          .scale = scale,
          .onChange = std::move(onChange),
      });
    }

    std::unique_ptr<Node> makeInputSlider(
        double value, double minValue, double maxValue, double step, bool integerValue, float scale,
        std::function<void(double)> onCommit, std::string startLabel = {}, std::string endLabel = {}
    ) {
      auto slider = ui::slider({
          .minValue = minValue,
          .maxValue = maxValue,
          .step = step,
          .value = value,
          .width = kSliderWidth * scale,
          .onValueChanged = [onCommit = std::move(onCommit), integerValue](double v) {
            onCommit(integerValue ? std::round(v) : v);
          },
      });
      if (startLabel.empty()) {
        return slider;
      }
      const auto end = [&](std::string text) {
        return makeLabel(text, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2));
      };
      return ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, end(std::move(startLabel)), std::move(slider),
          end(std::move(endLabel))
      );
    }

    void addToggleRow(Flex& body, std::string_view titleKey, std::string_view key, const SettingsInputContext& ctx) {
      body.addChild(makeInputRow(
          i18n::tr(titleKey),
          makeInputToggle(
              ctx.input->value(key) == "true", ctx.scale,
              [set = ctx.set, key = std::string(key)](bool checked) { set(key, checked ? "true" : "false"); }
          ),
          key, ctx
      ));
    }

    void addChoiceRow(
        Flex& body, std::string_view titleKey, std::string_view key, std::vector<std::string> values,
        std::vector<std::string> labels, const SettingsInputContext& ctx
    ) {
      const std::optional<std::size_t> selected = indexOf(values, ctx.input->value(key));
      body.addChild(makeInputRow(
          i18n::tr(titleKey),
          makeInputSelect(
              std::move(labels), selected, ctx.scale,
              [set = ctx.set, key = std::string(key), values = std::move(values)](std::size_t index) {
                set(key, values[index]);
              }
          ),
          key, ctx
      ));
    }

    // Icon themes with a cursors directory, in the XCursor search order (user dirs first, first name wins).
    std::vector<std::string> cursorThemes() {
      namespace fs = std::filesystem;
      std::vector<fs::path> roots;
      const char* home = std::getenv("HOME");
      const char* dataHome = std::getenv("XDG_DATA_HOME");
      if (dataHome != nullptr && *dataHome != '\0') {
        roots.emplace_back(fs::path(dataHome) / "icons");
      } else if (home != nullptr) {
        roots.emplace_back(fs::path(home) / ".local/share/icons");
      }
      if (home != nullptr) {
        roots.emplace_back(fs::path(home) / ".icons");
      }
      const char* dataDirs = std::getenv("XDG_DATA_DIRS");
      std::string_view dirs = dataDirs != nullptr && *dataDirs != '\0' ? dataDirs : "/usr/local/share:/usr/share";
      while (!dirs.empty()) {
        const std::size_t colon = dirs.find(':');
        roots.emplace_back(fs::path(dirs.substr(0, colon)) / "icons");
        dirs = colon == std::string_view::npos ? std::string_view{} : dirs.substr(colon + 1);
      }
      std::vector<std::string> themes;
      std::error_code ec;
      for (const fs::path& root : roots) {
        for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
          std::string name = entry.path().filename().string();
          if (fs::is_directory(entry.path() / "cursors", ec) && !std::ranges::contains(themes, name)) {
            themes.push_back(std::move(name));
          }
        }
      }
      std::ranges::sort(themes);
      return themes;
    }

    void addPointerCard(Flex& content, const SettingsInputContext& ctx) {
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.pointer"), ctx.scale);
      const SettingsControl& input = *ctx.input;

      std::vector<std::string> themes = cursorThemes();
      if (const std::string current = input.value("input.cursor.theme");
          !current.empty() && !std::ranges::contains(themes, current)) {
        themes.insert(themes.begin(), current);
      }
      std::vector<std::string> themeLabels = themes;
      addChoiceRow(*body, "settings.input.pointer.theme", "input.cursor.theme", std::move(themes),
                   std::move(themeLabels), ctx);

      std::vector<std::string> sizes = {"24", "32", "48", "64", "96"};
      if (const std::string current = input.value("input.cursor.size");
          !current.empty() && !std::ranges::contains(sizes, current)) {
        sizes.insert(sizes.begin(), current);
      }
      std::vector<std::string> sizeLabels;
      for (const std::string& size : sizes) {
        sizeLabels.push_back(std::format("{} px", size));
      }
      addChoiceRow(*body, "settings.input.pointer.size", "input.cursor.size", std::move(sizes), std::move(sizeLabels),
                   ctx);

      addToggleRow(*body, "settings.input.pointer.hide-while-typing", "input.cursor.hide_when_typing", ctx);

      // 0 = never hide.
      addChoiceRow(
          *body, "settings.input.pointer.hide-after", "input.cursor.hide_timeout_ms",
          {"0", "3000", "5000", "10000", "30000", "60000"},
          {i18n::tr("settings.input.pointer.hide-never"), "3 s", "5 s", "10 s", "30 s", "1 min"}, ctx
      );

      addToggleRow(*body, "settings.input.pointer.middle-click-paste", "input.middle_click_paste", ctx);
    }

    void addTabletCard(Flex& content, const SettingsInputContext& ctx) {
      if (std::ranges::none_of(ctx.input->devices(), [](const InputDevice& d) {
            return d.kind == InputDeviceKind::Tablet;
          })) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.tablet"), ctx.scale);
      addToggleRow(*body, "settings.input.tablet.enabled", "input.tablet.enabled", ctx);
      addToggleRow(*body, "settings.input.touchpad.left-handed", "input.tablet.left_handed", ctx);
    }

    void addTouchscreenCard(Flex& content, const SettingsInputContext& ctx) {
      if (std::ranges::none_of(ctx.input->devices(), [](const InputDevice& d) {
            return d.kind == InputDeviceKind::Touch;
          })) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.touchscreen"), ctx.scale);
      addToggleRow(*body, "settings.input.touchscreen.enabled", "input.touch.enabled", ctx);
    }

    void addKeyboardCard(Flex& content, const SettingsInputContext& ctx) {
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.keyboard"), ctx.scale);
      const SettingsControl& input = *ctx.input;
      const xkb::Catalog& catalog = *ctx.catalog;

      const std::string currentLayout = input.value("input.keyboard.layout");
      const std::string currentVariant = input.value("input.keyboard.variant");
      constexpr std::string_view layoutKey = "input.keyboard.layout";

      // Input sources: the compositor's layout key holds them comma separated, the first is the default.
      std::vector<std::string> sources = splitOptions(currentLayout);
      const auto describe = [&](const std::string& name) {
        const xkb::Layout* found = xkb::findLayout(catalog, name);
        return found != nullptr ? found->description : name;
      };
      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale,
                            .minHeight = metrics::kRowHeight * ctx.scale, .fillWidth = true});
        row->addChild(makeLabel(describe(sources[i]), Style::fontSizeBody * ctx.scale,
                                colorSpecFromRole(ColorRole::Label1), FontWeight::Medium));
        row->addChild(ui::spacer());
        if (i == 0 && sources.size() > 1) {
          row->addChild(makeLabel(i18n::tr("settings.input.keyboard.default-source"), Style::fontSizeBody * ctx.scale,
                                  colorSpecFromRole(ColorRole::Label2)));
        }
        if (sources.size() > 1) {
          row->addChild(ui::button({
              .glyph = "minus",
              .glyphSize = Style::fontSizeBody * ctx.scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("settings.input.keyboard.remove-source"),
              .minWidth = Style::controlHeight * ctx.scale,
              .minHeight = Style::controlHeight * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .radius = Style::spaceSm * ctx.scale,
              .onClick = [set = ctx.set, sources, i]() {
                std::vector<std::string> rest = sources;
                rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));
                set("input.keyboard.layout", joinOptions(rest));
                if (i == 0) {
                  set("input.keyboard.variant", "");
                }
              },
          }));
        }
        body->addChild(std::move(row));
      }
      {
        std::vector<std::string> addLabels;
        std::vector<std::string> addNames;
        for (const xkb::Layout& layout : catalog.layouts) {
          if (!std::ranges::contains(sources, layout.name)) {
            addNames.push_back(layout.name);
            addLabels.push_back(layout.description);
          }
        }
        body->addChild(makeInputRow(
            i18n::tr("settings.input.keyboard.add-source"),
            makeInputSelect(
                std::move(addLabels), std::nullopt, ctx.scale,
                [set = ctx.set, sources, addNames = std::move(addNames)](std::size_t index) {
                  std::vector<std::string> next = sources;
                  next.push_back(addNames[index]);
                  set("input.keyboard.layout", joinOptions(next));
                }
            ),
            layoutKey, ctx
        ));
      }

      if (const xkb::Layout* layout = sources.size() == 1 ? xkb::findLayout(catalog, currentLayout) : nullptr;
          layout != nullptr && !layout->variants.empty()) {
        std::vector<std::string> variantLabels = {i18n::tr("settings.input.keyboard.variant-default")};
        std::vector<std::string> variantNames = {""};
        std::optional<std::size_t> variantSelected = 0;
        for (const xkb::LayoutVariant& variant : layout->variants) {
          if (variant.name == currentVariant) {
            variantSelected = variantNames.size();
          }
          variantNames.push_back(variant.name);
          variantLabels.push_back(variant.description);
        }
        constexpr std::string_view variantKey = "input.keyboard.variant";
        body->addChild(makeInputRow(
            i18n::tr("settings.input.keyboard.variant"),
            makeInputSelect(
                std::move(variantLabels), variantSelected, ctx.scale,
                [set = ctx.set, variantNames = std::move(variantNames)](std::size_t index) {
                  set("input.keyboard.variant", variantNames[index]);
                }
            ),
            variantKey, ctx
        ));
      }

      constexpr std::string_view repeatRateKey = "input.keyboard.repeat_rate";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.repeat-rate"),
          makeInputSlider(
              toDouble(input.value("input.keyboard.repeat_rate"), 25.0), 0.0, 1000.0, 1.0, true, ctx.scale,
              [set = ctx.set](double v) { set("input.keyboard.repeat_rate", std::format("{}", static_cast<int>(v))); },
              i18n::tr("settings.sliders.slow"), i18n::tr("settings.sliders.fast")
          ),
          repeatRateKey, ctx
      ));

      constexpr std::string_view repeatDelayKey = "input.keyboard.repeat_delay";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.repeat-delay"),
          makeInputSlider(
              toDouble(input.value("input.keyboard.repeat_delay"), 600.0), 0.0, 10000.0, 50.0, true, ctx.scale,
              [set = ctx.set](double v) { set("input.keyboard.repeat_delay", std::format("{}", static_cast<int>(v))); },
              i18n::tr("settings.sliders.short"), i18n::tr("settings.sliders.long")
          ),
          repeatDelayKey, ctx
      ));

      constexpr std::string_view numlockKey = "input.keyboard.numlock_toggle";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.numlock"),
          makeInputToggle(
              input.value("input.keyboard.numlock_toggle") == "true", ctx.scale,
              [set = ctx.set](bool checked) { set("input.keyboard.numlock_toggle", checked ? "true" : "false"); }
          ),
          numlockKey, ctx
      ));

      addChoiceRow(
          *body, "settings.input.keyboard.track-layout", "input.keyboard.track_layout", {"global", "window"},
          {i18n::tr("settings.input.keyboard.track-layout.global"),
           i18n::tr("settings.input.keyboard.track-layout.window")},
          ctx
      );

      // Caps Lock and Compose key behavior, from the two matching XKB option groups.
      for (const std::string_view group : {"caps", "compose"}) {
        const xkb::OptionGroup* optionGroup = xkb::findOptionGroup(catalog, group);
        if (optionGroup == nullptr || optionGroup->options.empty()) {
          continue;
        }
        constexpr std::string_view optionsKey = "input.keyboard.options";
        const std::string current = groupOptionValue(input.value("input.keyboard.options"), group);
        std::vector<std::string> labels = {i18n::tr("settings.input.keyboard.option-none")};
        std::vector<std::string> values = {""};
        std::optional<std::size_t> selected = 0;
        for (const xkb::OptionEntry& option : optionGroup->options) {
          if (option.name == current) {
            selected = values.size();
          }
          values.push_back(option.name);
          labels.push_back(option.description);
        }
        body->addChild(makeInputRow(
            optionGroup->description,
            makeInputSelect(
                std::move(labels), selected, ctx.scale,
                [set = ctx.set, group = std::string(group), values = std::move(values),
                 currentOptions = input.value("input.keyboard.options")](std::size_t index) {
                  set("input.keyboard.options", withGroupOption(currentOptions, group, values[index]));
                }
            ),
            optionsKey, ctx
        ));
      }
    }

    void addTouchpadCard(Flex& content, const SettingsInputContext& ctx) {
      const SettingsControl& input = *ctx.input;
      const bool hasTouchpad = std::ranges::any_of(input.devices(), [](const InputDevice& d) {
        return d.kind == InputDeviceKind::Touchpad;
      });
      if (!hasTouchpad) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.touchpad"), ctx.scale);
      addToggleRow(*body, "settings.input.touchpad.enabled", "input.touchpad.enabled", ctx);

      const auto boolRow = [&](std::string_view titleKey, std::string_view key) {
        body->addChild(makeInputRow(
            i18n::tr(titleKey),
            makeInputToggle(
                input.value(key) == "true", ctx.scale,
                [set = ctx.set, key = std::string(key)](bool checked) { set(key, checked ? "true" : "false"); }
            ),
            key, ctx
        ));
      };

      boolRow("settings.input.touchpad.tap", "input.touchpad.tap");
      boolRow("settings.input.touchpad.natural-scroll", "input.touchpad.natural_scroll");
      boolRow("settings.input.touchpad.disable-while-typing", "input.touchpad.disable_while_typing");
      boolRow("settings.input.touchpad.left-handed", "input.touchpad.left_handed");
      boolRow("settings.input.touchpad.natural-swipe", "input.touchpad.natural_swipe");
      boolRow("settings.input.touchpad.disable-on-external-mouse", "input.touchpad.disable_on_external_mouse");

      if (input.value("input.touchpad.tap") == "true") {
        boolRow("settings.input.touchpad.tap-and-drag", "input.touchpad.tap_and_drag");
        if (input.value("input.touchpad.tap_and_drag") != "false") {
          addChoiceRow(
              *body, "settings.input.touchpad.drag-lock", "input.touchpad.drag_lock", {"off", "timeout", "sticky"},
              {i18n::tr("settings.input.touchpad.drag-lock.off"), i18n::tr("settings.input.touchpad.drag-lock.timeout"),
               i18n::tr("settings.input.touchpad.drag-lock.sticky")},
              ctx
          );
        }
        addChoiceRow(
            *body, "settings.input.touchpad.tap-map", "input.touchpad.tap_button_map",
            {"left_right_middle", "left_middle_right"},
            {i18n::tr("settings.input.touchpad.tap-map.lrm"), i18n::tr("settings.input.touchpad.tap-map.lmr")}, ctx
        );
      }

      constexpr std::string_view clickKey = "input.touchpad.click_method";
      const std::vector<std::string> clickValues = {"button_areas", "clickfinger"};
      const std::vector<std::string> clickLabels = {
          i18n::tr("settings.input.touchpad.click-method.button-areas"),
          i18n::tr("settings.input.touchpad.click-method.clickfinger"),
      };
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.click-method"),
          makeInputSelect(
              clickLabels, indexOf(clickValues, input.value("input.touchpad.click_method")), ctx.scale,
              [set = ctx.set, clickValues](std::size_t index) {
                set("input.touchpad.click_method", clickValues[index]);
              }
          ),
          clickKey, ctx
      ));

      constexpr std::string_view accelKey = "input.touchpad.accel_profile";
      const std::vector<std::string> accelValues = {"flat", "adaptive"};
      const std::vector<std::string> accelLabels = {
          i18n::tr("settings.input.accel.flat"), i18n::tr("settings.input.accel.adaptive")
      };
      body->addChild(makeInputRow(
          i18n::tr("settings.input.accel-profile"),
          makeInputSelect(
              accelLabels, indexOf(accelValues, input.value("input.touchpad.accel_profile")), ctx.scale,
              [set = ctx.set, accelValues](std::size_t index) {
                set("input.touchpad.accel_profile", accelValues[index]);
              }
          ),
          accelKey, ctx
      ));

      constexpr std::string_view sensitivityKey = "input.touchpad.sensitivity";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.speed"),
          makeInputSlider(
              toDouble(input.value("input.touchpad.sensitivity"), 0.0), -1.0, 1.0, 0.05, false, ctx.scale,
              [set = ctx.set](double v) { set("input.touchpad.sensitivity", std::format("{:.2f}", v)); }
          ),
          sensitivityKey, ctx
      ));

      constexpr std::string_view scrollKey = "input.touchpad.scroll_factor";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.scroll-speed"),
          makeInputSlider(
              toDouble(input.value(scrollKey), 1.0), 0.1, 3.0, 0.05, false, ctx.scale,
              [set = ctx.set, key = std::string(scrollKey)](double v) { set(key, std::format("{:.2f}", v)); },
              i18n::tr("settings.sliders.slow"), i18n::tr("settings.sliders.fast")
          ),
          scrollKey, ctx
      ));
    }

    void addMouseCard(Flex& content, const SettingsInputContext& ctx) {
      const SettingsControl& input = *ctx.input;
      const bool hasMouse =
          std::ranges::any_of(input.devices(), [](const InputDevice& d) { return d.kind == InputDeviceKind::Mouse; });
      if (!hasMouse) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.mouse"), ctx.scale);

      constexpr std::string_view naturalKey = "input.mouse.natural_scroll";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.natural-scroll"),
          makeInputToggle(
              input.value("input.mouse.natural_scroll") == "true", ctx.scale,
              [set = ctx.set](bool checked) { set("input.mouse.natural_scroll", checked ? "true" : "false"); }
          ),
          naturalKey, ctx
      ));

      constexpr std::string_view leftHandedKey = "input.mouse.left_handed";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.left-handed"),
          makeInputToggle(
              input.value("input.mouse.left_handed") == "true", ctx.scale,
              [set = ctx.set](bool checked) { set("input.mouse.left_handed", checked ? "true" : "false"); }
          ),
          leftHandedKey, ctx
      ));

      constexpr std::string_view accelKey = "input.mouse.accel_profile";
      const std::vector<std::string> accelValues = {"flat", "adaptive"};
      const std::vector<std::string> accelLabels = {
          i18n::tr("settings.input.accel.flat"), i18n::tr("settings.input.accel.adaptive")
      };
      body->addChild(makeInputRow(
          i18n::tr("settings.input.accel-profile"),
          makeInputSelect(
              accelLabels, indexOf(accelValues, input.value("input.mouse.accel_profile")), ctx.scale,
              [set = ctx.set, accelValues](std::size_t index) { set("input.mouse.accel_profile", accelValues[index]); }
          ),
          accelKey, ctx
      ));

      constexpr std::string_view sensitivityKey = "input.mouse.sensitivity";
      body->addChild(makeInputRow(
          i18n::tr("settings.input.speed"),
          makeInputSlider(
              toDouble(input.value("input.mouse.sensitivity"), 0.0), -1.0, 1.0, 0.05, false, ctx.scale,
              [set = ctx.set](double v) { set("input.mouse.sensitivity", std::format("{:.2f}", v)); }
          ),
          sensitivityKey, ctx
      ));
    }

  } // namespace

  void addSettingsInput(Flex& content, const SettingsInputContext& ctx) {
    if (ctx.input == nullptr || !ctx.input->ready() || ctx.catalog == nullptr) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.input.unavailable"), ctx.scale));
      return;
    }
    if (ctx.pointer) {
      addPointerCard(content, ctx);
      addMouseCard(content, ctx);
      addTouchpadCard(content, ctx);
      addTabletCard(content, ctx);
      addTouchscreenCard(content, ctx);
    } else {
      addKeyboardCard(content, ctx);
    }
  }

} // namespace settings

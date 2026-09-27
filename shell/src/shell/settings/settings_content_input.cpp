#include "shell/settings/settings_content_input.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <charconv>
#include <cmath>
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

    std::unique_ptr<Flex>
    makeInputRow(std::string_view title, std::unique_ptr<Node> control, bool locked, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (locked) {
        row->addChild(
            ui::glyph({
                .glyph = "lock",
                .glyphSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::Secondary),
            })
        );
      }
      row->addChild(ui::spacer());
      row->addChild(std::move(control));
      return row;
    }

    std::unique_ptr<Node> makeInputSelect(
        std::vector<std::string> options, std::optional<std::size_t> selected, bool locked, float scale,
        std::function<void(std::size_t)> onSelect
    ) {
      return ui::select({
          .options = std::move(options),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = !locked,
          .width = kSelectWidth * scale,
          .height = Style::controlHeight * scale,
          .onSelectionChanged = [onSelect = std::move(onSelect)](std::size_t index, std::string_view) {
            onSelect(index);
          },
      });
    }

    std::unique_ptr<Node> makeInputToggle(bool checked, bool locked, float scale, std::function<void(bool)> onChange) {
      return ui::toggle({
          .checked = checked,
          .enabled = !locked,
          .scale = scale,
          .onChange = std::move(onChange),
      });
    }

    std::unique_ptr<Node> makeInputSlider(
        double value, double minValue, double maxValue, double step, bool integerValue, bool locked, float scale,
        std::function<void(double)> onCommit
    ) {
      return ui::slider({
          .minValue = minValue,
          .maxValue = maxValue,
          .step = step,
          .value = value,
          .enabled = !locked,
          .width = kSliderWidth * scale,
          .onValueChanged = [onCommit = std::move(onCommit), integerValue](double v) {
            onCommit(integerValue ? std::round(v) : v);
          },
      });
    }

    void addKeyboardCard(Flex& content, const SettingsInputContext& ctx) {
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.keyboard"), ctx.scale);
      const InputControl& input = *ctx.input;
      const xkb::Catalog& catalog = *ctx.catalog;

      const std::string currentLayout = input.value("input.keyboard.layout");
      const std::string currentVariant = input.value("input.keyboard.variant");
      const bool layoutLocked = input.locked("input.keyboard.layout");

      std::vector<std::string> layoutLabels;
      std::vector<std::string> layoutNames;
      std::optional<std::size_t> layoutSelected;
      for (const xkb::Layout& layout : catalog.layouts) {
        if (layout.name == currentLayout) {
          layoutSelected = layoutNames.size();
        }
        layoutNames.push_back(layout.name);
        layoutLabels.push_back(layout.description);
      }
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.layout"),
          makeInputSelect(
              std::move(layoutLabels), layoutSelected, layoutLocked, ctx.scale,
              [set = ctx.set, layoutNames = std::move(layoutNames)](std::size_t index) {
                set("input.keyboard.layout", layoutNames[index]);
                set("input.keyboard.variant", "");
              }
          ),
          layoutLocked, ctx.scale
      ));

      if (const xkb::Layout* layout = xkb::findLayout(catalog, currentLayout);
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
        const bool variantLocked = input.locked("input.keyboard.variant");
        body->addChild(makeInputRow(
            i18n::tr("settings.input.keyboard.variant"),
            makeInputSelect(
                std::move(variantLabels), variantSelected, variantLocked, ctx.scale,
                [set = ctx.set, variantNames = std::move(variantNames)](std::size_t index) {
                  set("input.keyboard.variant", variantNames[index]);
                }
            ),
            variantLocked, ctx.scale
        ));
      }

      const bool repeatRateLocked = input.locked("input.keyboard.repeat_rate");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.repeat-rate"),
          makeInputSlider(
              toDouble(input.value("input.keyboard.repeat_rate"), 25.0), 0.0, 1000.0, 1.0, true, repeatRateLocked,
              ctx.scale,
              [set = ctx.set](double v) { set("input.keyboard.repeat_rate", std::format("{}", static_cast<int>(v))); }
          ),
          repeatRateLocked, ctx.scale
      ));

      const bool repeatDelayLocked = input.locked("input.keyboard.repeat_delay");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.repeat-delay"),
          makeInputSlider(
              toDouble(input.value("input.keyboard.repeat_delay"), 600.0), 0.0, 10000.0, 50.0, true, repeatDelayLocked,
              ctx.scale,
              [set = ctx.set](double v) { set("input.keyboard.repeat_delay", std::format("{}", static_cast<int>(v))); }
          ),
          repeatDelayLocked, ctx.scale
      ));

      const bool numlockLocked = input.locked("input.keyboard.numlock_toggle");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.keyboard.numlock"),
          makeInputToggle(
              input.value("input.keyboard.numlock_toggle") == "true", numlockLocked, ctx.scale,
              [set = ctx.set](bool checked) { set("input.keyboard.numlock_toggle", checked ? "true" : "false"); }
          ),
          numlockLocked, ctx.scale
      ));

      // Caps Lock and Compose key behavior, from the two matching XKB option groups.
      for (const std::string_view group : {"caps", "compose"}) {
        const xkb::OptionGroup* optionGroup = xkb::findOptionGroup(catalog, group);
        if (optionGroup == nullptr || optionGroup->options.empty()) {
          continue;
        }
        const bool optionsLocked = input.locked("input.keyboard.options");
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
                std::move(labels), selected, optionsLocked, ctx.scale,
                [set = ctx.set, group = std::string(group), values = std::move(values),
                 currentOptions = input.value("input.keyboard.options")](std::size_t index) {
                  set("input.keyboard.options", withGroupOption(currentOptions, group, values[index]));
                }
            ),
            optionsLocked, ctx.scale
        ));
      }
    }

    void addTouchpadCard(Flex& content, const SettingsInputContext& ctx) {
      const InputControl& input = *ctx.input;
      const bool hasTouchpad = std::ranges::any_of(input.devices(), [](const InputDevice& d) {
        return d.kind == InputDeviceKind::Touchpad;
      });
      if (!hasTouchpad) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.touchpad"), ctx.scale);

      const auto boolRow = [&](std::string_view titleKey, std::string_view key) {
        const bool locked = input.locked(key);
        body->addChild(makeInputRow(
            i18n::tr(titleKey),
            makeInputToggle(
                input.value(key) == "true", locked, ctx.scale,
                [set = ctx.set, key = std::string(key)](bool checked) { set(key, checked ? "true" : "false"); }
            ),
            locked, ctx.scale
        ));
      };

      boolRow("settings.input.touchpad.tap", "input.touchpad.tap");
      boolRow("settings.input.touchpad.natural-scroll", "input.touchpad.natural_scroll");
      boolRow("settings.input.touchpad.disable-while-typing", "input.touchpad.disable_while_typing");
      boolRow("settings.input.touchpad.left-handed", "input.touchpad.left_handed");

      const bool clickLocked = input.locked("input.touchpad.click_method");
      const std::vector<std::string> clickValues = {"button_areas", "clickfinger"};
      const std::vector<std::string> clickLabels = {
          i18n::tr("settings.input.touchpad.click-method.button-areas"),
          i18n::tr("settings.input.touchpad.click-method.clickfinger"),
      };
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.click-method"),
          makeInputSelect(
              clickLabels, indexOf(clickValues, input.value("input.touchpad.click_method")), clickLocked, ctx.scale,
              [set = ctx.set, clickValues](std::size_t index) {
                set("input.touchpad.click_method", clickValues[index]);
              }
          ),
          clickLocked, ctx.scale
      ));

      const bool accelLocked = input.locked("input.touchpad.accel_profile");
      const std::vector<std::string> accelValues = {"flat", "adaptive"};
      const std::vector<std::string> accelLabels = {
          i18n::tr("settings.input.accel.flat"), i18n::tr("settings.input.accel.adaptive")
      };
      body->addChild(makeInputRow(
          i18n::tr("settings.input.accel-profile"),
          makeInputSelect(
              accelLabels, indexOf(accelValues, input.value("input.touchpad.accel_profile")), accelLocked, ctx.scale,
              [set = ctx.set, accelValues](std::size_t index) {
                set("input.touchpad.accel_profile", accelValues[index]);
              }
          ),
          accelLocked, ctx.scale
      ));

      const bool sensitivityLocked = input.locked("input.touchpad.sensitivity");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.speed"),
          makeInputSlider(
              toDouble(input.value("input.touchpad.sensitivity"), 0.0), -1.0, 1.0, 0.05, false, sensitivityLocked,
              ctx.scale, [set = ctx.set](double v) { set("input.touchpad.sensitivity", std::format("{:.2f}", v)); }
          ),
          sensitivityLocked, ctx.scale
      ));
    }

    void addMouseCard(Flex& content, const SettingsInputContext& ctx) {
      const InputControl& input = *ctx.input;
      const bool hasMouse =
          std::ranges::any_of(input.devices(), [](const InputDevice& d) { return d.kind == InputDeviceKind::Mouse; });
      if (!hasMouse) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.input.mouse"), ctx.scale);

      const bool naturalLocked = input.locked("input.mouse.natural_scroll");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.natural-scroll"),
          makeInputToggle(
              input.value("input.mouse.natural_scroll") == "true", naturalLocked, ctx.scale,
              [set = ctx.set](bool checked) { set("input.mouse.natural_scroll", checked ? "true" : "false"); }
          ),
          naturalLocked, ctx.scale
      ));

      const bool leftHandedLocked = input.locked("input.mouse.left_handed");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.touchpad.left-handed"),
          makeInputToggle(
              input.value("input.mouse.left_handed") == "true", leftHandedLocked, ctx.scale,
              [set = ctx.set](bool checked) { set("input.mouse.left_handed", checked ? "true" : "false"); }
          ),
          leftHandedLocked, ctx.scale
      ));

      const bool accelLocked = input.locked("input.mouse.accel_profile");
      const std::vector<std::string> accelValues = {"flat", "adaptive"};
      const std::vector<std::string> accelLabels = {
          i18n::tr("settings.input.accel.flat"), i18n::tr("settings.input.accel.adaptive")
      };
      body->addChild(makeInputRow(
          i18n::tr("settings.input.accel-profile"),
          makeInputSelect(
              accelLabels, indexOf(accelValues, input.value("input.mouse.accel_profile")), accelLocked, ctx.scale,
              [set = ctx.set, accelValues](std::size_t index) { set("input.mouse.accel_profile", accelValues[index]); }
          ),
          accelLocked, ctx.scale
      ));

      const bool sensitivityLocked = input.locked("input.mouse.sensitivity");
      body->addChild(makeInputRow(
          i18n::tr("settings.input.speed"),
          makeInputSlider(
              toDouble(input.value("input.mouse.sensitivity"), 0.0), -1.0, 1.0, 0.05, false, sensitivityLocked,
              ctx.scale, [set = ctx.set](double v) { set("input.mouse.sensitivity", std::format("{:.2f}", v)); }
          ),
          sensitivityLocked, ctx.scale
      ));
    }

  } // namespace

  void addSettingsInput(Flex& content, const SettingsInputContext& ctx) {
    if (ctx.input == nullptr || !ctx.input->ready() || ctx.catalog == nullptr) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.input.unavailable"), ctx.scale));
      return;
    }
    addKeyboardCard(content, ctx);
    addTouchpadCard(content, ctx);
    addMouseCard(content, ctx);
  }

} // namespace settings

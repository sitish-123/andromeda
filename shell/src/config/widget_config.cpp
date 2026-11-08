#include "config/widget_config.h"

#include "core/files/resource_paths.h"

#include "ui/style.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noctalia::config {

  std::optional<WidgetSettingValue> readWidgetSettingValue(const toml::node& node) {
    if (const auto* stringValue = node.as_string()) {
      return WidgetSettingValue{stringValue->get()};
    }
    if (const auto* intValue = node.as_integer()) {
      return WidgetSettingValue{intValue->get()};
    }
    if (const auto* floatValue = node.as_floating_point()) {
      return WidgetSettingValue{floatValue->get()};
    }
    if (const auto* boolValue = node.as_boolean()) {
      return WidgetSettingValue{boolValue->get()};
    }
    if (const auto* arrayValue = node.as_array()) {
      std::vector<std::string> strings;
      for (const auto& item : *arrayValue) {
        if (auto value = item.value<std::string>()) {
          strings.push_back(*value);
        }
      }
      // GCC 16+: -Wfree-nonheap-object false positive on variant move-return.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
      return WidgetSettingValue{std::move(strings)};
#pragma GCC diagnostic pop
    }
    if (const auto* tableValue = node.as_table()) {
      WidgetSettingStringMap strings;
      for (const auto& [key, valueNode] : *tableValue) {
        const auto value = valueNode.value<std::string>();
        if (!value.has_value()) {
          return std::nullopt;
        }
        strings.emplace(std::string(key.str()), *value);
      }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
      return WidgetSettingValue{std::move(strings)};
#pragma GCC diagnostic pop
    }
    return std::nullopt;
  }

  void seedBuiltinWidgets(Config& config) {
    auto seed = [&](const char* name, WidgetConfig wc) { config.widgets.emplace(name, std::move(wc)); };

    WidgetConfig cpu;
    cpu.type = "sysmon";
    cpu.settings["stat"] = std::string("cpu_usage");
    seed("cpu", std::move(cpu));

    WidgetConfig temp;
    temp.type = "sysmon";
    temp.settings["stat"] = std::string("cpu_temp");
    seed("temp", std::move(temp));

    WidgetConfig ram;
    ram.type = "sysmon";
    ram.settings["stat"] = std::string("ram_used");
    seed("ram", std::move(ram));

    WidgetConfig netTx;
    netTx.type = "sysmon";
    netTx.settings["stat"] = std::string("net_tx");
    seed("network_tx", std::move(netTx));

    WidgetConfig netRx;
    netRx.type = "sysmon";
    netRx.settings["stat"] = std::string("net_rx");
    seed("network_rx", std::move(netRx));

    WidgetConfig outputVolume;
    outputVolume.type = "volume";
    outputVolume.settings["device"] = std::string("output");
    seed("output_volume", std::move(outputVolume));

    WidgetConfig inputVolume;
    inputVolume.type = "volume";
    inputVolume.settings["device"] = std::string("input");
    seed("input_volume", std::move(inputVolume));

    WidgetConfig date;
    date.type = "clock";
    date.settings["format"] = std::string("{:%a %d %b}");
    seed("date", std::move(date));

    // Desktop Layouts, Windows: the Start button (the amark line glyph, board section 1) opens the launcher's apps grid.
    WidgetConfig start;
    start.type = "launcher";
    start.settings["custom_image"] = paths::assetPath("amark.svg").string();
    start.settings["custom_image_colorize"] = true;
    start.settings["icon_size"] = std::int64_t{24};
    start.tables["actions"]["left"] = "panel-toggle launcher apps";
    seed("start", std::move(start));

    // Desktop Layouts, Windows: 24 px icons in 40 px cells over a running bar, window previews on hover.
    WidgetConfig windowsTaskbar;
    windowsTaskbar.type = "taskbar";
    windowsTaskbar.settings["icon_scale"] = 1.5;
    windowsTaskbar.settings["cell_size"] = std::int64_t{40};
    windowsTaskbar.settings["running_bar"] = true;
    windowsTaskbar.settings["window_previews"] = true;
    windowsTaskbar.settings["combine_apps"] = true;
    seed("windows_taskbar", std::move(windowsTaskbar));

    // Desktop Layouts: the battery in the status cell is the symbol alone (boards desktop-layouts.html 1 and 7).
    WidgetConfig statusBattery;
    statusBattery.type = "battery";
    statusBattery.settings["display_mode"] = std::string("graphic");
    statusBattery.settings["show_label"] = false;
    seed("status_battery", std::move(statusBattery));

    // Desktop Layouts, Windows: time over date, right-aligned, in the taskbar's trailing group.
    WidgetConfig windowsClock;
    windowsClock.type = "clock";
    windowsClock.settings["format"] = std::string("{:%H:%M}\n{:%d/%m/%Y}");
    windowsClock.settings["stack_style"] = std::string("even");
    seed("windows_clock", std::move(windowsClock));

    // Desktop Layouts, macOS: the brand menu (session actions under the Andromeda mark), the bold app name, and the
    // one-line date and time.
    WidgetConfig brand;
    brand.type = "session";
    brand.settings["custom_image"] = paths::assetPath("amark.svg").string();
    brand.settings["custom_image_colorize"] = true;
    seed("brand", std::move(brand));

    WidgetConfig macosApp;
    macosApp.type = "window_menu";
    seed("macos_app", std::move(macosApp));

    WidgetConfig macosClock;
    macosClock.type = "clock";
    macosClock.settings["format"] = std::string("{:%a %-d %b %H:%M}");
    seed("macos_clock", std::move(macosClock));

    WidgetConfig activeWindow;
    activeWindow.type = "active_window";
    activeWindow.settings["max_length"] = 260.0;
    activeWindow.settings["min_length"] = 80.0;
    activeWindow.settings["icon_size"] = static_cast<double>(Style::fontSizeBody);
    activeWindow.settings["title_scroll"] = std::string("none");
    seed("active_window", std::move(activeWindow));

    WidgetConfig media;
    media.type = "media";
    media.settings["max_length"] = 220.0;
    media.settings["min_length"] = 80.0;
    media.settings["art_size"] = 16.0;
    media.settings["title_scroll"] = std::string("none");
    seed("media", std::move(media));

    WidgetConfig keyboardLayout;
    keyboardLayout.type = "keyboard_layout";
    keyboardLayout.settings["hide_when_single_layout"] = false;
    seed("keyboard_layout", std::move(keyboardLayout));

    WidgetConfig lockKeys;
    lockKeys.type = "lock_keys";
    lockKeys.settings["show_caps_lock"] = true;
    lockKeys.settings["show_num_lock"] = true;
    lockKeys.settings["show_scroll_lock"] = false;
    lockKeys.settings["hide_when_off"] = false;
    lockKeys.settings["display"] = std::string("short");
    seed("lock_keys", std::move(lockKeys));

    WidgetConfig spacer;
    spacer.type = "spacer";
    spacer.settings["interactive"] = false;
    seed("spacer", std::move(spacer));
  }

  WidgetConfig
  readBarWidgetConfig(std::string_view widgetName, const toml::table& entryTable, const Config& baseConfig) {
    const std::string name(widgetName);
    WidgetConfig wc;

    if (auto type = entryTable["type"].value<std::string>()) {
      wc.type = *type;
      if (auto it = baseConfig.widgets.find(name); it != baseConfig.widgets.end() && it->second.type == wc.type) {
        wc.settings = it->second.settings;
        wc.tables = it->second.tables;
      }
    } else if (auto it = baseConfig.widgets.find(name); it != baseConfig.widgets.end()) {
      wc = it->second;
    } else {
      wc.type = name;
    }

    for (const auto& [key, value] : entryTable) {
      if (key == "type") {
        continue;
      }
      if (const auto* tableValue = value.as_table()) {
        std::unordered_map<std::string, std::string> table;
        for (const auto& [tableKey, tableNode] : *tableValue) {
          if (auto parsed = tableNode.value<std::string>()) {
            table.emplace(std::string(tableKey.str()), *parsed);
          }
        }
        wc.tables[std::string(key.str())] = std::move(table);
      } else if (auto parsed = readWidgetSettingValue(value); parsed.has_value()) {
        wc.settings[std::string(key.str())] = std::move(*parsed);
      }
    }

    return wc;
  }

} // namespace noctalia::config

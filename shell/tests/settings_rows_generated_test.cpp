// Rows built from the config schema must equal the hand-written rows they replaced.
// tests/settings_rows_generated/hand_written_rows.tsv holds those rows, captured from the
// registry before any of them moved to the schema (`--write-fixture <file>` rewrites it).
#include "config/config_types.h"
#include "shell/settings/settings_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

  using namespace settings;

  // Pages whose plain rows come from the config schema. Listed here, not read from the RowSpecs, so a
  // dropped RowSpec shows up as a missing row.
  constexpr std::array<std::string_view, 1> kGeneratedPages{"control-center"};

  // i18n has no catalog loaded here, so tr(key) returns "!!key!!": unwrap it back to the key.
  std::string keyOf(std::string_view text) {
    if (text.size() >= 4 && text.starts_with("!!") && text.ends_with("!!")) {
      return std::string(text.substr(2, text.size() - 4));
    }
    return "=" + std::string(text);
  }

  std::string joined(const std::vector<std::string>& parts, char sep) {
    std::string out;
    for (const auto& part : parts) {
      if (!out.empty()) {
        out.push_back(sep);
      }
      out += part;
    }
    return out;
  }

  std::string optionsText(const std::vector<SelectOption>& options) {
    std::string out;
    for (const auto& option : options) {
      if (!out.empty()) {
        out.push_back('|');
      }
      out += option.value + "=" + keyOf(option.label);
    }
    return out;
  }

  // Kind and control detail of a plain row; empty kind for every other control.
  std::pair<std::string, std::string> describeControl(const SettingControl& control) {
    if (const auto* c = std::get_if<ToggleSetting>(&control)) {
      return {"toggle", std::format("checked={} enabled={}", c->checked, c->enabled)};
    }
    if (const auto* c = std::get_if<SliderSetting>(&control)) {
      return {
          "slider",
          std::format(
              "value={} min={} max={} step={} int={} suffix={} invert={} linkedCommit={}", c->value, c->minValue,
              c->maxValue, c->step, c->integerValue, c->valueSuffix, static_cast<int>(c->invertSlot),
              static_cast<bool>(c->linkedCommit)
          )
      };
    }
    if (const auto* c = std::get_if<StepperSetting>(&control)) {
      return {
          "stepper",
          std::format("value={} min={} max={} step={} suffix={}", c->value, c->minValue, c->maxValue, c->step,
                      c->valueSuffix)
      };
    }
    if (const auto* c = std::get_if<SelectSetting>(&control)) {
      return {
          "select",
          std::format(
              "selected={} options={} segmented={} type={} clearOnEmpty={} allowEmpty={} width={} linkedPath={} "
              "groupedCommit={}",
              c->selectedValue, optionsText(c->options), c->segmented, static_cast<int>(c->valueType),
              c->clearOnEmpty, c->allowEmptySelection, c->preferredWidth, joined(c->linkedPath, '.'),
              static_cast<bool>(c->groupedCommit)
          )
      };
    }
    if (const auto* c = std::get_if<TextSetting>(&control)) {
      return {
          "text",
          std::format(
              "value={} placeholder={} width={} browse={} exts={} fallback={}", c->value, keyOf(c->placeholder),
              c->width, static_cast<int>(c->browseMode), joined(c->browseFileExtensions, ','),
              c->browseFallbackDirectory
          )
      };
    }
    if (const auto* c = std::get_if<OptionalNumberSetting>(&control)) {
      return {
          "optional-number",
          std::format("value={} min={} max={} placeholder={}", c->value ? std::format("{}", *c->value) : "unset",
                      c->minValue, c->maxValue, keyOf(c->placeholder))
      };
    }
    if (const auto* c = std::get_if<OptionalStepperSetting>(&control)) {
      return {
          "optional-stepper",
          std::format(
              "value={} min={} max={} step={} fallback={} unset={} custom={}",
              c->value ? std::format("{}", *c->value) : "unset", c->minValue, c->maxValue, c->step, c->fallbackValue,
              keyOf(c->unsetLabel), keyOf(c->customLabel)
          )
      };
    }
    return {};
  }

  // Default config plus one bar, with every environment feature on, so conditional rows appear.
  std::vector<SettingEntry> registryUnderTest() {
    Config cfg;
    cfg.bars.push_back(BarConfig{});
    cfg.bars.back().name = "default";
    RegistryEnvironment env;
    env.niriBackdropSupported = true;
    env.niriOverviewTypeToLaunchSupported = true;
    env.andromedaOverviewTypeToLaunchSupported = true;
    env.niriOverviewCloseOnLaunchSupported = true;
    env.andromedaOverviewCloseOnLaunchSupported = true;
    env.screencopySupported = true;
    env.backgroundEffectBlurSupported = true;
    env.ddcutilAvailable = true;
    env.systemdUserManaged = true;
    env.gammaControlAvailable = true;
    env.greeterSyncAvailable = true;
    env.batteryAvailable = true;
    env.systemBatteryAvailable = true;
    return buildSettingsRegistry(cfg, &cfg.bars.front(), nullptr, env);
  }

  // One line per row in rendered order (settings_content groups rows like this): position on its page,
  // page key, group, path, kind, then for plain rows title key, subtitle key, advanced, visibleWhen,
  // search text and control detail. Other rows keep only their position, so moved plain rows cannot
  // pass custom rows of the same group.
  std::vector<std::string> renderedRows(const std::vector<SettingEntry>& entries) {
    const auto order = coalesceByGroupKey(entries.size(), [&](std::size_t i) {
      return barSettingContentSectionKey(entries[i]) + '\x1F' + entries[i].group;
    });
    std::unordered_map<std::string, std::size_t> positionOnPage;
    std::vector<std::string> rows;
    for (const std::size_t i : order) {
      const auto& entry = entries[i];
      const std::string page = barSettingContentSectionKey(entry);
      const std::size_t position = positionOnPage[page]++;
      auto [kind, detail] = describeControl(entry.control);
      std::string row = std::format("{}\t{}\t{}\t{}\t", position, page, entry.group, joined(entry.path, '.'));
      if (kind.empty()) {
        row += std::format("custom-{}", entry.control.index());
      } else {
        row += std::format(
            "{}\t{}\t{}\t{}\t{}\t{}\t{}", kind, keyOf(entry.title), keyOf(entry.subtitle), entry.advanced,
            static_cast<bool>(entry.visibleWhen), entry.searchText, detail
        );
      }
      rows.push_back(std::move(row));
    }
    return rows;
  }

} // namespace

int main(int argc, char** argv) {
  // Some placeholders and browse folders derive from the home and XDG directories.
  ::setenv("HOME", "/nonexistent-home", 1);
  ::setenv("XDG_CONFIG_HOME", "/nonexistent-home/.config", 1);
  ::unsetenv("XDG_PICTURES_DIR");
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.size() == 2 && args[0] == "--write-fixture") {
    std::ofstream out{std::string(args[1]), std::ios::trunc};
    for (const auto& row : renderedRows(registryUnderTest())) {
      out << row << '\n';
    }
    return out ? 0 : 1;
  }
  if (args.size() != 2) {
    std::println(stderr, "usage: settings_rows_generated_test <fixture.tsv> <en.json> | --write-fixture <file>");
    return 1;
  }

  std::vector<std::string> fixture;
  {
    std::ifstream in{std::string(args[0])};
    for (std::string line; std::getline(in, line);) {
      fixture.push_back(line);
    }
  }
  nlohmann::json catalog;
  {
    std::ifstream in{std::string(args[1])};
    catalog = nlohmann::json::parse(in);
  }
  const auto entries = registryUnderTest();
  const auto current = renderedRows(entries);

  bool ok = !fixture.empty();
  for (const std::string_view page : kGeneratedPages) {
    const auto onPage = [&](const std::string& row) {
      const std::size_t start = row.find('\t') + 1;
      return row.substr(start, row.find('\t', start) - start) == page;
    };
    std::vector<std::string> want;
    std::vector<std::string> got;
    std::ranges::copy_if(fixture, std::back_inserter(want), onPage);
    std::ranges::copy_if(current, std::back_inserter(got), onPage);
    if (want.empty() || want != got) {
      ok = false;
      std::println(stderr, "page {}: rows differ from the hand-written rows", page);
      for (const auto& row : want) {
        if (!std::ranges::contains(got, row)) {
          std::println(stderr, "  missing: {}", row);
        }
      }
      for (const auto& row : got) {
        if (!std::ranges::contains(want, row)) {
          std::println(stderr, "  extra:   {}", row);
        }
      }
    }
  }

  for (const auto& entry : entries) {
    if (!std::ranges::contains(kGeneratedPages, barSettingContentSectionKey(entry))
        || describeControl(entry.control).first.empty()) {
      continue;
    }
    std::vector<std::string> keys{keyOf(entry.title), keyOf(entry.subtitle)};
    if (const auto* select = std::get_if<SelectSetting>(&entry.control)) {
      for (const auto& option : select->options) {
        keys.push_back(keyOf(option.label));
      }
    }
    for (std::string key : keys) {
      std::ranges::replace(key, '.', '/');
      if (!catalog.contains(nlohmann::json::json_pointer("/" + key))) {
        ok = false;
        std::println(stderr, "{}: no en.json key {}", joined(entry.path, '.'), key);
      }
    }
  }
  return ok ? 0 : 1;
}

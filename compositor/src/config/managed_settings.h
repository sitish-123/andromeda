#pragma once

#include "config/config.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace umbriel {

  // The settings a desktop shell may change at runtime, named by their dotted config keys ("input.touchpad.tap",
  // "appearance.border_width"). Values cross the protocol as plain text: "true", "0.3", "25", "adaptive", "us,de".
  // An empty value means unset, which restores the value from config.toml or the built-in default.

  [[nodiscard]] std::span<const std::string_view> managedSettingKeys();

  // Effective value of `key` in `config` as protocol text; empty when unset or when `key` is not a managed setting.
  [[nodiscard]] std::string managedSettingValue(const Config& config, std::string_view key);

  // A validated value ready to store, or std::monostate to remove the key.
  using ManagedSettingValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

  struct ManagedSettingParse {
    std::optional<ManagedSettingValue> value;
    std::string error; // set when value is empty
  };

  // Validates `text` against `key`'s type and range.
  [[nodiscard]] ManagedSettingParse parseManagedSetting(std::string_view key, std::string_view text);

  // The file Umbriel writes these settings to, next to `configRoot`. It is loaded after config.toml and its includes.
  [[nodiscard]] std::filesystem::path managedSettingsFile(const std::filesystem::path& configRoot);

  // Sets or removes `key` in the generated settings `file`. Returns false on I/O error.
  bool saveManagedSetting(const std::filesystem::path& file, std::string_view key, const ManagedSettingValue& value);

  // Same edit on a TOML document, for tests.
  [[nodiscard]] std::string
  editManagedSetting(std::string_view existing, std::string_view key, const ManagedSettingValue& value);

  // Binds `chord` to `action` in the settings file's [keybinds], or removes the entry when `action` is empty so
  // config.toml or the built-in set applies again. kUnboundAction ("none") unbinds the chord. Returns false on I/O error.
  bool saveManagedKeybind(const std::filesystem::path& file, std::string_view chord, std::string_view action);

  // The [keybinds] entries of a settings document as written, chord text to action text; table-form entries report
  // their `action`.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> documentKeybinds(std::string_view document);

  // True when the TOML document `root` sets `key`.
  [[nodiscard]] bool documentSetsKey(std::string_view root, std::string_view key);

} // namespace umbriel

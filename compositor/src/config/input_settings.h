#pragma once

#include "config/config.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace umbriel {

  // The input settings a desktop shell may change at runtime, named by their dotted config keys
  // ("input.touchpad.tap"). Values cross the protocol as plain text: "true", "0.3", "25", "adaptive", "us,de".
  // An empty value means unset, which restores the device or built-in default.

  [[nodiscard]] std::span<const std::string_view> inputSettingKeys();

  // Effective value of `key` in `config` as protocol text; empty when unset or when `key` is not an input setting.
  [[nodiscard]] std::string inputSettingValue(const Config& config, std::string_view key);

  // A validated value ready to store, or std::monostate to remove the key.
  using InputSettingValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

  struct InputSettingParse {
    std::optional<InputSettingValue> value;
    std::string error; // set when value is empty
  };

  // Validates `text` against `key`'s type and range.
  [[nodiscard]] InputSettingParse parseInputSetting(std::string_view key, std::string_view text);

  // Sets or removes `key` in the generated input.toml `file`. Returns false on I/O error.
  bool saveInputSetting(const std::filesystem::path& file, std::string_view key, const InputSettingValue& value);

  // Same edit on a TOML document, for tests.
  [[nodiscard]] std::string
  editInputSetting(std::string_view existing, std::string_view key, const InputSettingValue& value);

  // True when the TOML document `root` (the user's own config.toml, without its includes) sets `key`, which wins over
  // the generated file.
  [[nodiscard]] bool documentSetsKey(std::string_view root, std::string_view key);

} // namespace umbriel

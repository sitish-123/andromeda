#pragma once

#include "core/files/file_watcher.h"

#include <filesystem>
#include <functional>
#include <string>

namespace noctalia::theme::dev_tokens {

  // Parses a design-tokens.toml and writes its values into Tokens::. All-or-nothing: on a parse error,
  // an unknown key, or a type mismatch it returns false, sets `error`, and changes nothing.
  bool applyFile(const std::filesystem::path& path, std::string& error);

  // ANDROMEDA_DEV_TOKENS: the path to design-tokens.toml, or empty when dev mode is off.
  [[nodiscard]] std::filesystem::path envPath();

  // Applies the env file once (call before the config loads) and returns false when dev mode is off.
  bool applyFromEnv();

  // Watches the env file; each valid change calls `onApplied`.
  void watchEnv(FileWatcher& watcher, std::function<void()> onApplied);

} // namespace noctalia::theme::dev_tokens

#pragma once

#include <filesystem>
#include <string>

namespace greeter::config {

  // Andromeda greeter-mode config (outputs, cursor, keyboard) in andromeda config syntax, built from greeter.toml and
  // sync.toml in stateDir (greeter.toml wins). Empty stateDir uses the default synced data dir.
  [[nodiscard]] std::string andromedaGreeterConfig(const std::filesystem::path& stateDir);

} // namespace greeter::config

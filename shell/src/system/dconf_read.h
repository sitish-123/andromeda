#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Reads dconf GVDB databases directly: GSettings would start the dconf worker thread and a session-bus connection.
namespace dconf {

  // A string value at `key` (a full dconf path) in one database file; nullopt when absent, not a string, or the
  // file is malformed.
  std::optional<std::string> readStringFromDatabase(const std::filesystem::path& database, std::string_view key);

  // A string value at `key` from the databases in the dconf profile, in profile order. Locks are not honoured.
  std::optional<std::string> readString(std::string_view key);

  // The writable user database of the profile; nullopt when the profile has none.
  std::optional<std::filesystem::path> userDatabasePath();

} // namespace dconf

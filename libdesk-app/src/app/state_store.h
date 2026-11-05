#pragma once

#include "core/timer_manager.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// Per-app state that survives a crash: one JSON object in $XDG_STATE_HOME/andromeda/<app-id>/state.json.
// A change is written 500 ms after the last change, or at once by flush(); writes are atomic (tmp, fsync, rename).
class StateStore {
public:
  explicit StateStore(std::filesystem::path file);
  ~StateStore();

  StateStore(const StateStore&) = delete;
  StateStore& operator=(const StateStore&) = delete;

  [[nodiscard]] static std::filesystem::path pathFor(std::string_view appId);

  // A missing or corrupt file leaves the store empty; a corrupt file is kept as state.json.bad.
  void load();
  [[nodiscard]] const nlohmann::json& data() const noexcept { return m_data; }
  // Value under a JSON pointer ("/window/width"), or fallback when absent or of another type.
  template <typename T> [[nodiscard]] T get(std::string_view pointer, T fallback) const {
    const nlohmann::json::json_pointer ptr{std::string(pointer)};
    if (!m_data.contains(ptr)) {
      return fallback;
    }
    try {
      return m_data.at(ptr).get<T>();
    } catch (const nlohmann::json::exception&) {
      return fallback;
    }
  }
  void set(std::string_view pointer, nlohmann::json value);

  bool flush();
  [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_file; }

private:
  std::filesystem::path m_file;
  nlohmann::json m_data = nlohmann::json::object();
  Timer m_saveTimer;
  bool m_dirty = false;
};

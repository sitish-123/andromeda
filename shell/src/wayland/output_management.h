#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

struct wl_registry;
struct zwlr_output_configuration_head_v1;
struct zwlr_output_configuration_v1;
struct zwlr_output_head_v1;
struct zwlr_output_manager_v1;
struct zwlr_output_mode_v1;

struct OutputMode {
  zwlr_output_mode_v1* handle = nullptr;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t refreshMhz = 0;
  bool preferred = false;
};

struct OutputHead {
  zwlr_output_head_v1* handle = nullptr;
  std::string name;
  std::string description;
  std::string make;
  std::string model;
  std::string serialNumber;
  std::int32_t physicalWidthMm = 0;
  std::int32_t physicalHeightMm = 0;
  std::vector<OutputMode> modes;
  zwlr_output_mode_v1* currentMode = nullptr;
  bool enabled = false;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t transform = 0; // wl_output_transform
  double scale = 1.0;
  bool adaptiveSync = false;
  bool adaptiveSyncReported = false; // the compositor sent adaptive_sync (protocol v4)
};

struct OutputHeadConfig {
  std::string name;
  bool enabled = true;
  zwlr_output_mode_v1* mode = nullptr;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t transform = 0;
  double scale = 1.0;
  bool adaptiveSync = false;

  bool operator==(const OutputHeadConfig&) const = default;
};

enum class OutputApplyResult : std::uint8_t {
  Succeeded,
  Failed,
  Cancelled,
};

// A private zwlr_output_manager_v1 binding that exists only while a client needs full output state, so the shell
// pays nothing for it the rest of the time.
class OutputManagement {
public:
  using ChangeCallback = std::function<void()>;
  using ResultCallback = std::function<void(OutputApplyResult)>;

  OutputManagement(wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange);
  ~OutputManagement();

  OutputManagement(const OutputManagement&) = delete;
  OutputManagement& operator=(const OutputManagement&) = delete;

  // Empty until the compositor's first done event.
  [[nodiscard]] const std::vector<OutputHead>& heads() const noexcept { return m_heads; }
  [[nodiscard]] bool ready() const noexcept { return m_ready; }
  [[nodiscard]] bool busy() const noexcept { return m_configuration != nullptr; }

  // Heads missing from `config` keep their current state, because the protocol rejects a configuration that omits a
  // head. Returns false when not ready, when another configuration is in flight, or when the manager is gone.
  bool apply(std::span<const OutputHeadConfig> config, bool testOnly, ResultCallback done);

  [[nodiscard]] static OutputHeadConfig currentConfig(const OutputHead& head);

private:
  friend struct OutputManagementListeners;

  OutputHead* findHead(zwlr_output_head_v1* handle);
  OutputMode* findMode(zwlr_output_mode_v1* handle);
  void finishConfiguration(OutputApplyResult result);
  void destroyConfiguration();
  void releaseAll();

  zwlr_output_manager_v1* m_manager = nullptr;
  std::uint32_t m_version = 0;
  std::vector<OutputHead> m_heads;
  std::uint32_t m_serial = 0;
  bool m_ready = false;
  ChangeCallback m_onChange;
  zwlr_output_configuration_v1* m_configuration = nullptr;
  std::vector<zwlr_output_configuration_head_v1*> m_configurationHeads;
  ResultCallback m_resultCallback;
};

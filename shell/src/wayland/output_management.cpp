#include "wayland/output_management.h"

#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <utility>
#include <wayland-client.h>

struct OutputManagementListeners {
  static OutputManagement& self(void* data) { return *static_cast<OutputManagement*>(data); }

  static void modeSize(void* data, zwlr_output_mode_v1* mode, std::int32_t width, std::int32_t height) {
    if (OutputMode* m = self(data).findMode(mode)) {
      m->width = width;
      m->height = height;
    }
  }

  static void modeRefresh(void* data, zwlr_output_mode_v1* mode, std::int32_t refreshMhz) {
    if (OutputMode* m = self(data).findMode(mode)) {
      m->refreshMhz = refreshMhz;
    }
  }

  static void modePreferred(void* data, zwlr_output_mode_v1* mode) {
    if (OutputMode* m = self(data).findMode(mode)) {
      m->preferred = true;
    }
  }

  static void modeFinished(void* data, zwlr_output_mode_v1* mode) {
    OutputManagement& om = self(data);
    for (OutputHead& head : om.m_heads) {
      if (std::erase_if(head.modes, [mode](const OutputMode& m) { return m.handle == mode; }) > 0) {
        if (head.currentMode == mode) {
          head.currentMode = nullptr;
        }
        break;
      }
    }
    zwlr_output_mode_v1_release(mode);
  }

  static constexpr zwlr_output_mode_v1_listener kMode = {
      .size = modeSize,
      .refresh = modeRefresh,
      .preferred = modePreferred,
      .finished = modeFinished,
  };

  template <auto Member, typename T> static void setField(void* data, zwlr_output_head_v1* head, T value) {
    if (OutputHead* h = self(data).findHead(head)) {
      h->*Member = value;
    }
  }

  static void headString(std::string OutputHead::* member, void* data, zwlr_output_head_v1* head, const char* value) {
    if (OutputHead* h = self(data).findHead(head); h != nullptr && value != nullptr) {
      h->*member = value;
    }
  }

  static void headName(void* data, zwlr_output_head_v1* head, const char* v) {
    headString(&OutputHead::name, data, head, v);
  }
  static void headDescription(void* data, zwlr_output_head_v1* head, const char* v) {
    headString(&OutputHead::description, data, head, v);
  }
  static void headMake(void* data, zwlr_output_head_v1* head, const char* v) {
    headString(&OutputHead::make, data, head, v);
  }
  static void headModel(void* data, zwlr_output_head_v1* head, const char* v) {
    headString(&OutputHead::model, data, head, v);
  }
  static void headSerialNumber(void* data, zwlr_output_head_v1* head, const char* v) {
    headString(&OutputHead::serialNumber, data, head, v);
  }

  static void headPhysicalSize(void* data, zwlr_output_head_v1* head, std::int32_t width, std::int32_t height) {
    if (OutputHead* h = self(data).findHead(head)) {
      h->physicalWidthMm = width;
      h->physicalHeightMm = height;
    }
  }

  static void headMode(void* data, zwlr_output_head_v1* head, zwlr_output_mode_v1* mode) {
    if (OutputHead* h = self(data).findHead(head)) {
      h->modes.push_back(OutputMode{.handle = mode});
    }
    zwlr_output_mode_v1_add_listener(mode, &kMode, data);
  }

  static void headEnabled(void* data, zwlr_output_head_v1* head, std::int32_t enabled) {
    setField<&OutputHead::enabled>(data, head, enabled != 0);
  }

  static void headCurrentMode(void* data, zwlr_output_head_v1* head, zwlr_output_mode_v1* mode) {
    setField<&OutputHead::currentMode>(data, head, mode);
  }

  static void headPosition(void* data, zwlr_output_head_v1* head, std::int32_t x, std::int32_t y) {
    if (OutputHead* h = self(data).findHead(head)) {
      h->x = x;
      h->y = y;
    }
  }

  static void headTransform(void* data, zwlr_output_head_v1* head, std::int32_t transform) {
    setField<&OutputHead::transform>(data, head, transform);
  }

  static void headScale(void* data, zwlr_output_head_v1* head, wl_fixed_t scale) {
    setField<&OutputHead::scale>(data, head, wl_fixed_to_double(scale));
  }

  static void headAdaptiveSync(void* data, zwlr_output_head_v1* head, std::uint32_t state) {
    if (OutputHead* h = self(data).findHead(head)) {
      h->adaptiveSync = state == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED;
      h->adaptiveSyncReported = true;
    }
  }

  static void headFinished(void* data, zwlr_output_head_v1* head) {
    OutputManagement& om = self(data);
    if (OutputHead* h = om.findHead(head)) {
      for (OutputMode& mode : h->modes) {
        zwlr_output_mode_v1_release(mode.handle);
      }
    }
    std::erase_if(om.m_heads, [head](const OutputHead& h) { return h.handle == head; });
    zwlr_output_head_v1_release(head);
  }

  static constexpr zwlr_output_head_v1_listener kHead = {
      .name = headName,
      .description = headDescription,
      .physical_size = headPhysicalSize,
      .mode = headMode,
      .enabled = headEnabled,
      .current_mode = headCurrentMode,
      .position = headPosition,
      .transform = headTransform,
      .scale = headScale,
      .finished = headFinished,
      .make = headMake,
      .model = headModel,
      .serial_number = headSerialNumber,
      .adaptive_sync = headAdaptiveSync,
  };

  static void managerHead(void* data, zwlr_output_manager_v1* /*manager*/, zwlr_output_head_v1* head) {
    self(data).m_heads.push_back(OutputHead{.handle = head});
    zwlr_output_head_v1_add_listener(head, &kHead, data);
  }

  static void managerDone(void* data, zwlr_output_manager_v1* /*manager*/, std::uint32_t serial) {
    OutputManagement& om = self(data);
    om.m_serial = serial;
    om.m_ready = true;
    if (om.m_onChange) {
      om.m_onChange();
    }
  }

  static void managerFinished(void* data, zwlr_output_manager_v1* /*manager*/) {
    OutputManagement& om = self(data);
    om.releaseAll();
    om.m_ready = false;
    if (om.m_onChange) {
      om.m_onChange();
    }
  }

  static constexpr zwlr_output_manager_v1_listener kManager = {
      .head = managerHead,
      .done = managerDone,
      .finished = managerFinished,
  };

  static void configSucceeded(void* data, zwlr_output_configuration_v1* /*config*/) {
    self(data).finishConfiguration(OutputApplyResult::Succeeded);
  }
  static void configFailed(void* data, zwlr_output_configuration_v1* /*config*/) {
    self(data).finishConfiguration(OutputApplyResult::Failed);
  }
  static void configCancelled(void* data, zwlr_output_configuration_v1* /*config*/) {
    self(data).finishConfiguration(OutputApplyResult::Cancelled);
  }

  static constexpr zwlr_output_configuration_v1_listener kConfiguration = {
      .succeeded = configSucceeded,
      .failed = configFailed,
      .cancelled = configCancelled,
  };
};

OutputManagement::OutputManagement(
    wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange
)
    : m_version(version), m_onChange(std::move(onChange)) {
  if (registry == nullptr || globalName == 0) {
    return;
  }
  m_manager = static_cast<zwlr_output_manager_v1*>(
      wl_registry_bind(registry, globalName, &zwlr_output_manager_v1_interface, version)
  );
  zwlr_output_manager_v1_add_listener(m_manager, &OutputManagementListeners::kManager, this);
}

OutputManagement::~OutputManagement() {
  if (m_configuration != nullptr) {
    destroyConfiguration();
  }
  if (m_manager != nullptr) {
    zwlr_output_manager_v1_stop(m_manager);
  }
  releaseAll();
}

void OutputManagement::releaseAll() {
  for (OutputHead& head : m_heads) {
    for (OutputMode& mode : head.modes) {
      zwlr_output_mode_v1_release(mode.handle);
    }
    zwlr_output_head_v1_release(head.handle);
  }
  m_heads.clear();
  if (m_manager != nullptr) {
    zwlr_output_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

OutputHead* OutputManagement::findHead(zwlr_output_head_v1* handle) {
  const auto it = std::ranges::find(m_heads, handle, &OutputHead::handle);
  return it != m_heads.end() ? &*it : nullptr;
}

OutputMode* OutputManagement::findMode(zwlr_output_mode_v1* handle) {
  for (OutputHead& head : m_heads) {
    const auto it = std::ranges::find(head.modes, handle, &OutputMode::handle);
    if (it != head.modes.end()) {
      return &*it;
    }
  }
  return nullptr;
}

OutputHeadConfig OutputManagement::currentConfig(const OutputHead& head) {
  return OutputHeadConfig{
      .name = head.name,
      .enabled = head.enabled,
      .mode = head.currentMode,
      .x = head.x,
      .y = head.y,
      .transform = head.transform,
      .scale = head.scale,
      .adaptiveSync = head.adaptiveSync,
  };
}

bool OutputManagement::apply(std::span<const OutputHeadConfig> config, bool testOnly, ResultCallback done) {
  if (m_manager == nullptr || !m_ready || m_configuration != nullptr) {
    return false;
  }
  m_configuration = zwlr_output_manager_v1_create_configuration(m_manager, m_serial);
  zwlr_output_configuration_v1_add_listener(m_configuration, &OutputManagementListeners::kConfiguration, this);
  m_resultCallback = std::move(done);

  for (const OutputHead& head : m_heads) {
    const auto it = std::ranges::find(config, head.name, &OutputHeadConfig::name);
    const OutputHeadConfig wanted = it != config.end() ? *it : currentConfig(head);
    if (!wanted.enabled) {
      zwlr_output_configuration_v1_disable_head(m_configuration, head.handle);
      continue;
    }
    zwlr_output_configuration_head_v1* ch = zwlr_output_configuration_v1_enable_head(m_configuration, head.handle);
    m_configurationHeads.push_back(ch);
    if (wanted.mode != nullptr) {
      zwlr_output_configuration_head_v1_set_mode(ch, wanted.mode);
    }
    zwlr_output_configuration_head_v1_set_position(ch, wanted.x, wanted.y);
    zwlr_output_configuration_head_v1_set_transform(ch, wanted.transform);
    zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(wanted.scale));
    if (m_version >= ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_SET_ADAPTIVE_SYNC_SINCE_VERSION && head.adaptiveSyncReported) {
      zwlr_output_configuration_head_v1_set_adaptive_sync(
          ch,
          wanted.adaptiveSync ? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
                              : ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED
      );
    }
  }

  if (testOnly) {
    zwlr_output_configuration_v1_test(m_configuration);
  } else {
    zwlr_output_configuration_v1_apply(m_configuration);
  }
  return true;
}

void OutputManagement::destroyConfiguration() {
  // Configuration heads have no destructor request; their proxies are freed client-side only.
  for (zwlr_output_configuration_head_v1* head : m_configurationHeads) {
    zwlr_output_configuration_head_v1_destroy(head);
  }
  m_configurationHeads.clear();
  zwlr_output_configuration_v1_destroy(m_configuration);
  m_configuration = nullptr;
}

void OutputManagement::finishConfiguration(OutputApplyResult result) {
  destroyConfiguration();
  if (ResultCallback done = std::exchange(m_resultCallback, {})) {
    done(result);
  }
}

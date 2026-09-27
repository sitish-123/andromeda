#include "server/desktop_output_manager.h"

#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "desktop-unstable-v1-protocol.h"
#include "output/display_store.h"
#include "output/output.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

    DesktopOutputManager* managerFrom(wl_resource* resource) {
      return static_cast<DesktopOutputManager*>(wl_resource_get_user_data(resource));
    }

    Output* outputNamed(const Server& server, std::string_view connector) {
      for (const auto& output : server.outputs()) {
        if (connector == output->wlr()->name) {
          return output.get();
        }
      }
      return nullptr;
    }

    enum class PropertyKind : uint8_t { Bool, Int, Real, Choice, Workspaces };

    constexpr std::array<std::string_view, 3> kVrr = {"disabled", "always", "fullscreen"};
    constexpr std::array<std::string_view, 4> kHdr = {"off", "on", "auto", "fullscreen"};
    constexpr std::array<std::string_view, 2> kAxis = {"vertical", "horizontal"};

    // Enumerators and choice arrays share their order.
    std::string property(const OutputRule& rule, std::string_view key) {
      if (key == "vrr") {
        return std::string(kVrr[static_cast<size_t>(rule.vrr)]);
      }
      if (key == "hdr") {
        return std::string(kHdr[static_cast<size_t>(rule.hdr)]);
      }
      if (key == "sdr_white") {
        return std::format("{:g}", rule.sdrWhite);
      }
      if (key == "tearing") {
        return rule.allowTearing ? "true" : "false";
      }
      if (key == "workspaces") {
        if (const auto* count = rule.workspaces ? std::get_if<size_t>(&*rule.workspaces) : nullptr) {
          return std::to_string(*count);
        }
        return rule.workspaces ? "named" : "dynamic";
      }
      if (key == "min_workspaces") {
        return std::to_string(rule.minWorkspaces);
      }
      if (key == "cyclic_workspaces") {
        return rule.cyclicWorkspaces ? "true" : "false";
      }
      return std::string(kAxis[static_cast<size_t>(rule.workspaceAxis)]);
    }

    struct PropertySpec {
      std::string_view key;
      PropertyKind kind;
      double min = 0.0;
      double max = 0.0;
      std::span<const std::string_view> choices{};
    };

    // The ranges repeat the [output] reader's own.
    constexpr std::array kProperties = {
        PropertySpec{"vrr", PropertyKind::Choice, 0, 0, kVrr},
        PropertySpec{"hdr", PropertyKind::Choice, 0, 0, kHdr},
        PropertySpec{"sdr_white", PropertyKind::Real, 80, 1000},
        PropertySpec{"tearing", PropertyKind::Bool},
        PropertySpec{"workspaces", PropertyKind::Workspaces, 1, 64},
        PropertySpec{"min_workspaces", PropertyKind::Int, 1, 64},
        PropertySpec{"cyclic_workspaces", PropertyKind::Bool},
        PropertySpec{"workspace_axis", PropertyKind::Choice, 0, 0, kAxis},
    };

    std::optional<SavedProperty> parseProperty(const PropertySpec& spec, std::string_view text) {
      if (text.empty()) {
        return SavedProperty{std::monostate{}};
      }
      switch (spec.kind) {
      case PropertyKind::Bool:
        if (text == "true" || text == "false") {
          return SavedProperty{text == "true"};
        }
        return std::nullopt;
      case PropertyKind::Workspaces:
        if (text == "dynamic") {
          return SavedProperty{std::string(text)};
        }
        [[fallthrough]];
      case PropertyKind::Int: {
        int64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < spec.min || value > spec.max) {
          return std::nullopt;
        }
        return SavedProperty{value};
      }
      case PropertyKind::Real: {
        double value = 0.0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < spec.min || value > spec.max) {
          return std::nullopt;
        }
        return SavedProperty{value};
      }
      case PropertyKind::Choice:
        if (std::ranges::find(spec.choices, text) != spec.choices.end()) {
          return SavedProperty{std::string(text)};
        }
        return std::nullopt;
      }
      return std::nullopt;
    }

  } // namespace

  DesktopOutputManager::DesktopOutputManager(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &dsk_output_manager_v1_interface, kVersion, this, bind);
  }

  DesktopOutputManager::~DesktopOutputManager() {
    for (wl_resource* resource : m_resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DesktopOutputManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<DesktopOutputManager*>(data);
    wl_resource* resource = wl_resource_create(client, &dsk_output_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kImplementation, self, handleResourceDestroyed);
    self->m_resources.push_back(resource);
    for (const auto& output : self->m_server.outputs()) {
      sendState(resource, *output);
      sendProperties(resource, *output);
    }
    dsk_output_manager_v1_send_done(resource);
  }

  void
  DesktopOutputManager::handleSetMirror(wl_client*, wl_resource* resource, const char* target, const char* source) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      self->apply(resource, target, source);
    }
  }

  void DesktopOutputManager::handleClearMirror(wl_client*, wl_resource* resource, const char* target) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      self->apply(resource, target, {});
    }
  }

  void DesktopOutputManager::handleSetProperty(
      wl_client*, wl_resource* resource, const char* target, const char* key, const char* value
  ) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      self->setProperty(resource, target, key, value);
    }
  }

  void DesktopOutputManager::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void DesktopOutputManager::handleResourceDestroyed(wl_resource* resource) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      std::erase(self->m_resources, resource);
    }
  }

  void DesktopOutputManager::apply(wl_resource* resource, std::string_view target, std::string_view source) {
    const std::string targetName(target);
    Output* targetOutput = outputNamed(m_server, target);
    Output* sourceOutput = source.empty() ? nullptr : outputNamed(m_server, source);
    const char* reason = nullptr;
    if (targetOutput == nullptr || (!source.empty() && sourceOutput == nullptr)) {
      reason = "unknown output";
    } else if (sourceOutput == targetOutput) {
      reason = "an output cannot mirror itself";
    } else if (sourceOutput != nullptr && (sourceOutput->mirrorSource() != nullptr || !sourceOutput->onDesktop())) {
      reason = "the source is not on the desktop";
    } else if (sourceOutput != nullptr && std::ranges::any_of(m_server.outputs(), [&](const auto& output) {
                 return output->mirrorSource() == targetOutput;
               })) {
      reason = "the target is being mirrored";
    }
    if (reason != nullptr) {
      dsk_output_manager_v1_send_failed(resource, targetName.c_str(), reason);
      return;
    }
    m_server.setOutputMirror(*targetOutput, sourceOutput);
  }

  void DesktopOutputManager::setProperty(
      wl_resource* resource, std::string_view target, std::string_view key, std::string_view value
  ) {
    const std::string targetName(target);
    const auto reject = [&](const std::string& reason) {
      dsk_output_manager_v1_send_failed(resource, targetName.c_str(), reason.c_str());
    };
    Output* output = outputNamed(m_server, target);
    if (output == nullptr) {
      reject("unknown output");
      return;
    }
    const auto spec = std::ranges::find(kProperties, key, &PropertySpec::key);
    if (spec == kProperties.end()) {
      reject(std::format("'{}' is not a display property", key));
      return;
    }
    const std::optional<SavedProperty> parsed = parseProperty(*spec, value);
    if (!parsed) {
      reject(std::format("'{}' is not a valid {}", value, key));
      return;
    }
    const std::string name = savedOutputName(output->identity());
    if (documentSetsOutput(configRootPath(), name)) {
      reject(std::format("[output.{}] is set in {}", name, configRootPath().filename().string()));
      return;
    }
    const std::filesystem::path file = configRootPath().parent_path() / "displays.toml";
    if (!std::ranges::contains(configWatchPaths(), file)) {
      reject(configRootPath().filename().string() + " does not include displays.toml");
      return;
    }
    // The reader refuses min_workspaces next to a fixed count, so a count drops it and the minimum needs dynamic.
    const OutputRule* rule = findOutputRule(config(), output->identity());
    const bool fixedCount = rule != nullptr && rule->workspaces.has_value();
    if (key == "min_workspaces" && fixedCount && !std::holds_alternative<std::monostate>(*parsed)) {
      reject("min_workspaces applies to dynamic workspaces");
      return;
    }
    if (key == "workspaces" && std::holds_alternative<int64_t>(*parsed)
        && !saveOutputProperty(file, name, "min_workspaces", std::monostate{})) {
      reject("cannot write " + file.string());
      return;
    }
    if (!saveOutputProperty(file, name, key, *parsed)) {
      reject("cannot write " + file.string());
      return;
    }
    m_server.handleConfigReload();
  }

  void DesktopOutputManager::propertiesChanged() {
    for (wl_resource* resource : m_resources) {
      for (const auto& output : m_server.outputs()) {
        sendProperties(resource, *output);
      }
      dsk_output_manager_v1_send_done(resource);
    }
  }

  void DesktopOutputManager::sendProperties(wl_resource* resource, const Output& output) {
    const OutputRule* found = findOutputRule(config(), output.identity());
    const OutputRule rule = found != nullptr ? *found : OutputRule{};
    for (const PropertySpec& spec : kProperties) {
      const std::string key(spec.key);
      const std::string value = property(rule, spec.key);
      dsk_output_manager_v1_send_property(resource, output.wlr()->name, key.c_str(), value.c_str());
    }
  }

  void DesktopOutputManager::broadcast(const Output& output) {
    for (wl_resource* resource : m_resources) {
      sendState(resource, output);
    }
  }

  void DesktopOutputManager::sendState(wl_resource* resource, const Output& output) {
    const Output* source = output.mirrorSource();
    dsk_output_manager_v1_send_mirror(resource, output.wlr()->name, source != nullptr ? source->wlr()->name : "");
  }

  const struct dsk_output_manager_v1_interface DesktopOutputManager::kImplementation = {
      .destroy = DesktopOutputManager::handleDestroy,
      .set_mirror = DesktopOutputManager::handleSetMirror,
      .clear_mirror = DesktopOutputManager::handleClearMirror,
      .set_property = DesktopOutputManager::handleSetProperty,
  };

} // namespace umbriel

#include "output/display_store.h"

#include "config/generated_file.h"
#include "core/toml.h"
#include "output/identity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <variant>

namespace andromeda {

  namespace {

    constexpr std::string_view kHeader = "# Written by Andromeda when displays are changed from a settings app.\n"
                                         "# [output] tables in the including config.toml override these.\n\n";

    constexpr std::array<std::string_view, 8> kTransformNames = {
        "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270",
    };

    toml::table outputTable(const SavedOutput& output) {
      toml::table table;
      if (output.enabled) {
        table.insert("enabled", *output.enabled);
      }
      if (const std::optional<SavedMode>& mode = output.mode; mode && mode->width > 0 && mode->height > 0) {
        table.insert(
            "mode",
            mode->refreshMHz > 0 ? std::format("{}x{}@{:.3f}", mode->width, mode->height, mode->refreshMHz / 1000.0)
                                 : std::format("{}x{}", mode->width, mode->height)
        );
      }
      if (output.position) {
        table.insert("position", toml::array{(*output.position)[0], (*output.position)[1]});
      }
      if (output.scale) {
        table.insert("scale", *output.scale);
      }
      if (output.transform && *output.transform >= 0 && *output.transform < static_cast<int>(kTransformNames.size())) {
        table.insert("transform", kTransformNames[static_cast<size_t>(*output.transform)]);
      }
      if (output.adaptiveSync) {
        table.insert("vrr", *output.adaptiveSync ? "always" : "disabled");
      }
      return table;
    }

    template <typename T> std::optional<T> changed(const std::optional<T>& live, const std::optional<T>& requested) {
      return live != requested ? requested : std::nullopt;
    }

    void mergeOutputs(toml::table& root, std::span<const SavedOutput> outputs) {
      for (const SavedOutput& output : outputs) {
        toml::table saved = outputTable(output);
        if (saved.empty()) {
          continue;
        }
        toml::table& table = generatedTable(root, {"output", output.name});
        // zwlr_output_manager_v1 only knows on and off, so an apply keeps a VRR mode set through set_property.
        const bool keepVrr = table["vrr"].value<std::string>() == "fullscreen";
        for (auto&& [key, value] : saved) {
          if (!(keepVrr && key.str() == "vrr")) {
            table.insert_or_assign(key, std::move(value));
          }
        }
      }
    }

    void setMirror(toml::table& root, const std::string& name, const std::optional<std::string>& source) {
      toml::table& table = generatedTable(root, {"output", name});
      if (source) {
        table.insert_or_assign("mirror", *source);
      } else {
        table.erase("mirror");
      }
    }

  } // namespace

  std::string savedOutputName(const OutputIdentity& identity) {
    const bool reportsEdid = !identity.make.empty() || !identity.model.empty() || !identity.serial.empty();
    return reportsEdid ? outputDescriptor(identity) : std::string(identity.connector);
  }

  std::string mergeSavedOutputs(std::string_view existing, std::span<const SavedOutput> outputs) {
    return editGeneratedToml(existing, kHeader, [&](toml::table& root) { mergeOutputs(root, outputs); });
  }

  std::string
  mergeSavedMirror(std::string_view existing, const std::string& name, const std::optional<std::string>& source) {
    return editGeneratedToml(existing, kHeader, [&](toml::table& root) { setMirror(root, name, source); });
  }

  bool saveOutputs(const std::filesystem::path& file, std::span<const SavedOutput> outputs) {
    rewriteGeneratedTomlAsync(file, kHeader, [&](toml::table& root) { mergeOutputs(root, outputs); });
    return true;
  }

  bool saveOutputProperty(
      const std::filesystem::path& file, const std::string& name, std::string_view key, const SavedProperty& value
  ) {
    rewriteGeneratedTomlAsync(file, kHeader, [&](toml::table& root) {
      toml::table& table = generatedTable(root, {"output", name});
      std::visit(
          [&]<typename T>(const T& stored) {
            if constexpr (std::is_same_v<T, std::monostate>) {
              table.erase(key);
            } else {
              table.insert_or_assign(key, stored);
            }
          },
          value
      );
    });
    return true;
  }

  bool
  saveMirror(const std::filesystem::path& file, const std::string& name, const std::optional<std::string>& source) {
    rewriteGeneratedTomlAsync(file, kHeader, [&](toml::table& root) { setMirror(root, name, source); });
    return true;
  }

  bool
  documentSetsOutputKey(const std::filesystem::path& configRoot, std::string_view name, std::string_view key) {
    return std::ranges::contains(documentOutputKeys(configRoot, name), key);
  }

  std::vector<std::string> documentOutputKeys(const std::filesystem::path& configRoot, std::string_view name) {
    std::string content;
    if (std::ifstream in(configRoot); in) {
      content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    toml::table root;
    try {
      root = toml::parse(content);
    } catch (const toml::parse_error&) {
      return {};
    }
    std::vector<std::string> keys;
    // Direct member lookup, not at_path: an EDID descriptor name ("<make> <model> <serial>") can contain the dots
    // and spaces at_path would otherwise parse as a nested path.
    const toml::table* outputs = root["output"].as_table();
    const toml::table* table = outputs != nullptr ? (*outputs)[name].as_table() : nullptr;
    if (table != nullptr) {
      for (const auto& [key, value] : *table) {
        keys.emplace_back(key.str());
      }
    }
    return keys;
  }

  SavedOutput changedOutputKeys(const SavedOutput& live, const SavedOutput& requested) {
    // Scales travel as wl_fixed (1/256) and are stored as float; nearer than that is the same scale.
    const bool sameScale = live.scale && requested.scale && std::abs(*live.scale - *requested.scale) < 1.0 / 512;
    return {
        .name = requested.name,
        .enabled = changed(live.enabled, requested.enabled),
        .mode = changed(live.mode, requested.mode),
        .position = changed(live.position, requested.position),
        .scale = sameScale ? std::nullopt : requested.scale,
        .transform = changed(live.transform, requested.transform),
        .adaptiveSync = changed(live.adaptiveSync, requested.adaptiveSync),
    };
  }

  void dropSavedKey(SavedOutput& output, std::string_view key) {
    if (key == "enabled") {
      output.enabled.reset();
    } else if (key == "mode") {
      output.mode.reset();
    } else if (key == "position") {
      output.position.reset();
    } else if (key == "scale") {
      output.scale.reset();
    } else if (key == "transform") {
      output.transform.reset();
    } else if (key == "vrr") {
      output.adaptiveSync.reset();
    }
  }

} // namespace andromeda

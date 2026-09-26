#include "output/display_store.h"

#include "config/generated_file.h"
#include "core/toml.h"
#include "output/identity.h"

#include <array>
#include <format>
#include <string_view>

namespace umbriel {

  namespace {

    constexpr std::string_view kHeader = "# Written by Umbriel when displays are changed from a settings app.\n"
                                         "# [output] tables in the including config.toml override these.\n\n";

    constexpr std::array<std::string_view, 8> kTransformNames = {
        "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270",
    };

    toml::table outputTable(const SavedOutput& output) {
      toml::table table;
      table.insert("enabled", output.enabled);
      if (output.width > 0 && output.height > 0) {
        table.insert(
            "mode",
            output.refreshMHz > 0 ? std::format("{}x{}@{:.3f}", output.width, output.height, output.refreshMHz / 1000.0)
                                  : std::format("{}x{}", output.width, output.height)
        );
      }
      table.insert("position", toml::array{output.x, output.y});
      table.insert("scale", output.scale);
      if (output.transform >= 0 && output.transform < static_cast<int>(kTransformNames.size())) {
        table.insert("transform", kTransformNames[static_cast<size_t>(output.transform)]);
      }
      table.insert("vrr", output.adaptiveSync ? "always" : "disabled");
      return table;
    }

    void mergeOutputs(toml::table& root, std::span<const SavedOutput> outputs) {
      for (const SavedOutput& output : outputs) {
        toml::table& table = generatedTable(root, {"output", output.name});
        for (auto&& [key, value] : outputTable(output)) {
          table.insert_or_assign(key, std::move(value));
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
    return rewriteGeneratedToml(file, kHeader, [&](toml::table& root) { mergeOutputs(root, outputs); });
  }

  bool
  saveMirror(const std::filesystem::path& file, const std::string& name, const std::optional<std::string>& source) {
    return rewriteGeneratedToml(file, kHeader, [&](toml::table& root) { setMirror(root, name, source); });
  }

} // namespace umbriel

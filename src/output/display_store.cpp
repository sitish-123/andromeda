#include "output/display_store.h"

#include "core/toml.h"
#include "output/identity.h"

#include <array>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

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

  } // namespace

  namespace {

    toml::table parseOrEmpty(std::string_view existing) {
      if (!existing.empty()) {
        try {
          return toml::parse(existing);
        } catch (const toml::parse_error&) {
          // A hand-damaged file is replaced rather than kept half-parsed.
        }
      }
      return {};
    }

    toml::table& outputTableNamed(toml::table& root, const std::string& name) {
      if (root["output"].as_table() == nullptr) {
        root.insert_or_assign("output", toml::table{});
      }
      toml::table& outputs = *root["output"].as_table();
      if (outputs[name].as_table() == nullptr) {
        outputs.insert_or_assign(name, toml::table{});
      }
      return *outputs[name].as_table();
    }

    std::string serialize(const toml::table& root) {
      std::ostringstream out;
      out << kHeader << root << '\n';
      return std::move(out).str();
    }

    template <typename Edit> bool rewrite(const std::filesystem::path& file, Edit edit) {
      std::string existing;
      if (std::ifstream in(file); in) {
        existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      const std::string document = edit(existing);
      std::filesystem::path temporary = file;
      temporary += ".tmp";
      {
        std::ofstream out(temporary, std::ios::trunc);
        out << document;
        if (!out.flush()) {
          return false;
        }
      }
      std::error_code error;
      std::filesystem::rename(temporary, file, error);
      return !error;
    }

  } // namespace

  std::string savedOutputName(const OutputIdentity& identity) {
    const bool reportsEdid = !identity.make.empty() || !identity.model.empty() || !identity.serial.empty();
    return reportsEdid ? outputDescriptor(identity) : std::string(identity.connector);
  }

  std::string mergeSavedOutputs(std::string_view existing, std::span<const SavedOutput> outputs) {
    toml::table root = parseOrEmpty(existing);
    for (const SavedOutput& output : outputs) {
      toml::table& table = outputTableNamed(root, output.name);
      for (auto&& [key, value] : outputTable(output)) {
        table.insert_or_assign(key, std::move(value));
      }
    }
    return serialize(root);
  }

  std::string
  mergeSavedMirror(std::string_view existing, const std::string& name, const std::optional<std::string>& source) {
    toml::table root = parseOrEmpty(existing);
    toml::table& table = outputTableNamed(root, name);
    if (source) {
      table.insert_or_assign("mirror", *source);
    } else {
      table.erase("mirror");
    }
    return serialize(root);
  }

  bool saveOutputs(const std::filesystem::path& file, std::span<const SavedOutput> outputs) {
    return rewrite(file, [&](std::string_view existing) { return mergeSavedOutputs(existing, outputs); });
  }

  bool
  saveMirror(const std::filesystem::path& file, const std::string& name, const std::optional<std::string>& source) {
    return rewrite(file, [&](std::string_view existing) { return mergeSavedMirror(existing, name, source); });
  }

} // namespace umbriel

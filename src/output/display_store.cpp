#include "output/display_store.h"

#include "core/toml.h"

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

  std::string mergeSavedOutputs(std::string_view existing, std::span<const SavedOutput> outputs) {
    toml::table root;
    if (!existing.empty()) {
      try {
        root = toml::parse(existing);
      } catch (const toml::parse_error&) {
        // A hand-damaged file is replaced rather than kept half-parsed.
      }
    }
    toml::table* outputTables = root["output"].as_table();
    if (outputTables == nullptr) {
      root.insert_or_assign("output", toml::table{});
      outputTables = root["output"].as_table();
    }
    for (const SavedOutput& output : outputs) {
      outputTables->insert_or_assign(output.name, outputTable(output));
    }
    std::ostringstream out;
    out << kHeader << root << '\n';
    return std::move(out).str();
  }

  bool saveOutputs(const std::filesystem::path& file, std::span<const SavedOutput> outputs) {
    std::string existing;
    if (std::ifstream in(file); in) {
      existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string document = mergeSavedOutputs(existing, outputs);

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

} // namespace umbriel

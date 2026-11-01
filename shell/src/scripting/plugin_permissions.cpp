#include "scripting/plugin_permissions.h"

#include "util/file_utils.h"

#include <algorithm>

namespace scripting {

  namespace {

    std::vector<std::string> stringArray(const toml::table& tbl, std::string_view key) {
      std::vector<std::string> out;
      if (const auto* arr = tbl[key].as_array()) {
        for (const auto& node : *arr) {
          if (auto value = node.value<std::string>(); value.has_value() && !value->empty()) {
            out.push_back(std::move(*value));
          }
        }
      }
      return out;
    }

    std::filesystem::path resolvedPath(const std::filesystem::path& path) {
      std::error_code ec;
      auto resolved = std::filesystem::weakly_canonical(path, ec);
      return ec ? path.lexically_normal() : resolved;
    }

    bool rootsCover(const std::vector<std::string>& granted, const std::vector<std::string>& requested) {
      const auto roots = expandRoots(granted);
      return std::ranges::all_of(requested, [&roots](const std::string& root) {
        return pathWithin(FileUtils::expandUserPath(root), roots);
      });
    }

  } // namespace

  bool PluginPermissions::covers(const PluginPermissions& requested) const {
    return (process || !requested.process) && (network || !requested.network)
        && (clipboardRead || !requested.clipboardRead) && (environment || !requested.environment)
        && rootsCover(filesRead, requested.filesRead) && rootsCover(filesWrite, requested.filesWrite);
  }

  bool PluginPermissions::empty() const { return *this == PluginPermissions{}; }

  PluginPermissions parsePluginPermissions(const toml::table& root) {
    PluginPermissions out;
    const auto* tbl = root["permissions"].as_table();
    if (tbl == nullptr) {
      return out;
    }
    out.process = (*tbl)["process"].value_or(false);
    out.network = (*tbl)["network"].value_or(false);
    out.clipboardRead = (*tbl)["clipboard_read"].value_or(false);
    out.environment = (*tbl)["environment"].value_or(false);
    out.filesRead = stringArray(*tbl, "files_read");
    out.filesWrite = stringArray(*tbl, "files_write");
    return out;
  }

  std::vector<std::filesystem::path> expandRoots(const std::vector<std::string>& roots) {
    std::vector<std::filesystem::path> out;
    out.reserve(roots.size());
    for (const auto& root : roots) {
      out.push_back(FileUtils::expandUserPath(root));
    }
    return out;
  }

  bool pathWithin(const std::filesystem::path& path, const std::vector<std::filesystem::path>& roots) {
    if (!path.is_absolute()) {
      return false;
    }
    const auto target = resolvedPath(path);
    return std::ranges::any_of(roots, [&target](const std::filesystem::path& root) {
      if (!root.is_absolute()) {
        return false;
      }
      const auto base = resolvedPath(root);
      auto [baseEnd, targetIt] = std::ranges::mismatch(base, target);
      return baseEnd == base.end() || (std::next(baseEnd) == base.end() && baseEnd->empty());
    });
  }

} // namespace scripting

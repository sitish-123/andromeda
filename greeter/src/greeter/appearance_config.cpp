#include "greeter/appearance_config.h"

#include "core/log.h"
#include "greeter/appearance_sync.h"
#include "greeter/greeter_config_store.h"
#include "render/core/color.h"

#include <array>
#include <utility>

namespace {
  constexpr Logger kLog("greeter-appearance");

  bool
  parseTomlPaletteColor(const std::unordered_map<std::string, std::string>& palette, std::string_view key, Color& out) {
    const auto it = palette.find(std::string(key));
    if (it == palette.end()) {
      return false;
    }
    return tryParseHexColor(it->second, out);
  }

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  void applyTomlWallpaper(const greeter::config::GreeterTomlWallpaper& wallpaper, GreeterOutputWallpaper& out) {
    if (wallpaper.path.has_value()) {
      out.path = *wallpaper.path;
    }
    if (wallpaper.fillMode.has_value()) {
      if (const auto parsed = greeter::appearance::parseFillMode(*wallpaper.fillMode)) {
        out.fillMode = *parsed;
      }
    }
    if (wallpaper.fillColor.has_value()) {
      Color color;
      if (tryParseHexColor(*wallpaper.fillColor, color)) {
        out.fillColor = color;
      }
    } else if (!out.path.empty()) {
      Color color;
      if (parseColorWallpaperPath(out.path, color)) {
        out.fillColor = color;
      }
    }
  }

  [[nodiscard]] greeter::config::GreeterTomlWallpaper mergeTomlWallpaper(
      const greeter::config::GreeterTomlWallpaper& fallback, const greeter::config::GreeterTomlWallpaper& preferred
  ) {
    greeter::config::GreeterTomlWallpaper result = fallback;
    if (preferred.path.has_value()) {
      result.path = preferred.path;
    }
    if (preferred.fillMode.has_value()) {
      result.fillMode = preferred.fillMode;
    }
    if (preferred.fillColor.has_value()) {
      result.fillColor = preferred.fillColor;
    }
    return result;
  }

  [[nodiscard]] greeter::config::GreeterTomlAppearance mergeWallpaperAppearance(
      const greeter::config::GreeterTomlAppearance& fallback, const greeter::config::GreeterTomlAppearance& preferred
  ) {
    greeter::config::GreeterTomlAppearance result;
    if (fallback.wallpaper.has_value()) {
      result.wallpaper = fallback.wallpaper;
    }
    if (preferred.wallpaper.has_value()) {
      result.wallpaper = result.wallpaper.has_value() ? mergeTomlWallpaper(*result.wallpaper, *preferred.wallpaper)
                                                      : preferred.wallpaper;
    }

    result.wallpapers = fallback.wallpapers;
    for (const auto& [connector, wallpaper] : preferred.wallpapers) {
      const auto existing = result.wallpapers.find(connector);
      if (existing == result.wallpapers.end()) {
        result.wallpapers.emplace(connector, wallpaper);
      } else {
        existing->second = mergeTomlWallpaper(existing->second, wallpaper);
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<GreeterWallpaperAppearance>
  convertTomlWallpapers(const greeter::config::GreeterTomlAppearance& appearance) {
    if (!appearance.wallpaper.has_value() && appearance.wallpapers.empty()) {
      return std::nullopt;
    }

    GreeterWallpaperAppearance result;
    if (appearance.wallpaper.has_value()) {
      GreeterOutputWallpaper wallpaper;
      applyTomlWallpaper(*appearance.wallpaper, wallpaper);
      result.wallpaper = wallpaper;
    }
    for (const auto& [connector, wallpaper] : appearance.wallpapers) {
      GreeterOutputWallpaper outputWallpaper;
      applyTomlWallpaper(wallpaper, outputWallpaper);
      result.wallpapersByOutput.emplace(connector, std::move(outputWallpaper));
    }
    return result;
  }

  // Precondition: appearance.hasCompletePalette().
  [[nodiscard]] std::optional<GreeterSyncedAppearance>
  convertTomlAppearance(const greeter::config::GreeterTomlAppearance& appearance) {
    GreeterSyncedAppearance result;
    result.themeMode = appearance.themeMode.value_or("dark");
    result.cornerRadiusScale = appearance.cornerRadiusScale.value_or(1.0f);
    result.fontFamily = appearance.fontFamily.value_or("");

    // The sync carries the shell's token colors under the greeter's legacy 16-key names; the two roles it does not
    // export (fill_1, solid_thin) come from the token scheme of the same mode.
    result.palette = tokenPalette(result.themeMode == "light");
    const std::array<std::pair<std::string_view, Color*>, 10> keys = {{
        {"primary", &result.palette.accent},
        {"on_primary", &result.palette.onAccent},
        {"error", &result.palette.danger},
        {"surface", &result.palette.solidRegular},
        {"on_surface", &result.palette.label1},
        {"surface_variant", &result.palette.fill2},
        {"on_surface_variant", &result.palette.label2},
        {"outline", &result.palette.stroke},
        {"shadow", &result.palette.shadow},
        {"hover", &result.palette.fill3},
    }};
    for (const auto& [key, color] : keys) {
      if (!parseTomlPaletteColor(appearance.palette, key, *color)) {
        kLog.warn("greeter.toml appearance.palette has an invalid hex value");
        return std::nullopt;
      }
    }

    return result;
  }

} // namespace

std::filesystem::path greeterAppearanceConfigPath() { return greeter::appearance::manifestPath(); }

std::optional<GreeterSyncedAppearance> loadGreeterSyncedAppearance() {
  const auto config = greeter::config::loadConfig(greeter::appearance::packageConfPath());
  if (config.appearance.hasCompletePalette()) {
    if (auto fromToml = convertTomlAppearance(config.appearance)) {
      return fromToml;
    }
  }

  const auto syncPath = greeter::appearance::syncConfPath();
  greeter::config::GreeterSyncFile sync = greeter::config::loadSync(syncPath);
  if (sync.appearance.hasCompletePalette()) {
    if (auto fromSync = convertTomlAppearance(sync.appearance)) {
      return fromSync;
    }
  }

  // One-shot migration: legacy Sync-owned appearance.json -> sync.toml [appearance].
  if (const auto legacy = greeter::appearance::parseManifestForSync(greeter::appearance::manifestPath())) {
    sync.appearance = legacy->appearance;
    if (!greeter::config::writeSync(syncPath, sync)) {
      kLog.warn("failed to migrate legacy appearance.json into {}", syncPath.string());
    } else {
      kLog.info("migrated legacy appearance.json into {}", syncPath.string());
    }
    return convertTomlAppearance(legacy->appearance);
  }

  return std::nullopt;
}

std::optional<GreeterWallpaperAppearance> loadGreeterWallpaperAppearance() {
  const auto config = greeter::config::loadConfig(greeter::appearance::packageConfPath());
  const auto sync = greeter::config::loadSync(greeter::appearance::syncConfPath());
  return convertTomlWallpapers(mergeWallpaperAppearance(sync.appearance, config.appearance));
}

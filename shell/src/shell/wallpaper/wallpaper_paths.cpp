#include "shell/wallpaper/wallpaper_paths.h"

#include "config/config_types.h"
#include "util/file_utils.h"

#include <filesystem>
#include <system_error>

namespace {
  // Installed by the andromeda-wallpapers package (pkg/wallpapers).
  constexpr const char* kPackagedWallpaperDirectory = "/usr/share/backgrounds/andromeda";
} // namespace

ThemeMode wallpaper::effectiveThemeMode(ThemeMode mode, bool isLight) noexcept {
  if (mode == ThemeMode::Auto) {
    return isLight ? ThemeMode::Light : ThemeMode::Dark;
  }
  return mode;
}

const WallpaperMonitorOverride*
wallpaper::findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output) {
  for (const auto& ovr : config.monitorOverrides) {
    if (outputMatchesSelector(ovr.match, output)) {
      return &ovr;
    }
  }
  return nullptr;
}

std::string
wallpaper::resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode) {
  if (config.perMonitorDirectories) {
    if (const auto* ovr = findWallpaperMonitorOverride(config, output); ovr != nullptr) {
      if (mode == ThemeMode::Light && ovr->directoryLight.has_value() && !ovr->directoryLight->empty()) {
        return *ovr->directoryLight;
      }
      if (mode == ThemeMode::Dark && ovr->directoryDark.has_value() && !ovr->directoryDark->empty()) {
        return *ovr->directoryDark;
      }
      if (ovr->directory.has_value() && !ovr->directory->empty()) {
        return *ovr->directory;
      }
    }
  }
  return resolveGlobalWallpaperDirectory(config, mode);
}

std::string wallpaper::resolveGlobalWallpaperDirectory(const WallpaperConfig& config, ThemeMode mode) {
  if (mode == ThemeMode::Light && !config.directoryLight.empty()) {
    return config.directoryLight;
  }
  if (mode == ThemeMode::Dark && !config.directoryDark.empty()) {
    return config.directoryDark;
  }
  if (!config.directory.empty()) {
    return config.directory;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(kPackagedWallpaperDirectory, ec)) {
    return kPackagedWallpaperDirectory;
  }
  return FileUtils::defaultPicturesDirectory().string();
}

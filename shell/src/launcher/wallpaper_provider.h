#pragma once

#include "launcher/launcher_provider.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class WaylandConnection;

namespace noctalia::theme {
  class ThemeService;
}

class WallpaperProvider : public LauncherProvider {
public:
  WallpaperProvider(
      ConfigService* config, WaylandConnection* wayland, noctalia::theme::ThemeService* themeService = nullptr
  );

  [[nodiscard]] std::string_view defaultPrefix() const override { return "wall"; }
  [[nodiscard]] std::string_view id() const override { return "Wallpaper"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "wallpaper-selector"; }
  [[nodiscard]] bool supportsGrid() const override { return true; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;
  void setResultsChangedCallback(std::function<void()> callback) override { m_resultsChanged = std::move(callback); }
  [[nodiscard]] bool isLoading() const override { return m_scanning && m_candidates == nullptr; }

  struct Candidate {
    std::string name;
    std::string path;
    std::string searchable;
  };

private:
  // The directory walk runs on the SharedWorker; a query reads the last walk and asks for a new one when it is older
  // than kRescan, so typing never walks the tree.
  void requestScan(const std::string& directory) const;

  mutable std::string m_scannedDirectory;
  mutable std::shared_ptr<const std::vector<Candidate>> m_candidates;
  mutable std::chrono::steady_clock::time_point m_scannedAt;
  mutable bool m_scanning = false;
  std::function<void()> m_resultsChanged;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);

  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  noctalia::theme::ThemeService* m_themeService = nullptr;
};

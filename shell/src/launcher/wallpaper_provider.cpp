#include "launcher/wallpaper_provider.h"

#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/shared_worker.h"
#include "i18n/i18n.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "theme/theme_service.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

  constexpr std::size_t kMaxResults = 50;

  using WallpaperCandidate = WallpaperProvider::Candidate;
  constexpr auto kRescan = std::chrono::seconds(5);

  std::vector<WallpaperCandidate> collectWallpapers(const std::filesystem::path& directory) {
    std::vector<WallpaperCandidate> candidates;
    if (directory.empty()) {
      return candidates;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
      return candidates;
    }

    FileUtils::walkDirectoryTree(
        directory, [](const std::filesystem::directory_entry&) { return true; },
        [&candidates](const std::filesystem::directory_entry& entry) {
          std::error_code typeEc;
          if (!entry.is_regular_file(typeEc) || typeEc || !DirectoryScanner::isImagePath(entry.path())) {
            return;
          }

          WallpaperCandidate candidate;
          candidate.name = entry.path().filename().string();
          candidate.path = entry.path().string();
          candidate.searchable =
              StringUtils::toLower(candidate.name + " " + entry.path().parent_path().filename().string());
          candidates.push_back(std::move(candidate));
        }
    );

    std::ranges::sort(candidates, [](const auto& a, const auto& b) {
      return StringUtils::toLower(a.name) < StringUtils::toLower(b.name);
    });
    return candidates;
  }

} // namespace

WallpaperProvider::WallpaperProvider(
    ConfigService* config, WaylandConnection* wayland, noctalia::theme::ThemeService* themeService
)
    : m_config(config), m_wayland(wayland), m_themeService(themeService) {}

std::string WallpaperProvider::displayName() const { return i18n::tr("launcher.providers.wallpaper.title"); }

std::vector<LauncherResult> WallpaperProvider::query(std::string_view text) const {
  if (m_config == nullptr) {
    return {};
  }

  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  const ThemeMode configured = shellThemeMode(m_config->config().theme);
  const bool isLight = m_themeService != nullptr ? m_themeService->isLightMode() : configured == ThemeMode::Light;
  const std::string directory = wallpaper::resolveGlobalWallpaperDirectory(
      m_config->config().wallpaper, wallpaper::effectiveThemeMode(configured, isLight)
  );
  if (directory != m_scannedDirectory) {
    m_scannedDirectory = directory;
    m_candidates.reset();
    requestScan(directory);
  } else if (std::chrono::steady_clock::now() - m_scannedAt >= kRescan) {
    requestScan(directory);
  }
  const auto candidates = m_candidates;
  if (candidates == nullptr || candidates->empty()) {
    return {};
  }

  std::vector<std::pair<double, const WallpaperCandidate*>> scored;
  scored.reserve(candidates->size());
  for (const auto& candidate : *candidates) {
    const double score = query.empty() ? 0.0 : FuzzyMatch::score(query, candidate.searchable);
    if (query.empty() || FuzzyMatch::isMatch(score)) {
      scored.emplace_back(score, &candidate);
    }
  }

  const auto limit = std::min(scored.size(), kMaxResults);
  std::partial_sort(
      scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(limit), scored.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; }
  );

  std::vector<LauncherResult> results;
  results.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& [score, candidate] = scored[i];
    LauncherResult result;
    result.id = candidate->path;
    result.title = candidate->name;
    result.subtitle = candidate->path;
    result.glyphName = "wallpaper-selector";
    result.iconPath = candidate->path;
    result.score = score;
    results.push_back(std::move(result));
  }
  return results;
}

void WallpaperProvider::requestScan(const std::string& directory) const {
  if (m_scanning) {
    return;
  }
  m_scanning = true;
  m_scannedAt = std::chrono::steady_clock::now();
  SharedWorker::instance().post<std::shared_ptr<const std::vector<Candidate>>>(
      m_alive,
      [directory] { return std::make_shared<const std::vector<Candidate>>(collectWallpapers(directory)); },
      [this, directory](std::shared_ptr<const std::vector<Candidate>> candidates) {
        m_scanning = false;
        if (directory != m_scannedDirectory) {
          requestScan(m_scannedDirectory);
          return;
        }
        const bool changed = m_candidates == nullptr
            || !std::ranges::equal(*m_candidates, *candidates, [](const Candidate& a, const Candidate& b) {
                 return a.path == b.path;
               });
        m_candidates = std::move(candidates);
        m_scannedAt = std::chrono::steady_clock::now();
        if (changed && m_resultsChanged) {
          m_resultsChanged();
        }
      }
  );
}

bool WallpaperProvider::activate(const LauncherResult& result) {
  if (m_config == nullptr || result.id.empty()) {
    return false;
  }
  if (!result.providerId.empty() && result.providerId != id()) {
    return false;
  }

  const std::filesystem::path path(result.id);
  std::error_code ec;
  if (!DirectoryScanner::isImagePath(path) || !std::filesystem::is_regular_file(path, ec) || ec) {
    return false;
  }

  ConfigService::WallpaperBatch batch(*m_config);
  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (!out.connectorName.empty()) {
        m_config->setWallpaperPath(out.connectorName, result.id);
      }
    }
  }
  m_config->setWallpaperPath(std::nullopt, result.id);
  return true;
}

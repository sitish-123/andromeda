#include "shell/greeter/greeter_appearance_sync.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/shared_worker.h"
#include "dbus/polkit/polkit_session_support.h"
#include "ipc/ipc_service.h"
#include "render/core/color.h"
#include "render/core/image_encoder.h"
#include "render/core/image_file_loader.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/session/session_action_meta.h"
#include "ui/auth/auth_layout.h"
#include "ui/palette.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <initializer_list>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <linux/magic.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <system_error>
#include <toml++/toml.hpp>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("greeter-sync");
  using greeter::detail::ApplyHelperProtocol;
  std::atomic<bool> g_greeterSyncInProgress{false};

  void releaseGreeterSync() noexcept { g_greeterSyncInProgress.store(false, std::memory_order_release); }

  class GreeterSyncLease {
  public:
    GreeterSyncLease() = default;
    ~GreeterSyncLease() {
      if (m_releaseOnDestruction) {
        releaseGreeterSync();
      }
    }

    GreeterSyncLease(const GreeterSyncLease&) = delete;
    GreeterSyncLease& operator=(const GreeterSyncLease&) = delete;

    void handOffToCompletion() noexcept { m_releaseOnDestruction = false; }

  private:
    bool m_releaseOnDestruction = true;
  };

  constexpr std::string_view kApplyHelperName = "noctalia-greeter-apply-appearance";
  constexpr std::string_view kPkexecName = "pkexec";
  constexpr std::string_view kSyncArgument = "--sync";
  constexpr std::string_view kCapabilityArgument = "--supports";
  constexpr std::string_view kSecureSyncCapability = "secure-sync-v1";
  constexpr std::string_view kGreeterName = "noctalia-greeter";
  constexpr std::string_view kGreeterTomlFileName = "greeter.toml";
  constexpr std::string_view kDefaultGreeterStateDir = "/var/lib/noctalia-greeter";
  constexpr std::string_view kGreeterStateDirEnv = "ANDROMEDA_GREETER_STATE_DIR";
  constexpr std::string_view kStagedOutputLayoutFileName = "output_layout";
  constexpr std::string_view kStagedOutputTransformsFileName = "output_transforms";
  constexpr std::string_view kStagedOutputScalesFileName = "output_scales";
  // Staged appearance fragment; the apply helper merges it into live sync.toml.
  constexpr std::string_view kStagedSyncTomlFileName = "sync.toml";

  enum class GreeterSyncMode : std::uint8_t {
    LegacyAuthenticated,
    Constrained,
  };

  [[nodiscard]] bool secureStagedFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (FileUtils::setPrivateFilePermissions(path, ec)) {
      return true;
    }
    kLog.warn("failed to secure staged file '{}': {}", path.string(), ec.message());
    return false;
  }

  [[nodiscard]] bool secureStagingDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    if (FileUtils::setPrivateDirectoryPermissions(path, ec)) {
      return true;
    }
    kLog.warn("failed to secure staging directory '{}': {}", path.string(), ec.message());
    return false;
  }

  [[nodiscard]] std::string canonicalExecutablePath(const std::filesystem::path& candidate) {
    if (!process::commandExists(candidate.c_str())) {
      return {};
    }
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(candidate, ec);
    return ec ? std::string{} : canonical.string();
  }

  [[nodiscard]] bool isTrustedPrivilegeExecutable(const std::filesystem::path& executable) {
    struct stat executableState{};
    if (::stat(executable.c_str(), &executableState) != 0
        || !S_ISREG(executableState.st_mode)
        || executableState.st_uid != 0
        || (executableState.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return false;
    }

    struct statfs filesystemState{};
    if (::statfs(executable.c_str(), &filesystemState) != 0 || filesystemState.f_type == FUSE_SUPER_MAGIC) {
      return false;
    }

    for (auto directory = executable.parent_path(); !directory.empty(); directory = directory.parent_path()) {
      struct stat directoryState{};
      if (::stat(directory.c_str(), &directoryState) != 0
          || !S_ISDIR(directoryState.st_mode)
          || directoryState.st_uid != 0) {
        return false;
      }
      const bool writableByOthers = (directoryState.st_mode & (S_IWGRP | S_IWOTH)) != 0;
      if (writableByOthers && (directoryState.st_mode & S_ISVTX) == 0) {
        return false;
      }
      if (directory == directory.root_path()) {
        break;
      }
    }
    return true;
  }

  [[nodiscard]] std::string
  resolveProgramPath(std::string_view name, std::initializer_list<const char*> fallbackPaths) {
    if (name.find('/') != std::string_view::npos) { // NOLINT(readability-container-contains): C++20-compatible.
      return canonicalExecutablePath(std::filesystem::path(name));
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      pathEnv = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }

    const std::string programName(name);
    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find(':', start);
      const std::string_view dir = end == std::string_view::npos ? path.substr(start) : path.substr(start, end - start);
      const auto candidate = (dir.empty() ? std::filesystem::path(".") : std::filesystem::path(dir)) / programName;
      if (auto resolved = canonicalExecutablePath(candidate); !resolved.empty()) {
        return resolved;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }

    for (const char* candidate : fallbackPaths) {
      if (auto resolved = canonicalExecutablePath(candidate); !resolved.empty()) {
        return resolved;
      }
    }
    return {};
  }

  [[nodiscard]] bool programExists(std::string_view name, std::initializer_list<const char*> fallbackPaths) {
    return !resolveProgramPath(name, fallbackPaths).empty();
  }

  Color resolveWallpaperFillColor(const WallpaperConfig& config) {
    if (!config.fillColor) {
      return rgba(0.0F, 0.0F, 0.0F, 0.0F);
    }
    return resolveColorSpec(*config.fillColor);
  }

  // The greeter palette is opaque: translucent token roles are flattened onto the shell's solid surface.
  void putPaletteColor(toml::table& palette, std::string_view key, const Color& color) {
    const Color opaque{color.r, color.g, color.b, 1.0F};
    palette.insert_or_assign(std::string(key), formatRgbHex(lerpColor(::palette.solidRegular, opaque, color.a)));
  }

  [[nodiscard]] std::filesystem::path selectedGreeterStateDirectory() {
    const char* stateDir = std::getenv(kGreeterStateDirEnv.data());
    if (stateDir != nullptr && stateDir[0] != '\0') {
      return std::filesystem::path(stateDir).lexically_normal();
    }
    return std::filesystem::path(kDefaultGreeterStateDir);
  }

  [[nodiscard]] bool isDefaultGreeterStateDirectory(const std::filesystem::path& stateDirectory) {
    return stateDirectory == std::filesystem::path(kDefaultGreeterStateDir).lexically_normal();
  }

  [[nodiscard]] std::filesystem::path greeterTomlPath() {
    return selectedGreeterStateDirectory() / kGreeterTomlFileName;
  }

  [[nodiscard]] std::optional<std::string> readGreeterConfiguredOutput() {
    const auto path = greeterTomlPath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::nullopt;
    }

    try {
      const toml::table table = toml::parse_file(path.string());
      if (const toml::table* output = table["output"].as_table()) {
        const auto name = (*output)["name"].value<std::string>();
        if (name.has_value() && !name->empty()) {
          return name;
        }
      }
      return std::nullopt;
    } catch (const toml::parse_error& e) {
      kLog.warn("failed to parse {}: {}", path.string(), e.description());
      return std::nullopt;
    }
  }

  [[nodiscard]] std::string sanitizeConnectorFileToken(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const unsigned char c : name) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('_');
      }
    }
    return out.empty() ? std::string("unknown") : out;
  }

  // Fallback single wallpaper when no per-output map is usable.
  [[nodiscard]] std::string resolveSyncWallpaperPath(const ConfigService& configService) {
    // The greeter shows the lock screen's own image when it has one, as the lock screen does (not a live one).
    if (const std::string& lockWallpaper = configService.config().lockscreen.wallpaper;
        !lockWallpaper.empty() && LockScreen::liveWallpaperType(lockWallpaper).empty()) {
      return FileUtils::expandUserPath(lockWallpaper).string();
    }
    if (const auto output = readGreeterConfiguredOutput(); output.has_value() && !output->empty()) {
      const std::string path = configService.getWallpaperPath(*output);
      if (!path.empty()) {
        return path;
      }
    }
    std::string defaultPath = configService.getDefaultWallpaperPath();
    if (!defaultPath.empty()) {
      return defaultPath;
    }
    return configService.getGreeterSyncWallpaperPath();
  }

  struct StagedOutputWallpaper {
    std::string connector;
    std::string installedName;
    std::string sourcePath;
  };

  // Blur as a fraction of the image's long side, on an image at most kBakedBlurMaxSide (lock_surface.cpp kOwnImageBlur).
  constexpr float kBakedBlur = 0.03F;
  constexpr int kBakedBlurMaxSide = 1024;

  std::function<void()> g_onBakeReady;
  std::uint32_t g_clockInkFrom = 0;
  std::uint32_t g_clockInkTo = 0;
  std::unordered_set<std::string> g_bakesInFlight;
  // Bakes that failed (an image the loader cannot decode); the key holds the mtime, so a changed file tries again.
  std::unordered_set<std::string> g_bakesFailed;
  const std::shared_ptr<int> g_bakeAlive = std::make_shared<int>(0);

  [[nodiscard]] std::filesystem::path bakeCacheDir() { return std::filesystem::path(FileUtils::stateDir()) / "greeter-lock"; }

  // Same image file and the same blur and dim = the same key.
  [[nodiscard]] std::string bakeKey(const std::filesystem::path& source, const LockscreenConfig& lock) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(source, ec);
    const auto mtime = std::filesystem::last_write_time(source, ec).time_since_epoch().count();
    const std::string id = std::format(
        "{}|{}|{}|{:.3f}|{:.3f}", source.string(), size, mtime, lock.blurIntensity, lock.tintIntensity
    );
    return std::format("{:016x}", std::hash<std::string>{}(id));
  }

  // Worker thread: the Andromeda lock look (blur, then the 4.5:1 dim) as a JPEG at `destination`.
  [[nodiscard]] bool bakeLockWallpaper(
      const std::filesystem::path& source, const std::filesystem::path& destination, float blurIntensity,
      float tintIntensity
  ) {
    const float blur = std::clamp(blurIntensity, 0.0F, 1.0F) * kBakedBlur;
    auto image = loadImageFile(source.string(), blur > 0.0F ? kBakedBlurMaxSide : 0);
    if (!image.has_value()) {
      kLog.warn("greeter sync: cannot decode '{}' to bake the lock look: {}", source.string(), image.error());
      return false;
    }
    blurRgba(
        image->rgba, image->width, image->height,
        static_cast<int>(std::lround(blur * static_cast<float>(std::max(image->width, image->height))))
    );
    const float dim = authBackdropDim(std::min(tintIntensity, 0.6F), rgbaBrightLuminance(image->rgba));
    const float keep = 1.0F - std::clamp(dim, 0.0F, 1.0F);
    for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
      for (std::size_t c = 0; c < 3; ++c) {
        image->rgba[i + c] = static_cast<std::uint8_t>(std::lround(static_cast<float>(image->rgba[i + c]) * keep));
      }
    }
    std::string error;
    const auto jpeg = encodeJpeg(image->rgba.data(), image->width, image->height, 90, &error);
    if (jpeg.empty()) {
      kLog.warn("greeter sync: cannot encode the baked '{}': {}", source.string(), error);
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      if (!out.good()) {
        kLog.warn("greeter sync: cannot write '{}'", temporary.string());
        return false;
      }
    }
    std::filesystem::rename(temporary, destination, ec);
    return !ec;
  }

  // Stages `source` as `<stem>.<ext>`. The greeter only shows the image, so the Andromeda lock look is baked into it
  // (no blur work at boot). The bake runs on SharedWorker: until it is cached, the raw image is staged and a finished
  // bake runs the bake-ready callback (a new sync). Ryoku paints an opaque scene, so its image is copied.
  // `keys` collects the cache entries in use. Empty name = not staged; nullopt = the staging dir is not safe.
  [[nodiscard]] std::optional<std::string> stageLockWallpaper(
      const std::filesystem::path& staging, const std::filesystem::path& source, const std::string& stem,
      const LockscreenConfig& lock, std::unordered_set<std::string>& keys
  ) {
    std::filesystem::path from = source;
    std::string installedName = stem + source.extension().string();
    if (lock.theme != "ryoku") {
      const std::string key = bakeKey(source, lock);
      keys.insert(key);
      const std::filesystem::path baked = bakeCacheDir() / (key + ".jpg");
      std::error_code ec;
      if (std::filesystem::is_regular_file(baked, ec)) {
        from = baked;
        installedName = stem + ".jpg";
      } else if (!g_bakesFailed.contains(key) && g_bakesInFlight.insert(key).second) {
        const auto blur = static_cast<float>(lock.blurIntensity);
        const auto tint = static_cast<float>(lock.tintIntensity);
        SharedWorker::instance().post<bool>(
            g_bakeAlive, [source, baked, blur, tint] { return bakeLockWallpaper(source, baked, blur, tint); },
            [key](bool ok) {
              g_bakesInFlight.erase(key);
              if (!ok) {
                g_bakesFailed.insert(key);
              } else if (g_onBakeReady) {
                g_onBakeReady();
              }
            }
        );
      }
    }
    std::error_code ec;
    std::filesystem::copy_file(from, staging / installedName, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      kLog.warn("greeter sync: failed to stage wallpaper '{}': {}", from.string(), ec.message());
      return std::string{};
    }
    if (!secureStagedFile(staging / installedName)) {
      return std::nullopt;
    }
    return installedName;
  }

  // Drops cached bakes this sync did not use (old wallpapers, old blur and dim values).
  void pruneBakeCache(const std::unordered_set<std::string>& keys) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(bakeCacheDir(), ec)) {
      const std::string name = entry.path().filename().string();
      // <key>.jpg, or <key>.jpg.tmp a failed bake left.
      const std::string key = name.substr(0, name.find('.'));
      if ((name.ends_with(".jpg") || name.ends_with(".jpg.tmp")) && !keys.contains(key) && !g_bakesInFlight.contains(key)) {
        std::filesystem::remove(entry.path(), ec);
      }
    }
  }

  // Stage every per-monitor wallpaper so the greeter can pick by connector name.
  // File wallpapers are copied as wallpaper-<connector>.*; solid color: specs are
  // kept as sourcePath only (no file) so the wallpapers map can still reference them.
  [[nodiscard]] std::optional<std::vector<StagedOutputWallpaper>>
  stageAllOutputWallpapers(
      const std::filesystem::path& staging, const ConfigService& configService, std::unordered_set<std::string>& keys
  ) {
    std::vector<StagedOutputWallpaper> staged;
    // Each display gets the image its lock screen shows (LockScreen::wallpaperPathForOutput); a live lock wallpaper
    // (video, shader) cannot be shown by the greeter, which then keeps the desktop image.
    const LockscreenConfig& lock = configService.config().lockscreen;
    std::unordered_map<std::string, std::string> outputs = configService.monitorWallpaperPaths();
    for (const auto& [connector, path] : lock.wallpaperOutputs) {
      outputs.try_emplace(connector);
    }
    for (auto& [connector, path] : outputs) {
      const auto it = lock.wallpaperOutputs.find(connector);
      const bool desktop = it != lock.wallpaperOutputs.end() && it->second == LockScreen::kDesktopWallpaper;
      std::string own = it != lock.wallpaperOutputs.end() && !desktop ? it->second : std::string{};
      if (own.empty() && !desktop) {
        own = lock.wallpaper;
      }
      if (!own.empty() && LockScreen::liveWallpaperType(own).empty()) {
        path = FileUtils::expandUserPath(own).string();
      }
    }
    for (const auto& [connector, sourcePath] : outputs) {
      if (connector.empty() || sourcePath.empty()) {
        continue;
      }
      if (sourcePath.starts_with("color:")) {
        staged.push_back(StagedOutputWallpaper{connector, /*installedName=*/{}, sourcePath});
        kLog.info("greeter sync: color wallpaper for '{}' -> {}", connector, sourcePath);
        continue;
      }
      std::error_code ec;
      const std::filesystem::path source(sourcePath);
      if (!std::filesystem::is_regular_file(source, ec) || ec) {
        kLog.warn("greeter sync: skip missing wallpaper for '{}': {}", connector, sourcePath);
        continue;
      }
      const auto stagedName =
          stageLockWallpaper(staging, source, "wallpaper-" + sanitizeConnectorFileToken(connector), lock, keys);
      if (!stagedName.has_value()) {
        return std::nullopt;
      }
      if (stagedName->empty()) {
        continue;
      }
      const std::string& installedName = *stagedName;
      kLog.info("greeter sync: staged wallpaper for '{}' -> {}", connector, installedName);
      staged.push_back(StagedOutputWallpaper{connector, installedName, sourcePath});
    }
    // unordered_map iteration order is unspecified; keep map/fallback deterministic.
    std::ranges::sort(staged, [](const StagedOutputWallpaper& a, const StagedOutputWallpaper& b) {
      return a.connector < b.connector;
    });
    return staged;
  }

  [[nodiscard]] std::string findApplyHelper() {
    std::string helper = resolveProgramPath(
        kApplyHelperName,
        {"/usr/bin/noctalia-greeter-apply-appearance", "/usr/local/bin/noctalia-greeter-apply-appearance"}
    );
    if (!helper.empty() && !isTrustedPrivilegeExecutable(helper)) {
      kLog.warn("refusing untrusted greeter sync helper '{}'", helper);
      helper.clear();
    }
    return helper;
  }

  [[nodiscard]] ApplyHelperProtocol probeApplyHelperProtocol(std::string_view helper) {
    process::RunOptions options;
    options.timeout = std::chrono::seconds(1);
    options.maxOutputBytes = 16U * 1024U;
    const process::RunResult result = process::runSync(
        std::vector<std::string>{
            std::string(helper), std::string(kCapabilityArgument), std::string(kSecureSyncCapability)
        },
        options
    );
    return greeter::detail::classifyApplyHelperProtocol(result);
  }

  [[nodiscard]] bool defaultStateDirectorySelected() {
    return isDefaultGreeterStateDirectory(selectedGreeterStateDirectory());
  }

  [[nodiscard]] GreeterSyncMode chooseSyncMode(const ApplyHelperProtocol protocol) {
    if (protocol == ApplyHelperProtocol::Legacy || !defaultStateDirectorySelected()) {
      return GreeterSyncMode::LegacyAuthenticated;
    }
    return GreeterSyncMode::Constrained;
  }

  [[nodiscard]] std::filesystem::path makeStagingDirectory(const GreeterSyncMode mode) {
    std::filesystem::path runtimeDirectory;
    if (mode == GreeterSyncMode::Constrained) {
      // The secure helper accepts only this lexical path, derived from PKEXEC_UID.
      runtimeDirectory = std::filesystem::path("/run/user") / std::to_string(::getuid());
    } else {
      const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
      runtimeDirectory = runtimeDir != nullptr && runtimeDir[0] != '\0' ? std::filesystem::path(runtimeDir)
                                                                        : std::filesystem::temp_directory_path();
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(runtimeDirectory, ec) || ec) {
      kLog.warn(
          "required greeter sync runtime directory '{}' is unavailable: {}", runtimeDirectory.string(),
          ec ? ec.message() : "not a directory"
      );
      return {};
    }

    auto staging = runtimeDirectory / "noctalia-greeter-sync";
    std::filesystem::remove_all(staging, ec);
    if (ec) {
      kLog.warn("failed to clear greeter sync staging directory '{}': {}", staging.string(), ec.message());
      return {};
    }
    std::filesystem::create_directories(staging, ec);
    if (ec) {
      kLog.warn("failed to create greeter sync staging directory '{}': {}", staging.string(), ec.message());
      return {};
    }
    if (!secureStagingDirectory(staging)) {
      return {};
    }
    return staging;
  }

  void putOptionalString(toml::table& table, std::string_view key, const std::optional<std::string>& value) {
    if (value.has_value() && !value->empty()) {
      table.insert_or_assign(std::string(key), *value);
    }
  }

  void appendSessionToSyncToml(toml::table& root, const ShellSessionConfig& session) {
    toml::table sessionTable;
    toml::table power;
    putOptionalString(power, "suspend", session.power.suspend);
    putOptionalString(power, "reboot", session.power.reboot);
    putOptionalString(power, "shutdown", session.power.shutdown);
    if (!power.empty()) {
      sessionTable.insert("power", std::move(power));
    }

    toml::array actions;
    const auto& source = session.actions.empty() ? defaultSessionPanelActions() : session.actions;
    for (const SessionPanelActionConfig& row : source) {
      // The greeter has its own firmware-setup button and no consumer for reboot_uefi rows.
      if (!row.enabled || !session_action::isKnown(row.action) || row.action == "reboot_uefi") {
        continue;
      }
      if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
        continue;
      }

      toml::table item;
      item.insert_or_assign("action", row.action);
      putOptionalString(item, "command", row.command);
      putOptionalString(item, "label", row.label);
      putOptionalString(item, "glyph", row.glyph);
      if (row.variant != SessionActionButtonVariant::Default) {
        item.insert_or_assign("variant", std::string(enumToKey(kSessionActionButtonVariants, row.variant)));
      }
      actions.push_back(std::move(item));
    }
    if (!actions.empty()) {
      sessionTable.insert("actions", std::move(actions));
    }
    if (!sessionTable.empty()) {
      root.insert("session", std::move(sessionTable));
    }
  }

  [[nodiscard]] std::string formatToml(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{
        table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    return out.str();
  }

  // Secure sync stages appearance only. The authenticated legacy helper also expects
  // the previous session payload and otherwise clears its stored session commands.
  [[nodiscard]] bool writeStagedSyncToml(
      const std::filesystem::path& staging, const Config& config, std::string_view resolvedMode,
      const std::string& wallpaperPath, const std::string& installedWallpaperName,
      const std::vector<StagedOutputWallpaper>& outputWallpapers, const std::filesystem::path& installedStateDirectory,
      const GreeterSyncMode mode
  ) {
    toml::table root;
    toml::table appearance;
    appearance.insert_or_assign("scheme", "Synced");
    appearance.insert_or_assign("theme_mode", std::string(resolvedMode));
    appearance.insert_or_assign("corner_radius_scale", static_cast<double>(config.shell.cornerRadiusScale));
    if (!config.shell.fontFamily.empty()) {
      appearance.insert_or_assign("font_family", config.shell.fontFamily);
    }

    // Keys follow the greeter's own 16-color palette schema, filled from the shell's token roles.
    toml::table palette;
    putPaletteColor(palette, "primary", ::palette.accent);
    putPaletteColor(palette, "on_primary", ::palette.onAccent);
    putPaletteColor(palette, "secondary", ::palette.accent);
    putPaletteColor(palette, "on_secondary", ::palette.onAccent);
    putPaletteColor(palette, "tertiary", ::palette.accent);
    putPaletteColor(palette, "on_tertiary", ::palette.onAccent);
    putPaletteColor(palette, "error", ::palette.danger);
    putPaletteColor(palette, "on_error", ::palette.onAccent);
    putPaletteColor(palette, "surface", ::palette.solidRegular);
    putPaletteColor(palette, "on_surface", ::palette.label1);
    putPaletteColor(palette, "surface_variant", ::palette.fill2);
    putPaletteColor(palette, "on_surface_variant", ::palette.label2);
    putPaletteColor(palette, "outline", ::palette.stroke);
    putPaletteColor(palette, "shadow", ::palette.shadow);
    putPaletteColor(palette, "hover", ::palette.fill3);
    putPaletteColor(palette, "on_hover", ::palette.label1);
    appearance.insert("palette", std::move(palette));

    if (config.wallpaper.enabled) {
      toml::table wallpaper;
      if (!installedWallpaperName.empty()) {
        wallpaper.insert_or_assign("path", (installedStateDirectory / installedWallpaperName).string());
      } else if (!wallpaperPath.empty()) {
        wallpaper.insert_or_assign("path", wallpaperPath);
      }
      const std::string fillMode = std::string(enumToKey(kWallpaperFillModes, config.wallpaper.fillMode));
      wallpaper.insert_or_assign("fill_mode", fillMode);
      const Color fillColor = resolveWallpaperFillColor(config.wallpaper);
      if (fillColor.a > 0.0F) {
        wallpaper.insert_or_assign("fill_color", formatRgbHex(fillColor));
      }
      if (!wallpaper.empty()) {
        appearance.insert("wallpaper", std::move(wallpaper));
      }

      if (!outputWallpapers.empty()) {
        toml::table byOutput;
        for (const auto& entry : outputWallpapers) {
          toml::table item;
          if (!entry.installedName.empty()) {
            item.insert_or_assign("path", (installedStateDirectory / entry.installedName).string());
          } else if (!entry.sourcePath.empty()) {
            item.insert_or_assign("path", entry.sourcePath);
          } else {
            continue;
          }
          item.insert_or_assign("fill_mode", fillMode);
          if (fillColor.a > 0.0F) {
            item.insert_or_assign("fill_color", formatRgbHex(fillColor));
          }
          byOutput.insert(entry.connector, std::move(item));
        }
        if (!byOutput.empty()) {
          appearance.insert("wallpapers", std::move(byOutput));
        }
      }
    }

    root.insert("appearance", std::move(appearance));

    const LockscreenConfig& ls = config.lockscreen;
    toml::table lockscreen;
    lockscreen.insert_or_assign("theme", ls.theme);
    lockscreen.insert_or_assign("layout", ls.layout);
    lockscreen.insert_or_assign("clock_style", ls.clockStyle);
    lockscreen.insert_or_assign("clock_size", ls.clockSize);
    lockscreen.insert_or_assign("clock_format_24h", ls.clockFormat24h);
    lockscreen.insert_or_assign("clock_seconds", ls.clockSeconds);
    // The greeter validator accepts only these forms; out-of-range values go as the style default.
    lockscreen.insert_or_assign("clock_weight", ls.clockWeight >= 100 && ls.clockWeight <= 1000 ? ls.clockWeight : 0);
    lockscreen.insert_or_assign("clock_width", ls.clockWidth >= 50 && ls.clockWidth <= 200 ? ls.clockWidth : 0);
    lockscreen.insert_or_assign("clock_round", std::clamp(ls.clockRound, -1, 100));
    lockscreen.insert_or_assign("clock_slant", std::clamp(ls.clockSlant, 0, 15));
    // The greeter rejects the whole [lockscreen] table on an unknown value: send the default instead.
    const auto oneOf = [](const std::string& v, std::initializer_list<std::string_view> known, std::string_view fallback) {
      return std::ranges::find(known, v) != known.end() ? v : std::string(fallback);
    };
    lockscreen.insert_or_assign("clock_material", oneOf(ls.clockMaterial, {"solid", "glass", "tint"}, "solid"));
    lockscreen.insert_or_assign(
        "clock_color", oneOf(ls.clockColor, {"white", "accent", "wallpaper", "gradient"}, "white")
    );
    lockscreen.insert_or_assign("clock_motion", ls.clockMotion);
    if (g_clockInkFrom != 0 && g_clockInkTo != 0) {
      lockscreen.insert_or_assign("clock_ink_from", formatRgbHex(rgbaHex(g_clockInkFrom)));
      lockscreen.insert_or_assign("clock_ink_to", formatRgbHex(rgbaHex(g_clockInkTo)));
    }
    lockscreen.insert_or_assign("blur_intensity", static_cast<double>(ls.blurIntensity));
    lockscreen.insert_or_assign("tint_intensity", static_cast<double>(ls.tintIntensity));
    root.insert("lockscreen", std::move(lockscreen));

    if (mode == GreeterSyncMode::LegacyAuthenticated) {
      appendSessionToSyncToml(root, config.shell.session);
    }

    const auto syncPath = staging / kStagedSyncTomlFileName;
    std::ofstream out(syncPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged sync.toml '{}'", syncPath.string());
      return false;
    }
    if (!secureStagedFile(syncPath)) {
      return false;
    }
    out << "# noctalia-greeter staged sync.toml (merged into live sync.toml by apply-appearance)\n\n";
    out << formatToml(root);
    if (!out.good()) {
      kLog.warn("failed to write staged sync.toml '{}'", syncPath.string());
      return false;
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string> buildGreeterOutputLayout(const CompositorPlatform& platform) {
    if (!platform.wayland().hasXdgOutputManager()) {
      kLog.info("greeter sync: xdg-output unavailable; skipping output layout sync");
      return std::nullopt;
    }

    const auto& outputs = platform.outputs();
    std::vector<const WaylandOutput*> ready;
    ready.reserve(outputs.size());
    for (const auto& output : outputs) {
      if (!output.connectorName.empty() && !output.done) {
        kLog.info("greeter sync: output '{}' not ready; skipping output layout sync", output.connectorName);
        return std::nullopt;
      }
      if (!output.done || output.connectorName.empty() || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }
      ready.push_back(&output);
    }

    if (ready.size() < 2) {
      kLog.info("greeter sync: {} ready output(s); skipping output layout sync", ready.size());
      return std::nullopt;
    }

    const int originX = ready.front()->logicalX;
    const int originY = ready.front()->logicalY;
    const bool allShareOrigin = std::ranges::all_of(ready, [originX, originY](const WaylandOutput* output) {
      return output->logicalX == originX && output->logicalY == originY;
    });
    if (allShareOrigin) {
      kLog.info("greeter sync: ready outputs share the same origin; skipping output layout sync");
      return std::nullopt;
    }

    std::ranges::sort(ready, [](const WaylandOutput* lhs, const WaylandOutput* rhs) {
      if (lhs->logicalX != rhs->logicalX) {
        return lhs->logicalX < rhs->logicalX;
      }
      if (lhs->logicalY != rhs->logicalY) {
        return lhs->logicalY < rhs->logicalY;
      }
      return lhs->connectorName < rhs->connectorName;
    });

    std::string layout;
    for (const WaylandOutput* output : ready) {
      if (!layout.empty()) {
        layout += "; ";
      }
      layout += output->connectorName + ':' + std::to_string(output->logicalX) + ',' + std::to_string(output->logicalY);
    }
    return layout;
  }

  [[nodiscard]] std::optional<std::string> greeterTransformToken(std::int32_t transform) {
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
      return std::string("normal");
    case WL_OUTPUT_TRANSFORM_90:
      return std::string("90");
    case WL_OUTPUT_TRANSFORM_180:
      return std::string("180");
    case WL_OUTPUT_TRANSFORM_270:
      return std::string("270");
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      return std::string("flipped");
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      return std::string("flipped-90");
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      return std::string("flipped-180");
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      return std::string("flipped-270");
    default:
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<std::string> buildGreeterOutputTransforms(const CompositorPlatform& platform) {
    const auto& outputs = platform.outputs();
    std::vector<const WaylandOutput*> ready;
    ready.reserve(outputs.size());
    for (const auto& output : outputs) {
      if (!output.connectorName.empty() && !output.done) {
        kLog.info("greeter sync: output '{}' not ready; skipping output transforms sync", output.connectorName);
        return std::nullopt;
      }
      if (!output.done || output.connectorName.empty()) {
        continue;
      }
      ready.push_back(&output);
    }

    if (ready.empty()) {
      kLog.info("greeter sync: no ready outputs; skipping output transforms sync");
      return std::nullopt;
    }

    std::ranges::sort(ready, [](const WaylandOutput* lhs, const WaylandOutput* rhs) {
      return lhs->connectorName < rhs->connectorName;
    });

    std::string transforms;
    for (const WaylandOutput* output : ready) {
      const std::int32_t transform = output->effectiveTransform();
      const auto token = greeterTransformToken(transform);
      if (!token.has_value()) {
        kLog.warn(
            "greeter sync: output '{}' has unknown transform {}; skipping output transforms sync",
            output->connectorName, transform
        );
        return std::nullopt;
      }
      if (!transforms.empty()) {
        transforms += "; ";
      }
      transforms += output->connectorName + ':' + *token;
    }
    return transforms;
  }

  [[nodiscard]] std::optional<float> detectedLayoutScale(const WaylandOutput& output) {
    if (output.width <= 0 || output.height <= 0 || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      return std::nullopt;
    }

    const auto physicalW = static_cast<double>(output.width);
    const auto physicalH = static_cast<double>(output.height);
    const auto logicalW = static_cast<double>(output.logicalWidth);
    const auto logicalH = static_cast<double>(output.logicalHeight);

    const auto candidate = [](double xScale, double yScale) {
      return std::pair{(xScale + yScale) * 0.5, std::abs(xScale - yScale)};
    };
    const auto normal = candidate(physicalW / logicalW, physicalH / logicalH);
    const auto rotated = candidate(physicalW / logicalH, physicalH / logicalW);
    const double scale = rotated.second < normal.second ? rotated.first : normal.first;
    if (scale < 1.0) {
      return std::nullopt;
    }
    return static_cast<float>(scale);
  }

  [[nodiscard]] std::optional<std::string> buildGreeterOutputScales(const CompositorPlatform& platform) {
    if (!platform.wayland().hasXdgOutputManager()) {
      kLog.info("greeter sync: xdg-output unavailable; skipping output scales sync");
      return std::nullopt;
    }

    const auto& outputs = platform.outputs();
    std::vector<const WaylandOutput*> ready;
    ready.reserve(outputs.size());
    for (const auto& output : outputs) {
      if (!output.connectorName.empty() && !output.done) {
        kLog.info("greeter sync: output '{}' not ready; skipping output scales sync", output.connectorName);
        return std::nullopt;
      }
      if (!output.done || output.connectorName.empty()) {
        continue;
      }
      ready.push_back(&output);
    }

    if (ready.empty()) {
      kLog.info("greeter sync: no ready outputs; skipping output scales sync");
      return std::nullopt;
    }

    std::ranges::sort(ready, [](const WaylandOutput* lhs, const WaylandOutput* rhs) {
      return lhs->connectorName < rhs->connectorName;
    });

    std::string scales;
    for (const WaylandOutput* output : ready) {
      const auto scale = detectedLayoutScale(*output);
      if (!scale.has_value()) {
        kLog.warn(
            "greeter sync: output '{}' has unknown scale (logical={}x{} mode={}x{}); skipping output scales sync",
            output->connectorName, output->logicalWidth, output->logicalHeight, output->width, output->height
        );
        return std::nullopt;
      }
      if (!scales.empty()) {
        scales += "; ";
      }
      scales += output->connectorName + ':' + std::format("{:.3F}", *scale);
    }
    return scales;
  }

  void logOutputLayoutForGreeter(const CompositorPlatform& platform) {
    const auto& outputs = platform.outputs();
    if (outputs.empty()) {
      kLog.info("greeter sync: no Wayland outputs available for layout logging");
      return;
    }

    kLog.info("greeter sync: {} Wayland output(s) (logical layout from compositor):", outputs.size());
    for (const auto& output : outputs) {
      kLog.info(
          "  output connector='{}' description='{}' done={} logical=({}, {}) {}x{} physical={}x{} scale={} "
          "transform={}",
          output.connectorName.empty() ? "?" : output.connectorName,
          output.description.empty() ? "?" : output.description, output.done, output.logicalX, output.logicalY,
          output.logicalWidth, output.logicalHeight, output.width, output.height, output.scale,
          output.effectiveTransform()
      );
    }

    if (const auto layout = buildGreeterOutputLayout(platform)) {
      kLog.info("greeter sync: staging output_layout \"{}\"", *layout);
    }
    if (const auto transforms = buildGreeterOutputTransforms(platform)) {
      kLog.info("greeter sync: staging output_transforms \"{}\"", *transforms);
    }
    if (const auto scales = buildGreeterOutputScales(platform)) {
      kLog.info("greeter sync: staging output_scales \"{}\"", *scales);
    }
  }

  [[nodiscard]] bool stageOutputLayout(const std::filesystem::path& staging, std::string_view layout) {
    const auto layoutPath = staging / kStagedOutputLayoutFileName;
    std::ofstream out(layoutPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged output layout '{}'", layoutPath.string());
      return false;
    }
    if (!secureStagedFile(layoutPath)) {
      return false;
    }
    out << layout << '\n';
    if (!out.good()) {
      kLog.warn("failed to write staged output layout '{}'", layoutPath.string());
      return false;
    }
    return true;
  }

  [[nodiscard]] bool stageOutputTransforms(const std::filesystem::path& staging, std::string_view transforms) {
    const auto transformsPath = staging / kStagedOutputTransformsFileName;
    std::ofstream out(transformsPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged output transforms '{}'", transformsPath.string());
      return false;
    }
    if (!secureStagedFile(transformsPath)) {
      return false;
    }
    out << transforms << '\n';
    if (!out.good()) {
      kLog.warn("failed to write staged output transforms '{}'", transformsPath.string());
      return false;
    }
    return true;
  }

  [[nodiscard]] bool stageOutputScales(const std::filesystem::path& staging, std::string_view scales) {
    const auto scalesPath = staging / kStagedOutputScalesFileName;
    std::ofstream out(scalesPath);
    if (!out.is_open()) {
      kLog.warn("failed to open staged output scales '{}'", scalesPath.string());
      return false;
    }
    if (!secureStagedFile(scalesPath)) {
      return false;
    }
    out << scales << '\n';
    if (!out.good()) {
      kLog.warn("failed to write staged output scales '{}'", scalesPath.string());
      return false;
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string>
  stageWallpaper(
      const std::filesystem::path& staging, std::string_view sourcePath, const LockscreenConfig& lock,
      std::unordered_set<std::string>& keys
  ) {
    if (sourcePath.empty()) {
      return std::string{};
    }
    if (sourcePath.starts_with("color:")) {
      return std::string{};
    }

    std::error_code ec;
    const std::filesystem::path source(sourcePath);
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
      return std::string{};
    }

    return stageLockWallpaper(staging, source, "wallpaper", lock, keys);
  }

  [[nodiscard]] bool hasPrivilegeCommandOverride(const ShellGreeterSyncConfig& greeterSync) {
    return !StringUtils::trim(greeterSync.privilegeCommand).empty();
  }

  [[nodiscard]] std::string privilegeCommandPrefix(const ShellGreeterSyncConfig& greeterSync) {
    return StringUtils::trim(greeterSync.privilegeCommand);
  }

  [[nodiscard]] std::string trustedEnvironmentHelper() {
    for (const char* candidate : {"/usr/bin/env", "/bin/env", "/run/current-system/sw/bin/env"}) {
      if (auto resolved = canonicalExecutablePath(candidate);
          !resolved.empty() && isTrustedPrivilegeExecutable(resolved)) {
        return resolved;
      }
    }
    return {};
  }

  [[nodiscard]] std::optional<std::string>
  legacyStateEnvironment(const GreeterSyncMode mode, const std::filesystem::path& stateDirectory) {
    if (mode != GreeterSyncMode::LegacyAuthenticated || isDefaultGreeterStateDirectory(stateDirectory)) {
      return std::nullopt;
    }
    return std::string(kGreeterStateDirEnv) + '=' + stateDirectory.string();
  }

  [[nodiscard]] std::string buildPrivilegedApplyCommand(
      std::string_view privilegePrefix, std::string_view helper, std::string_view staging, const GreeterSyncMode mode,
      const std::optional<std::string>& stateEnvironment
  ) {
    std::string command = std::string(privilegePrefix) + ' ';
    if (stateEnvironment.has_value()) {
      const std::string envHelper = trustedEnvironmentHelper();
      if (envHelper.empty()) {
        return {};
      }
      command += StringUtils::shellQuote(envHelper) + ' ' + StringUtils::shellQuote(*stateEnvironment) + ' ';
    }
    command += StringUtils::shellQuote(helper) + ' ';
    if (mode == GreeterSyncMode::Constrained) {
      command += std::string(kSyncArgument) + " ";
    }
    return command + StringUtils::shellQuote(staging);
  }

  [[nodiscard]] bool launchPrivilegedApplyHelper(
      const ShellGreeterSyncConfig& greeterSync, std::string_view helper, const std::filesystem::path& staging,
      const std::filesystem::path& stateDirectory, const GreeterSyncMode mode, process::RunCallbacks callbacks
  ) {
    const auto stateEnvironment = legacyStateEnvironment(mode, stateDirectory);
    if (hasPrivilegeCommandOverride(greeterSync)) {
      const std::string command = buildPrivilegedApplyCommand(
          privilegeCommandPrefix(greeterSync), helper, staging.string(), mode, stateEnvironment
      );
      if (command.empty()) {
        return false;
      }
      return process::runAsync(command, std::move(callbacks));
    }

    if (mode == GreeterSyncMode::Constrained) {
      if (!process::commandExists(kPkexecName.data())) {
        return false;
      }
      return process::runAsync(
          std::vector<std::string>{
              std::string(kPkexecName), std::string(helper), std::string(kSyncArgument), staging.string()
          },
          std::move(callbacks)
      );
    }

    const std::string escalator = process::resolvePrivilegeEscalator().value_or(std::string{});
    if (escalator.empty()) {
      return false;
    }
    std::vector<std::string> arguments{escalator};
    if (stateEnvironment.has_value()) {
      if (escalator == "run0") {
        arguments.push_back("--setenv=" + *stateEnvironment);
      } else {
        const std::string envHelper = trustedEnvironmentHelper();
        if (envHelper.empty()) {
          return false;
        }
        arguments.push_back(envHelper);
        arguments.push_back(*stateEnvironment);
      }
    }
    arguments.emplace_back(helper);
    arguments.push_back(staging.string());
    return process::runAsync(arguments, std::move(callbacks));
  }

} // namespace

namespace greeter::detail {

  ApplyHelperProtocol classifyApplyHelperProtocol(const process::RunResult& result) {
    if (result.timedOut || result.outTruncated || result.errTruncated) {
      return ApplyHelperProtocol::Unknown;
    }
    // process::runSync strips trailing line endings from captured output.
    if (result.exitCode == 0 && result.out == kSecureSyncCapability && result.err.empty()) {
      return ApplyHelperProtocol::SecureSyncV1;
    }

    const std::string usage = result.out + '\n' + result.err;
    const bool knownLegacyUsage = result.exitCode == 2
        && usage.contains("usage:")
        && usage.contains("<staging-directory>")
        && usage.contains("--setup-system")
        && usage.contains("--print-greeter-user")
        && !usage.contains("--sync <staging-directory>");
    return knownLegacyUsage ? ApplyHelperProtocol::Legacy : ApplyHelperProtocol::Unknown;
  }

} // namespace greeter::detail

namespace greeter {

  bool appearanceSyncAvailable(const ShellGreeterSyncConfig& greeterSync) noexcept {
    return programExists(kGreeterName, {"/usr/bin/noctalia-greeter", "/usr/local/bin/noctalia-greeter"})
        && !findApplyHelper().empty()
        && (process::resolvePrivilegeEscalator().has_value() || hasPrivilegeCommandOverride(greeterSync));
  }

  GreeterSyncLaunch syncAppearanceToGreeterAsync(
      const ConfigService& configService, std::string_view resolvedThemeMode, SyncCompletion onComplete,
      const CompositorPlatform* platform, const bool logindOnSystemBus
  ) {
    bool expected = false;
    if (!g_greeterSyncInProgress.compare_exchange_strong(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed
        )) {
      kLog.info("greeter appearance sync is already in progress");
      return GreeterSyncLaunch::Busy;
    }
    GreeterSyncLease syncLease;

    const auto completion = std::make_shared<SyncCompletion>(std::move(onComplete));
    const auto finish = [completion](bool success) {
      if (completion && *completion) {
        (*completion)(success);
      }
    };

    const auto helper = findApplyHelper();
    if (helper.empty()) {
      kLog.warn("greeter sync helper is not installed");
      return GreeterSyncLaunch::Failed;
    }

    const ApplyHelperProtocol protocol = probeApplyHelperProtocol(helper);
    if (protocol == ApplyHelperProtocol::Unknown) {
      kLog.warn("greeter sync helper returned an unrecognized capability response");
      return GreeterSyncLaunch::Failed;
    }
    const GreeterSyncMode mode = chooseSyncMode(protocol);
    const std::filesystem::path stateDirectory = mode == GreeterSyncMode::Constrained
        ? std::filesystem::path(kDefaultGreeterStateDir)
        : selectedGreeterStateDirectory();
    if (!stateDirectory.is_absolute()) {
      kLog.warn("greeter state directory must be absolute for authenticated legacy sync");
      return GreeterSyncLaunch::Failed;
    }
    if (protocol == ApplyHelperProtocol::SecureSyncV1 && mode == GreeterSyncMode::LegacyAuthenticated) {
      kLog.info("custom greeter state directory requires administrator-authenticated legacy sync");
    }

    const Config& config = configService.config();
    const ShellGreeterSyncConfig& greeterSync = config.shell.greeterSync;
    if (!hasPrivilegeCommandOverride(greeterSync)) {
      if (mode == GreeterSyncMode::Constrained && !process::commandExists(kPkexecName.data())) {
        kLog.warn("secure greeter sync requires pkexec");
        return GreeterSyncLaunch::Failed;
      }
      if (mode == GreeterSyncMode::LegacyAuthenticated && !process::resolvePrivilegeEscalator().has_value()) {
        kLog.warn("no privilege escalator is available for legacy greeter sync");
        return GreeterSyncLaunch::Failed;
      }
    }

    const auto staging = makeStagingDirectory(mode);
    if (staging.empty()) {
      kLog.warn("failed to create greeter sync staging directory");
      return GreeterSyncLaunch::Failed;
    }

    if (platform != nullptr) {
      logOutputLayoutForGreeter(*platform);
      if (const auto layout = buildGreeterOutputLayout(*platform)) {
        if (!stageOutputLayout(staging, *layout)) {
          return GreeterSyncLaunch::Failed;
        }
      }
      if (const auto transforms = buildGreeterOutputTransforms(*platform)) {
        if (!stageOutputTransforms(staging, *transforms)) {
          return GreeterSyncLaunch::Failed;
        }
      }
      if (const auto scales = buildGreeterOutputScales(*platform)) {
        if (!stageOutputScales(staging, *scales)) {
          return GreeterSyncLaunch::Failed;
        }
      }
    } else {
      kLog.info("greeter sync: no compositor platform provided; skipping output layout/transforms/scales sync");
    }

    std::vector<StagedOutputWallpaper> outputWallpapers;
    std::string wallpaperPath;
    std::string installedWallpaperName;
    if (config.wallpaper.enabled) {
      std::unordered_set<std::string> bakeKeys;
      auto stagedOutputs = stageAllOutputWallpapers(staging, configService, bakeKeys);
      if (!stagedOutputs.has_value()) {
        return GreeterSyncLaunch::Failed;
      }
      outputWallpapers = std::move(*stagedOutputs);

      wallpaperPath = resolveSyncWallpaperPath(configService);
      const auto stagedWallpaper = stageWallpaper(staging, wallpaperPath, configService.config().lockscreen, bakeKeys);
      if (!stagedWallpaper.has_value()) {
        return GreeterSyncLaunch::Failed;
      }
      installedWallpaperName = *stagedWallpaper;
      pruneBakeCache(bakeKeys);
      if (installedWallpaperName.empty() && !wallpaperPath.starts_with("color:")) {
        wallpaperPath.clear();
      }
      // Prefer a staged per-output entry for the legacy single wallpaper when needed.
      if (installedWallpaperName.empty() && !outputWallpapers.empty()) {
        auto preferEntry = [&](const StagedOutputWallpaper& entry) {
          wallpaperPath = entry.sourcePath;
          installedWallpaperName = entry.installedName;
        };
        bool pinResolved = false;
        if (const auto pin = readGreeterConfiguredOutput(); pin.has_value() && !pin->empty()) {
          for (const auto& entry : outputWallpapers) {
            if (entry.connector == *pin) {
              preferEntry(entry);
              pinResolved = true;
              break;
            }
          }
        }
        if (!pinResolved) {
          for (const auto& entry : outputWallpapers) {
            if (!entry.installedName.empty()) {
              preferEntry(entry);
              break;
            }
          }
          if (installedWallpaperName.empty() && wallpaperPath.empty()) {
            preferEntry(outputWallpapers.front());
          }
        }
      }
    } else {
      kLog.info("greeter sync: wallpaper disabled; not syncing wallpaper");
    }
    if (!writeStagedSyncToml(
            staging, config, resolvedThemeMode, wallpaperPath, installedWallpaperName, outputWallpapers, stateDirectory,
            mode
        )) {
      return GreeterSyncLaunch::Failed;
    }

    if (mode == GreeterSyncMode::LegacyAuthenticated
        && !hasPrivilegeCommandOverride(greeterSync)
        && !polkit_session::likelySupportsInSessionPolkitAgent(logindOnSystemBus)) {
      kLog.info("greeter sync: staged legacy payload; skipping background authorization without a login session");
      return GreeterSyncLaunch::StagedOnly;
    }

    process::RunCallbacks callbacks;
    callbacks.onExit = [finish, mode](const process::RunResult& result) {
      releaseGreeterSync();
      if (!result) {
        if (!result.err.empty()) {
          kLog.warn("greeter sync failed: {}", result.err);
        } else {
          kLog.warn("greeter sync failed with exit code {}", result.exitCode);
        }
        finish(false);
        return;
      }
      kLog.info(
          "synced shell {} to greeter",
          mode == GreeterSyncMode::Constrained ? "appearance" : "appearance and session actions"
      );
      finish(true);
    };
    if (!launchPrivilegedApplyHelper(greeterSync, helper, staging, stateDirectory, mode, std::move(callbacks))) {
      return GreeterSyncLaunch::Failed;
    }
    syncLease.handOffToCompletion();
    return mode == GreeterSyncMode::Constrained ? GreeterSyncLaunch::LaunchedConstrained
                                                : GreeterSyncLaunch::LaunchedLegacy;
  }

  void setLockClockGradient(std::uint32_t from, std::uint32_t to) {
    g_clockInkFrom = from;
    g_clockInkTo = to;
  }

  void setLockWallpaperBakeReady(std::function<void()> callback) { g_onBakeReady = std::move(callback); }

  void registerIpc(
      IpcService& ipc, const ConfigService& config, std::function<std::string_view()> resolvedThemeMode,
      const CompositorPlatform* platform, std::function<bool()> logindOnSystemBus
  ) {
    if (!appearanceSyncAvailable(config.config().shell.greeterSync)) {
      return;
    }
    ipc.bind(
        noctalia::cli::msg::greeterSync,
        [&config, resolvedThemeMode = std::move(resolvedThemeMode), platform,
         logindOnSystemBus = std::move(logindOnSystemBus)](const std::string& args) -> std::string {
          if (!StringUtils::trim(args).empty()) {
            return "error: usage: greeter-sync\n";
          }
          if (!appearanceSyncAvailable(config.config().shell.greeterSync)) {
            return "error: noctalia greeter is not installed\n";
          }
          const bool logind = logindOnSystemBus != nullptr && logindOnSystemBus();
          const GreeterSyncLaunch launch =
              syncAppearanceToGreeterAsync(config, resolvedThemeMode(), {}, platform, logind);
          if (launch == GreeterSyncLaunch::Busy) {
            return "error: greeter appearance sync is already in progress\n";
          }
          if (launch == GreeterSyncLaunch::Failed) {
            return "error: failed to start greeter appearance sync\n";
          }
          if (launch == GreeterSyncLaunch::StagedOnly) {
            return "ok: staged (run legacy privilege install manually)\n";
          }
          return "ok\n";
        },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
  }

} // namespace greeter

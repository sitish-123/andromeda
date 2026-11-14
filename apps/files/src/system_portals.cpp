#include "system_portals.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("system-portals");
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kAccount = "org.freedesktop.impl.portal.Account";
  constexpr auto kWallpaper = "org.freedesktop.impl.portal.Wallpaper";
  constexpr auto kLauncher = "org.freedesktop.impl.portal.DynamicLauncher";
  constexpr auto kRequest = "org.freedesktop.impl.portal.Request";
  constexpr std::uint32_t kLauncherApp = 1;
  constexpr std::uint32_t kLauncherWebApp = 2;
  using Options = std::map<std::string, sdbus::Variant>;
  using IconVariant = sdbus::Struct<std::string, sdbus::Variant>; // a serialized GIcon: (kind, data)

  template <typename T> std::optional<T> option(const Options& options, const char* key) {
    const auto it = options.find(key);
    if (it == options.end()) {
      return std::nullopt;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error& e) {
      kLog.warn("option {} has the wrong type: {}", key, e.what());
      return std::nullopt;
    }
  }

  std::string fileFromUri(const std::string& uri) {
    gchar* path = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
    std::string out = path != nullptr ? path : "";
    g_free(path);
    return out;
  }

  // A serialized GIcon as a file the sheet or the notification server can load; bytes go to the runtime dir.
  std::string iconFile(const IconVariant& icon, const std::string& stem) {
    try {
      const std::string& kind = icon.get<0>();
      if (kind == "bytes") {
        const auto bytes = icon.get<1>().get<std::vector<std::uint8_t>>();
        const fs::path dir = fs::path(g_get_user_runtime_dir()) / "andromeda-files";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path file = dir / (stem + ".png");
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<std::streamsize>(bytes.size()));
        return file.string();
      }
      if (kind == "file") {
        return fileFromUri(icon.get<1>().get<std::string>());
      }
      if (kind == "themed") {
        const auto names = icon.get<1>().get<std::vector<std::string>>();
        return names.empty() ? std::string() : names.front();
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("icon {}: {}", stem, e.what());
    }
    return {};
  }

} // namespace

struct SystemPortals::Pending {
  std::optional<sdbus::Result<std::uint32_t, Options>> withResults; // Account, DynamicLauncher
  std::optional<sdbus::Result<std::uint32_t>> plain;                // Wallpaper
  std::unique_ptr<sdbus::IObject> requestObject;
  Request request;
  std::optional<sdbus::Variant> icon; // DynamicLauncher: returned as given
};

SystemPortals::User SystemPortals::currentUser() {
  User user{.id = g_get_user_name(), .name = g_get_real_name(), .image = {}};
  if (user.name.empty() || user.name == "Unknown") {
    user.name = user.id;
  }
  // AccountsService keeps the picture Settings › Users sets; ~/.face is the older place.
  for (const fs::path& file : {fs::path("/var/lib/AccountsService/icons") / user.id, fs::path(g_get_home_dir()) / ".face"}) {
    if (access(file.c_str(), R_OK) == 0) {
      user.image = file.string();
      break;
    }
  }
  return user;
}

SystemPortals::SystemPortals(SessionBus& bus, Handlers handlers) : m_bus(bus), m_handlers(std::move(handlers)) {
  auto& connection = m_bus.connection();
  m_account = sdbus::createObject(connection, kObjectPath);
  m_account
      ->addVTable(sdbus::registerMethod("GetUserInformation")
                      .withInputParamNames("handle", "app_id", "window", "options")
                      .withOutputParamNames("response", "results")
                      .implementedAs([this](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath handle,
                                            std::string appId, std::string window, Options options) {
                        auto pending = std::make_unique<Pending>();
                        pending->withResults = std::move(result);
                        pending->request = {.kind = Kind::Account, .appId = std::move(appId),
                                            .parentWindow = std::move(window),
                                            .reason = option<std::string>(options, "reason").value_or("")};
                        const Request request = pending->request;
                        const std::uint64_t id = track(handle, std::move(pending));
                        kLog.info("account for {}", request.appId.empty() ? "?" : request.appId);
                        m_handlers.ask(id, request);
                      }))
      .forInterface(kAccount);

  m_wallpaper = sdbus::createObject(connection, kObjectPath);
  m_wallpaper
      ->addVTable(sdbus::registerMethod("SetWallpaperURI")
                      .withInputParamNames("handle", "app_id", "parent_window", "uri", "options")
                      .withOutputParamNames("response")
                      .implementedAs([this](sdbus::Result<std::uint32_t>&& result, sdbus::ObjectPath handle,
                                            std::string appId, std::string parent, std::string uri, Options options) {
                        Request request{.kind = Kind::Wallpaper, .appId = std::move(appId),
                                        .parentWindow = std::move(parent), .path = fileFromUri(uri),
                                        .setOn = option<std::string>(options, "set-on").value_or("both"),
                                        .showPreview = option<bool>(options, "show-preview").value_or(false)};
                        std::error_code ec;
                        if (request.path.empty() || !fs::is_regular_file(request.path, ec)) {
                          kLog.warn("wallpaper: {} is not a local file", uri);
                          result.returnResults(std::uint32_t{2});
                          return;
                        }
                        auto pending = std::make_unique<Pending>();
                        pending->plain = std::move(result);
                        pending->request = request;
                        const std::uint64_t id = track(handle, std::move(pending));
                        kLog.info("wallpaper {} from {} (preview {})", request.path, request.appId, request.showPreview);
                        // Without a preview the frontend already asked consent (Access dialog, permission store).
                        if (request.showPreview) {
                          m_handlers.ask(id, request);
                        } else {
                          answer(id, 0);
                        }
                      }))
      .forInterface(kWallpaper);

  m_launcher = sdbus::createObject(connection, kObjectPath);
  m_launcher
      ->addVTable(
          sdbus::registerMethod("PrepareInstall")
              .withInputParamNames("handle", "app_id", "parent_window", "name", "icon_v", "options")
              .withOutputParamNames("response", "results")
              .implementedAs([this](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath handle,
                                    std::string appId, std::string parent, std::string name, sdbus::Variant icon,
                                    Options options) {
                const std::uint32_t type = option<std::uint32_t>(options, "launcher_type").value_or(kLauncherApp);
                Request request{.kind = Kind::Launcher, .appId = std::move(appId), .parentWindow = std::move(parent),
                                .name = std::move(name),
                                .editableName = option<bool>(options, "editable_name").value_or(true),
                                .webApp = type == kLauncherWebApp,
                                .target = option<std::string>(options, "target").value_or("")};
                try {
                  request.path = iconFile(icon.get<IconVariant>(), "launcher-" + std::to_string(m_nextId));
                } catch (const sdbus::Error& e) {
                  kLog.warn("launcher icon: {}", e.what());
                }
                auto pending = std::make_unique<Pending>();
                pending->withResults = std::move(result);
                pending->request = request;
                pending->icon = std::move(icon);
                const std::uint64_t id = track(handle, std::move(pending));
                kLog.info("launcher \"{}\" from {}", request.name, request.appId);
                m_handlers.ask(id, request);
              }),
          // Only host apps may install without a dialog; a sandboxed caller always gets PrepareInstall's sheet.
          sdbus::registerMethod("RequestInstallToken")
              .withInputParamNames("app_id", "options")
              .withOutputParamNames("response")
              .implementedAs([](const std::string& appId, const Options&) {
                kLog.info("install token for {} refused", appId);
                return std::uint32_t{2};
              }),
          sdbus::registerProperty("SupportedLauncherTypes").withGetter([] { return kLauncherApp | kLauncherWebApp; }),
          sdbus::registerProperty("version").withGetter([] { return 1U; })
      )
      .forInterface(kLauncher);
}

SystemPortals::~SystemPortals() {
  for (auto& [id, pending] : m_pending) {
    if (pending->withResults) {
      pending->withResults->returnResults(std::uint32_t{2}, Options{});
    } else if (pending->plain) {
      pending->plain->returnResults(std::uint32_t{2});
    }
  }
}

std::uint64_t SystemPortals::track(const std::string& handle, std::unique_ptr<Pending> pending) {
  const std::uint64_t id = m_nextId++;
  pending->requestObject = sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{handle});
  pending->requestObject
      ->addVTable(sdbus::registerMethod("Close").implementedAs([this, id] {
        // The caller gave up: end the call; the Request object goes after this callback, which it owns.
        const auto it = m_pending.find(id);
        if (it == m_pending.end()) {
          return;
        }
        if (it->second->withResults) {
          it->second->withResults->returnResults(std::uint32_t{2}, Options{});
          it->second->withResults.reset();
        } else if (it->second->plain) {
          it->second->plain->returnResults(std::uint32_t{2});
          it->second->plain.reset();
        }
        DeferredCall::callLater([this, id, alive = std::weak_ptr<bool>(m_alive)] {
          if (alive.expired()) {
            return;
          }
          m_pending.erase(id);
          if (m_handlers.closed) {
            m_handlers.closed(id);
          }
        });
      }))
      .forInterface(kRequest);
  m_pending.emplace(id, std::move(pending));
  return id;
}

void SystemPortals::answer(std::uint64_t id, std::uint32_t response, const std::string& name) {
  const auto it = m_pending.find(id);
  if (it == m_pending.end()) {
    return;
  }
  // The Request object stays until this call returns: it may be the one running.
  auto pending = std::move(it->second);
  m_pending.erase(it);
  const Request& r = pending->request;
  switch (r.kind) {
  case Kind::Account: {
    Options results;
    if (response == 0) {
      const User user = currentUser();
      results["id"] = sdbus::Variant(user.id);
      results["name"] = sdbus::Variant(user.name);
      gchar* uri = user.image.empty() ? nullptr : g_filename_to_uri(user.image.c_str(), nullptr, nullptr);
      results["image"] = sdbus::Variant(std::string(uri != nullptr ? uri : ""));
      g_free(uri);
    }
    kLog.info("account answer {}", response);
    if (pending->withResults) {
      pending->withResults->returnResults(response, results);
    }
    break;
  }
  case Kind::Wallpaper: {
    if (response == 0 && r.setOn == "lockscreen") {
      // The lock screen picture is set in Settings › Lock Screen; the shell has no message for it yet.
      kLog.warn("wallpaper: lock screen only is not supported");
      response = 2;
    }
    if (response == 0) {
      const gchar* argv[] = {"andromeda-shell", "msg", "wallpaper-set", r.path.c_str(), nullptr};
      GError* error = nullptr;
      if (!g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                         &error)) {
        kLog.warn("wallpaper set: {}", error != nullptr ? error->message : "?");
        g_clear_error(&error);
        response = 2;
      } else {
        kLog.info("wallpaper set {}", r.path);
      }
    }
    if (pending->plain) {
      pending->plain->returnResults(response);
    }
    break;
  }
  case Kind::Launcher: {
    Options results;
    if (response == 0) {
      results["name"] = sdbus::Variant(name.empty() ? r.name : name);
      if (pending->icon) {
        results["icon"] = *pending->icon;
      }
    }
    kLog.info("launcher answer {} \"{}\"", response, name.empty() ? r.name : name);
    if (pending->withResults) {
      pending->withResults->returnResults(response, results);
    }
    break;
  }
  }
}

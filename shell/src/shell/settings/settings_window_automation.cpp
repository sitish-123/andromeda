#include "config/config_service.h"
#include "core/process/process.h"
#include "core/shared_worker.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_automation.h"
#include "shell/settings/settings_content_services.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_window.h"
#include "system/app_permissions.h"
#include "system/default_apps.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <utility>

namespace {

  using Overrides = std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>;

  const AppPermissions::Kind kBackgroundKind{.table = "background", .id = "background"};

  bool startsAtLogin(const std::string& app) {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir = config != nullptr && *config != '\0' ? std::filesystem::path(config)
        : home != nullptr                                                   ? std::filesystem::path(home) / ".config"
                                                                            : std::filesystem::path{};
    std::error_code ec;
    return !dir.empty() && std::filesystem::exists(dir / "autostart" / (app + ".desktop"), ec);
  }

  // The event data HookManager passes for each kind, with sample values, so a Test run sees what a real one sees.
  std::vector<process::EnvOverride> sampleHookEnv(HookKind kind) {
    switch (kind) {
    case HookKind::WallpaperChanged:
      return {{"ANDROMEDA_WALLPAPER_PATH", "/usr/share/backgrounds/andromeda.png"}, {"ANDROMEDA_WALLPAPER_CONNECTOR", "eDP-1"}};
    case HookKind::ThemeModeChanged:
      return {{"ANDROMEDA_THEME_MODE", "dark"}, {"ANDROMEDA_THEME_MODE_PREVIOUS", "light"},
              {"ANDROMEDA_THEME_MODE_CONFIGURED", "auto"}};
    case HookKind::PowerProfileChanged:
      return {{"ANDROMEDA_POWER_PROFILE", "performance"}, {"ANDROMEDA_POWER_PROFILE_PREVIOUS", "balanced"},
              {"ANDROMEDA_POWER_PROFILE_ORIGIN", "user"}};
    case HookKind::BatteryCharging:
    case HookKind::BatteryDischarging:
    case HookKind::BatteryPlugged:
    case HookKind::BatteryPercentageChanged:
      return {{"ANDROMEDA_BATTERY_STATE", kind == HookKind::BatteryDischarging ? "discharging" : "charging"},
              {"ANDROMEDA_BATTERY_PERCENT", "80"}};
    default:
      return {};
    }
  }

  std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  // MprisService's match: a blacklist token equals the identity or bus suffix, or is part of the bus name.
  bool playerHidden(const std::vector<std::string>& blacklist, const std::string& bus, const std::string& identity) {
    const std::string b = lowered(bus);
    const std::string id = lowered(identity);
    return std::ranges::any_of(blacklist, [&](const std::string& raw) {
      const std::string token = lowered(StringUtils::trim(raw));
      return !token.empty() && (token == id || token == b || b.contains(token));
    });
  }

  // (bus name suffix, Identity) of each MPRIS player on the session bus; blocking: runs on the worker.
  std::vector<std::pair<std::string, std::string>> listMediaPlayers() {
    constexpr std::string_view kPrefix = "org.mpris.MediaPlayer2.";
    constexpr int kTimeoutMs = 500;
    std::vector<std::pair<std::string, std::string>> out;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus == nullptr) {
      return out;
    }
    GVariant* names = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"),
                                                  G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, nullptr);
    if (names != nullptr) {
      GVariantIter* iter = nullptr;
      g_variant_get(names, "(as)", &iter);
      const char* name = nullptr;
      while (g_variant_iter_loop(iter, "&s", &name)) {
        const std::string_view busName = name;
        if (!busName.starts_with(kPrefix)) {
          continue;
        }
        std::string identity(busName.substr(kPrefix.size()));
        if (GVariant* reply = g_dbus_connection_call_sync(
                bus, name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", "org.mpris.MediaPlayer2", "Identity"), G_VARIANT_TYPE("(v)"),
                G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr, nullptr
            );
            reply != nullptr) {
          GVariant* value = nullptr;
          g_variant_get(reply, "(v)", &value);
          if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            identity = g_variant_get_string(value, nullptr);
          }
          g_variant_unref(value);
          g_variant_unref(reply);
        }
        out.emplace_back(std::string(busName.substr(kPrefix.size())), std::move(identity));
      }
      g_variant_iter_free(iter);
      g_variant_unref(names);
    }
    g_object_unref(bus);
    return out;
  }

} // namespace

void SettingsWindow::addAutomationContent(float scale) {
  if (m_selectedSection != "automation" || !m_detail.empty() || m_config == nullptr) {
    return;
  }
  settings::addSettingsAutomation(
      *m_contentContainer,
      settings::SettingsAutomationContext{
          .scale = scale,
          .hooks = &m_config->config().hooks,
          .openRule = [this](std::optional<std::size_t> hook,
                             std::optional<std::size_t> command) { openHookRuleEditor(hook, command); },
          .setEnabled =
              [this](std::size_t hook, std::string command, bool enabled) {
                if (m_config == nullptr) {
                  return;
                }
                auto disabled = m_config->config().hooks.disabled[static_cast<std::size_t>(kHookKinds[hook].value)];
                std::erase(disabled, command);
                if (!enabled) {
                  disabled.push_back(std::move(command));
                }
                setSettingOverride({"hooks", "disabled", std::string(kHookKinds[hook].key)}, std::move(disabled));
                requestContentRebuild();
              },
      }
  );
}

void SettingsWindow::openHookRuleEditor(std::optional<std::size_t> hookIndex, std::optional<std::size_t> commandIndex) {
  if (m_config == nullptr) {
    return;
  }
  if (m_editorSheetModal == nullptr) {
    m_editorSheetModal = std::make_unique<settings::SettingsSheetModal>();
    m_editorSheetModal->initialize(m_modalHost, [this] { dismissOpenSelectDropdown(); });
  } else if (m_editorSheetModal->isOpen()) {
    m_editorSheetModal->close();
  }

  const auto commandsOf = [this](std::size_t hook) {
    return m_config->config().hooks.commands[static_cast<std::size_t>(kHookKinds[hook].value)];
  };
  const auto disabledOf = [this](std::size_t hook) {
    return m_config->config().hooks.disabled[static_cast<std::size_t>(kHookKinds[hook].value)];
  };
  const auto hookPath = [](std::size_t hook) {
    return std::vector<std::string>{"hooks", std::string(kHookKinds[hook].key)};
  };
  const auto disabledPath = [](std::size_t hook) {
    return std::vector<std::string>{"hooks", "disabled", std::string(kHookKinds[hook].key)};
  };

  struct Draft {
    std::size_t hook = static_cast<std::size_t>(HookKind::SessionLocked);
    std::string command;
  };
  auto draft = std::make_shared<Draft>();
  // The edited rule is found again by its event and text: a config reload may have moved or removed it.
  std::optional<std::size_t> originalHook;
  std::string original;
  if (hookIndex.has_value() && commandIndex.has_value() && *hookIndex < std::size(kHookKinds)) {
    const auto commands = commandsOf(*hookIndex);
    if (*commandIndex >= commands.size()) {
      return;
    }
    originalHook = *hookIndex;
    original = commands[*commandIndex];
    draft->hook = *hookIndex;
    draft->command = original;
  }
  const bool editing = originalHook.has_value();

  // Takes the original rule out of `out`; false when it is gone.
  const auto removeOriginal = [=](Overrides& out) {
    auto commands = commandsOf(*originalHook);
    const auto it = std::ranges::find(commands, original);
    if (it == commands.end()) {
      return false;
    }
    commands.erase(it);
    out.emplace_back(hookPath(*originalHook), std::move(commands));
    if (auto disabled = disabledOf(*originalHook); std::erase(disabled, original) > 0) {
      out.emplace_back(disabledPath(*originalHook), std::move(disabled));
    }
    return true;
  };
  const auto ruleGone = [this] {
    m_editorSheetModal->setStatusMessage(i18n::tr("settings.automation.rule-gone"), true);
  };

  const auto save = [=, this] {
    const std::string command = StringUtils::trim(draft->command);
    if (command.empty()) {
      m_editorSheetModal->setStatusMessage(i18n::tr("settings.automation.command-empty"), true);
      return;
    }
    Overrides overrides;
    if (editing && *originalHook == draft->hook) {
      auto commands = commandsOf(draft->hook);
      const auto it = std::ranges::find(commands, original);
      if (it == commands.end()) {
        ruleGone();
        return;
      }
      *it = command;
      overrides.emplace_back(hookPath(draft->hook), std::move(commands));
      if (auto disabled = disabledOf(draft->hook); std::ranges::contains(disabled, original)) {
        std::ranges::replace(disabled, original, command);
        overrides.emplace_back(disabledPath(draft->hook), std::move(disabled));
      }
    } else {
      if (editing && !removeOriginal(overrides)) {
        ruleGone();
        return;
      }
      auto commands = commandsOf(draft->hook);
      commands.push_back(command);
      overrides.emplace_back(hookPath(draft->hook), std::move(commands));
    }
    setSettingOverrides(std::move(overrides));
    m_editorSheetModal->close();
    requestContentRebuild();
  };

  // A Test result shows only on the sheet that started it.
  auto sheetOpen = std::make_shared<bool>(true);
  const float scale = uiScale();
  std::vector<std::string> events;
  for (std::size_t i = 0; i < std::size(kHookKinds); ++i) {
    events.push_back(settings::automationEventText(i));
  }
  m_editorSheetModal->open(settings::SettingsSheetRequest{
      .sheetTitle = i18n::tr(editing ? "settings.automation.edit-title" : "settings.automation.new-title"),
      .removeAction = editing ? std::function<void()>([=, this] {
        Overrides overrides;
        if (!removeOriginal(overrides)) {
          ruleGone();
          return;
        }
        setSettingOverrides(std::move(overrides));
        m_editorSheetModal->close();
        requestContentRebuild();
      })
                              : std::function<void()>{},
      .populateSheetBody =
          [this, draft, save, scale, events = std::move(events), editing, sheetOpen](Flex& body) {
            using namespace settings::page_rows;
            const auto field = [&](std::string label, std::unique_ptr<Node> control) {
              auto r = row(scale);
              r->setMinHeight(Style::controlHeight * scale);
              r->addChild(title(std::move(label), scale));
              r->addChild(std::move(control));
              body.addChild(std::move(r));
            };
            field(i18n::tr("settings.automation.when-label"),
                  ui::select({
                      .options = events,
                      .selectedIndex = draft->hook,
                      .fontSize = Style::fontSizeBody * scale,
                      .controlHeight = Tokens::sizeControlS * scale,
                      .glyphSize = Tokens::sizeSymbolInline * scale,
                      .width = 240.0F * scale,
                      .onSelectionChanged = [draft](std::size_t index, std::string_view) { draft->hook = index; },
                  }));
            field(i18n::tr("settings.automation.do-label"), valueText(i18n::tr("settings.automation.run-command"), scale));
            body.addChild(ui::input({
                .value = draft->command,
                .placeholder = i18n::tr("settings.schema.hooks.command-placeholder"),
                .fontSize = Tokens::typeFootnote.size * scale,
                .controlHeight = Style::controlHeight * scale,
                .horizontalPadding = Style::spaceSm * scale,
                .onChange = [draft](const std::string& text) { draft->command = text; },
                .onSubmit = [save](const std::string&) { save(); },
            }));
            body.addChild(ui::label({
                .text = i18n::tr("settings.automation.sheet-hint"),
                .fontSize = Tokens::typeFootnote.size * scale,
                .color = colorSpecFromRole(ColorRole::Label2),
                .maxLines = 3,
            }));
            const auto test = [this, draft, sheetOpen] {
              const std::string command = StringUtils::trim(draft->command);
              if (command.empty()) {
                return;
              }
              const auto show = [this, sheetOpen](bool ok, int code) {
                if (*sheetOpen && m_editorSheetModal != nullptr) {
                  m_editorSheetModal->setStatusMessage(
                      ok ? i18n::tr("settings.automation.test-ok")
                         : i18n::tr("settings.automation.test-failed", "code", std::to_string(code)),
                      !ok
                  );
                }
              };
              constexpr auto kTestTimeout = std::chrono::seconds(10);
              const bool started = process::runAsync(
                  command,
                  process::RunCallbacks{.onExit = [show](process::RunResult result) {
                    show(static_cast<bool>(result), result.timedOut ? -1 : result.exitCode);
                  }},
                  process::RunOptions{.timeout = kTestTimeout,
                                      .env = sampleHookEnv(kHookKinds[draft->hook].value)}
              );
              if (!started) {
                show(false, -1);
              }
            };
            auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
            actions->addChild(plainButton(i18n::tr("settings.automation.test"), ButtonVariant::Default, scale, test));
            actions->addChild(ui::spacer());
            actions->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Default, scale,
                                          [this] { m_editorSheetModal->close(); }));
            actions->addChild(plainButton(i18n::tr(editing ? "settings.automation.save" : "settings.automation.add-action"),
                                          ButtonVariant::Primary, scale, save));
            body.addChild(std::move(actions));
          },
      .scale = scale,
      .minWidth = 440.0F * scale,
      .maxWidth = 520.0F * scale,
      .onClosed = [sheetOpen] { *sheetOpen = false; },
  });
}

void SettingsWindow::addServicesContent(float scale) {
  if (m_selectedSection != "services") {
    m_servicesRequested = false;
    return;
  }
  if (!m_detail.empty() || m_config == nullptr) {
    return;
  }
  if (!m_servicesRequested) {
    m_servicesRequested = true;
    m_backgroundLoaded = false;
    if (m_permissions == nullptr && m_sessionBus != nullptr) {
      m_permissions = std::make_unique<AppPermissions>(*m_sessionBus);
    }
    if (m_permissions != nullptr) {
      m_permissions->load(kBackgroundKind, [this](AppPermissions::Grants grants) {
        m_backgroundGrants = std::move(grants);
        m_backgroundLoaded = true;
        if (isOpen() && m_selectedSection == "services") {
          requestContentRebuild();
        }
      });
    }
    m_mediaPlayers.clear();
    SharedWorker::instance().post<std::vector<std::pair<std::string, std::string>>>(
        std::weak_ptr<void>(m_aliveGuard), [] { return listMediaPlayers(); },
        [this](std::vector<std::pair<std::string, std::string>> players) {
          m_mediaPlayers = std::move(players);
          if (isOpen() && m_selectedSection == "services") {
            requestContentRebuild();
          }
        }
    );
  }

  const auto entries = desktopEntriesSnapshot();
  const auto findEntry = [&](const std::string& id) -> const DesktopEntry* {
    for (const auto& entry : *entries) {
      if (entry.id == id || entry.id == id + ".desktop") {
        return &entry;
      }
    }
    return nullptr;
  };

  const std::vector<std::string>& blacklist = m_config->config().shell.mpris.blacklist;
  const auto setHidden = [this](std::string bus, std::string identity, bool shown) {
    if (m_config == nullptr) {
      return;
    }
    auto next = m_config->config().shell.mpris.blacklist;
    std::erase_if(next, [&](const std::string& token) { return playerHidden({token}, bus, identity); });
    if (!shown) {
      next.push_back(lowered(identity));
    }
    setSettingOverride({"shell", "mpris", "blacklist"}, std::move(next));
    requestContentRebuild();
  };
  std::vector<settings::SettingsServicesContext::Player> players;
  for (const auto& [bus, identity] : m_mediaPlayers) {
    players.push_back({.label = identity,
                       .shown = !playerHidden(blacklist, bus, identity),
                       .setShown = [setHidden, bus, identity](bool shown) { setHidden(bus, identity, shown); }});
  }
  for (const auto& token : blacklist) {
    const bool running = std::ranges::any_of(m_mediaPlayers, [&](const auto& player) {
      return playerHidden({token}, player.first, player.second);
    });
    if (!running) {
      players.push_back({.label = token, .shown = false,
                         .setShown = [setHidden, token](bool shown) { setHidden(token, token, shown); }});
    }
  }

  std::vector<settings::SettingsServicesContext::BackgroundApp> background;
  for (const auto& grant : m_backgroundGrants) {
    const DesktopEntry* entry = findEntry(grant.app);
    background.push_back({
        .id = grant.app,
        .label = grant.app.empty() ? i18n::tr("settings.privacy.host-apps")
            : entry != nullptr     ? entry->name
                                   : grant.app,
        .icon = entry != nullptr ? entry->icon : std::string{},
        .allowed = grant.allowed,
        .startsAtLogin = !grant.app.empty() && startsAtLogin(grant.app),
    });
  }

  std::vector<settings::SettingsServicesContext::DefaultApp> defaults;
  const std::pair<default_apps::Category, const char*> kDefaults[] = {
      {default_apps::Category::WebBrowser, "settings.default-apps.web-browser"},
      {default_apps::Category::Mail, "settings.default-apps.mail"},
      {default_apps::Category::FileManager, "settings.default-apps.file-manager"},
  };
  for (const auto& [category, key] : kDefaults) {
    const std::string id = default_apps::effectiveDefault(category);
    const DesktopEntry* entry = id.empty() ? nullptr : findEntry(id);
    defaults.push_back({.label = i18n::tr(key),
                        .app = entry != nullptr ? entry->name : id.substr(0, id.rfind(".desktop"))});
  }

  settings::addSettingsServices(
      *m_contentContainer,
      settings::SettingsServicesContext{
          .scale = scale,
          .players = std::move(players),
          .playersLoaded = true,
          .background = std::move(background),
          .backgroundLoaded = m_backgroundLoaded,
          .setBackground =
              [this](std::string app, bool allowed) {
                const auto it = std::ranges::find(m_backgroundGrants, app, &AppPermissions::Grant::app);
                if (it == m_backgroundGrants.end() || m_permissions == nullptr) {
                  return;
                }
                m_permissions->setAllowed(kBackgroundKind, *it, allowed, [this, app, allowed](bool ok) {
                  if (const auto g = std::ranges::find(m_backgroundGrants, app, &AppPermissions::Grant::app);
                      ok && g != m_backgroundGrants.end()) {
                    g->allowed = allowed;
                  }
                  if (isOpen()) {
                    requestContentRebuild();
                  }
                });
              },
          .appIcon = [this](const std::string& icon, float size) -> std::unique_ptr<Node> {
            // Function-local: the icon-theme walk must not run before main().
            static IconResolver resolver;
            if (m_surface == nullptr || icon.empty()) {
              return nullptr;
            }
            const std::string& path = resolver.resolve(icon, static_cast<int>(size));
            if (path.empty()) {
              return nullptr;
            }
            Image* image = nullptr;
            auto node = ui::image({.out = &image, .fit = ImageFit::Contain, .width = size, .height = size});
            if (image == nullptr
                || !image->setSourceFile(m_surface->renderTarget().renderer(), path, static_cast<int>(size), true)) {
              return nullptr;
            }
            return node;
          },
          .defaults = std::move(defaults),
          .openDefaultApps = [this] { navigateTo("default-apps"); },
      }
  );
}

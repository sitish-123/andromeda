#include "shell/settings/settings_content_software_update.h"

#include "dbus/andromeda/software_update_service.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/progress_bar.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <format>

namespace settings {

  namespace {

    namespace rows = page_rows;
    using RunState = SoftwareUpdateService::RunState;

    constexpr std::array<std::string_view, 6> kGroups{"andromeda", "system", "apps", "snaps", "firmware", "held"};

    std::string sizeText(std::uint64_t bytes) {
      constexpr double kMb = 1000.0 * 1000.0;
      if (static_cast<double>(bytes) >= 1000.0 * kMb) {
        return std::format("{:.1f} GB", static_cast<double>(bytes) / (1000.0 * kMb));
      }
      return std::format("{:.0f} MB", std::max(1.0, static_cast<double>(bytes) / kMb));
    }

    std::uint64_t groupBytes(const SoftwareUpdateService& service, std::string_view group) {
      std::uint64_t bytes = 0;
      for (const auto& update : service.updates()) {
        bytes += update.group == group ? update.sizeBytes : 0;
      }
      return bytes;
    }

    std::size_t groupCount(const SoftwareUpdateService& service, std::string_view group) {
      return static_cast<std::size_t>(std::ranges::count(service.updates(), group, &SoftwareUpdate::group));
    }

    std::string lastCheckText(std::int64_t when) {
      if (when <= 0) {
        return {};
      }
      const auto then = static_cast<std::time_t>(when);
      std::tm tm{};
      localtime_r(&then, &tm);
      char text[32];
      std::strftime(text, sizeof(text), "%d %b %H:%M", &tm);
      return i18n::tr("settings.update.last-check", "time", std::string(text));
    }

    std::unique_ptr<Flex> caption(std::string_view text, float scale) {
      auto r = rows::row(scale);
      r->addChild(ui::label({.text = std::string(text), .fontSize = Style::fontSizeCaption * scale,
                             .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 3, .flexGrow = 1.0F}));
      return r;
    }

    // "1:r15209.g6451e8bb8-1" -> "r15209", "0.5.2-1" -> "0.5.2": no epoch, commit hash or package release.
    std::string shortVersion(std::string_view version) {
      if (const auto colon = version.find(':'); colon != std::string_view::npos) {
        version.remove_prefix(colon + 1);
      }
      version = version.substr(0, version.rfind('-'));
      // A git build ("r15209.g6451e8bb8"): the commit hash means nothing to the user.
      if (version.starts_with('r')) {
        version = version.substr(0, version.find('.'));
      }
      return std::string(version);
    }

    // Andromeda's own packages (desktop, Link): updated alone, without the Arch news gate.
    void addAndromeda(Flex& content, float scale, const SoftwareUpdateService& service,
                      const SoftwareUpdateActions& actions) {
      const std::size_t count = groupCount(service, "andromeda");
      if (count == 0) {
        return;
      }
      const auto& updates = service.updates();
      const auto meta = std::ranges::find_if(
          updates, [](const SoftwareUpdate& u) { return u.group == "andromeda" && u.name == "andromeda"; });
      Flex* card = rows::addCard(content, scale);
      auto r = rows::row(scale);
      r->addChild(rows::symbol("sparkles", Tokens::sizeSymbol * scale, ColorRole::Label2));
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(makeLabel(meta == updates.end()
                                   ? i18n::tr("settings.update.andromeda-title")
                                   : i18n::tr("settings.update.andromeda-version", "version", shortVersion(meta->newVersion)),
                               Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1), FontWeight::SemiBold));
      copy->addChild(makeLabel(i18n::tr("settings.update.available", "count", std::to_string(count), "size",
                                        sizeText(groupBytes(service, "andromeda"))),
                               Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2)));
      r->addChild(std::move(copy));
      if (service.canInstall() && service.runState() != RunState::Running) {
        r->addChild(rows::plainButton(i18n::tr("settings.update.andromeda-update"), ButtonVariant::Primary, scale,
                                      actions.upgradeAndromeda));
      }
      card->addChild(std::move(r));
    }

    void addNews(Flex& content, float scale, const SoftwareUpdateService& service, const SoftwareUpdateActions& actions) {
      if (service.news().empty()) {
        // Blocked with no item: the feed has not been read yet (offline, or the first check runs).
        if (service.newsBlocksSystem() && service.available() && !service.checking()) {
          Flex* card = rows::addCard(content, scale);
          auto r = rows::row(scale);
          r->addChild(rows::symbol("alert-triangle", Tokens::sizeSymbol * scale, ColorRole::Label2));
          r->addChild(makeLabel(i18n::tr("settings.update.news-unknown"), Style::fontSizeBody * scale,
                                colorSpecFromRole(ColorRole::Label1)));
          r->addChild(ui::spacer());
          r->addChild(rows::plainButton(i18n::tr("settings.update.check"), ButtonVariant::Default, scale, actions.check));
          card->addChild(std::move(r));
        }
        return;
      }
      const auto& item = service.news().front();
      Flex* card = rows::addCard(content, scale);
      auto top = rows::row(scale);
      top->addChild(rows::symbol("alert-triangle", Tokens::sizeSymbol * scale, ColorRole::Label2));
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(makeLabel(i18n::tr("settings.update.news-title"), Style::fontSizeBody * scale,
                               colorSpecFromRole(ColorRole::Label1), FontWeight::SemiBold));
      copy->addChild(makeLabel(item.title, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2)));
      top->addChild(std::move(copy));
      top->addChild(rows::plainButton(i18n::tr("settings.update.news-open"), ButtonVariant::Default, scale,
                                      [open = actions.openLink, link = item.link] { open(link); }));
      if (service.canInstall()) {
        top->addChild(rows::plainButton(i18n::tr("settings.update.news-read"), ButtonVariant::Primary, scale,
                                        actions.markNewsRead));
      }
      card->addChild(std::move(top));
    }

    void addHero(Flex& content, float scale, const SoftwareUpdateService& service, const SoftwareUpdateActions& actions) {
      // Update Now runs the full system upgrade (System and Andromeda); Apps and Firmware rows show their own count.
      const std::size_t count = groupCount(service, "system") + groupCount(service, "andromeda");
      const std::uint64_t bytes = groupBytes(service, "system") + groupBytes(service, "andromeda");
      rows::Hero hero;
      hero.glyph = "download";
      std::unique_ptr<Flex> progress;
      switch (service.runState()) {
      case RunState::Running: {
        hero.on = true;
        hero.title = i18n::tr("settings.update.running");
        hero.status = service.stage().empty()
            ? i18n::tr("settings.update.preparing")
            : std::format("{} {}", i18n::tr("settings.update.stage." + service.stage()), service.stagePackage());
        progress = rows::row(scale);
        auto bar = std::make_unique<ProgressBar>();
        bar->setProgress(static_cast<float>(service.percent()) / 100.0F);
        bar->setSize(0.0F, Tokens::spaceP4 * scale);
        bar->setFlexGrow(1.0F);
        progress->addChild(std::move(bar));
        break;
      }
      case RunState::NeedsChoice:
        hero.title = i18n::tr("settings.update.needs-choice");
        hero.status = i18n::tr("settings.update.needs-choice-detail");
        if (service.canInstall()) {
          auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
          buttons->addChild(rows::plainButton(i18n::tr("settings.update.check"), ButtonVariant::Default, scale,
                                              actions.check));
          buttons->addChild(rows::plainButton(i18n::tr("settings.update.accept"), ButtonVariant::Primary, scale,
                                              actions.accept));
          hero.action = std::move(buttons);
        } else {
          hero.action = rows::plainButton(i18n::tr("settings.update.check"), ButtonVariant::Default, scale,
                                          actions.check);
        }
        break;
      case RunState::Failed:
        // A refused Andromeda-only update leaves the system update as the way forward: show it as available.
        if (!service.needsSystemFirst()) {
          hero.title = i18n::tr("settings.update.failed");
          hero.status = service.resultMessage();
          hero.action = rows::plainButton(i18n::tr("settings.update.try-again"), ButtonVariant::Primary, scale,
                                          actions.retry);
          break;
        }
        [[fallthrough]];
      case RunState::Done:
      case RunState::Idle:
        if (service.restartNeeded()) {
          hero.on = true;
          hero.title = i18n::tr("settings.update.restart-needed");
          hero.action = rows::plainButton(i18n::tr("settings.update.restart"), ButtonVariant::Primary, scale,
                                          actions.restart);
        } else if (service.checking()) {
          hero.title = i18n::tr("settings.update.checking");
        } else if (!service.available()) {
          hero.title = i18n::tr("settings.update.unavailable");
        } else if (count == 0) {
          hero.title = i18n::tr("settings.update.up-to-date");
          hero.status = lastCheckText(service.lastCheck());
          hero.action = rows::plainButton(i18n::tr("settings.update.check"), ButtonVariant::Default, scale,
                                          actions.check);
        } else {
          hero.on = true;
          hero.title = i18n::tr("settings.update.available", "count", std::to_string(count), "size", sizeText(bytes));
          const bool kernel = std::ranges::any_of(service.updates(), [](const SoftwareUpdate& u) {
            return u.group == "system" && (u.name == "linux" || u.name.starts_with("linux-") || u.name == "mesa");
          });
          hero.status = kernel ? i18n::tr("settings.update.restart-after") : lastCheckText(service.lastCheck());
          // The news gate blocks only the System group, but Update Now runs System; wait for "I have read".
          if (service.tonight()) {
            hero.status = i18n::tr("settings.update.tonight-scheduled");
            hero.action = rows::plainButton(i18n::tr("settings.update.tonight-cancel"), ButtonVariant::Default, scale,
                                            [schedule = actions.scheduleTonight] { schedule(false); });
          } else if (service.canInstall()) {
            const bool blocked = service.newsBlocksSystem();
            auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
            if (!blocked) {
              buttons->addChild(rows::plainButton(i18n::tr("settings.update.tonight"), ButtonVariant::Default, scale,
                                                  [schedule = actions.scheduleTonight] { schedule(true); }));
            }
            auto now = rows::plainButton(i18n::tr("settings.update.update-now"), ButtonVariant::Primary, scale,
                                         actions.upgrade);
            static_cast<Button*>(now.get())->setEnabled(!blocked);
            buttons->addChild(std::move(now));
            hero.action = std::move(buttons);
            if (blocked) {
              hero.status = i18n::tr("settings.update.news-first");
            }
            if (service.needsSystemFirst()) {
              hero.status = i18n::tr("settings.update.needs-system");
            }
          }
        }
        break;
      }
      Flex* heroCard = rows::addHero(content, std::move(hero), scale);
      if (progress != nullptr) {
        heroCard->addChild(std::move(progress));
      }
      if (service.runState() == RunState::NeedsChoice) {
        Flex* card = rows::addCard(content, scale);
        for (const auto& question : service.questions()) {
          card->addChild(caption(question, scale));
        }
        addSettingsRowSeparators(*card, scale);
      }
      if (!service.appsError().empty()) {
        Flex* card = rows::addCard(content, scale);
        card->addChild(caption(i18n::tr("settings.update.apps-failed", "error", service.appsError()), scale));
      }
      if (!service.errors().empty() && service.runState() != RunState::Running) {
        Flex* card = rows::addCard(content, scale);
        for (const auto& error : service.errors()) {
          card->addChild(caption(error, scale));
        }
        auto retry = rows::row(scale);
        retry->addChild(ui::spacer());
        retry->addChild(rows::plainButton(i18n::tr("settings.update.check"), ButtonVariant::Default, scale, actions.check));
        card->addChild(std::move(retry));
      }
    }

    void addGroups(Flex& content, float scale, const SoftwareUpdateService& service, const SoftwareUpdateActions& actions) {
      Flex* card = nullptr;
      for (const std::string_view group : kGroups) {
        const std::size_t count = groupCount(service, group);
        if (count == 0) {
          continue;
        }
        if (card == nullptr) {
          card = rows::addCard(content, scale);
        }
        std::string trailing = i18n::tr("settings.update.group-count", "count", std::to_string(count), "size",
                                        sizeText(groupBytes(service, group)));
        if (group == "apps" && service.appsUpdating()) {
          trailing = i18n::tr("settings.update.updating");
        } else if (group == "held") {
          // Held packages (kept back, phased, IgnorePkg) are not downloaded, so their row has no size.
          trailing = std::to_string(count);
        }
        card->addChild(makeSettingsLinkRow(i18n::tr("settings.update.group." + std::string(group)), trailing, scale,
                                           [open = actions.openGroup, g = std::string(group)] { open(g); }));
      }
      if (card != nullptr) {
        addSettingsRowSeparators(*card, scale);
      }
    }

    void addGroupList(Flex& content, float scale, const SoftwareUpdateService& service, std::string_view group) {
      rows::addHeader(content, i18n::tr("settings.update.group." + std::string(group)), nullptr, scale);
      Flex* card = rows::addCard(content, scale);
      for (const auto& update : service.updates()) {
        if (update.group != group) {
          continue;
        }
        const std::string version =
            update.oldVersion.empty() ? update.newVersion : std::format("{} → {}", update.oldVersion, update.newVersion);
        rows::addValueRow(*card, update.name, version, scale);
      }
      addSettingsRowSeparators(*card, scale);
    }

  } // namespace

  void addSoftwareUpdate(
      Flex& content, float scale, const SoftwareUpdateService* service, std::string_view group,
      const std::vector<std::pair<std::string, std::string>>& hookFailures, bool checkDaily,
      const SoftwareUpdateActions& actions
  ) {
    if (service == nullptr) {
      rows::addEmptyState(content, "download", i18n::tr("settings.update.unavailable"), scale);
      return;
    }
    if (!group.empty()) {
      addGroupList(content, scale, *service, group);
      return;
    }
    addAndromeda(content, scale, *service, actions);
    addNews(content, scale, *service, actions);
    addHero(content, scale, *service, actions);
    addGroups(content, scale, *service, actions);
    Flex* options = rows::addCard(content, scale);
    auto daily = rows::row(scale);
    daily->addChild(makeLabel(i18n::tr("settings.update.check-daily"), Style::fontSizeBody * scale,
                              colorSpecFromRole(ColorRole::Label1)));
    daily->addChild(ui::spacer());
    daily->addChild(ui::toggle({.checked = checkDaily, .scale = scale, .onChange = actions.setCheckDaily}));
    options->addChild(std::move(daily));
    const std::string& snapshots = service->snapshots();
    rows::addValueRow(*options, i18n::tr("settings.update.snapshots"),
                      snapshots.empty() ? i18n::tr("settings.update.snapshots-off") : snapshots, scale);
    addSettingsRowSeparators(*options, scale);
    if (!hookFailures.empty()) {
      Flex* card = addSettingsGroupCard(content, i18n::tr("settings.update.hooks"), scale);
      for (const auto& [event, script] : hookFailures) {
        rows::addValueRow(*card, script, i18n::tr("settings.update.hook-failed", "event", event), scale);
      }
      addSettingsRowSeparators(*card, scale);
    }
  }

} // namespace settings

#include "shell/settings/settings_content_users.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/style.h"

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <array>
#include <string>
#include <vector>

namespace settings {
  namespace {

    using namespace page_rows;

    struct UserInfo {
      uid_t uid = 0;
      std::string login;
      std::string name;
      bool admin = false;
    };

    bool isAdmin(const passwd& pw) {
      int count = 64;
      std::vector<gid_t> groups(static_cast<std::size_t>(count));
      if (getgrouplist(pw.pw_name, pw.pw_gid, groups.data(), &count) < 0) {
        groups.resize(static_cast<std::size_t>(count));
        getgrouplist(pw.pw_name, pw.pw_gid, groups.data(), &count);
      }
      groups.resize(static_cast<std::size_t>(std::max(count, 0)));
      // Arch and Fedora use wheel; Debian and Ubuntu use sudo (older Ubuntu: admin).
      return std::ranges::any_of(std::array{"wheel", "sudo", "admin"}, [&](const char* name) {
        const group* g = getgrnam(name);
        return g != nullptr && std::ranges::contains(groups, g->gr_gid);
      });
    }

    // Login users: the uid range useradd gives people, and a real shell.
    std::vector<UserInfo> loginUsers() {
      constexpr uid_t kFirstUid = 1000;
      constexpr uid_t kLastUid = 60000;
      std::vector<UserInfo> users;
      setpwent();
      while (const passwd* pw = getpwent()) {
        const std::string shell = pw->pw_shell != nullptr ? pw->pw_shell : "";
        if (pw->pw_uid < kFirstUid || pw->pw_uid > kLastUid || shell.ends_with("nologin") || shell.ends_with("false")) {
          continue;
        }
        std::string gecos = pw->pw_gecos != nullptr ? pw->pw_gecos : "";
        gecos = gecos.substr(0, gecos.find(','));
        users.push_back(UserInfo{
            .uid = pw->pw_uid, .login = pw->pw_name, .name = gecos.empty() ? pw->pw_name : gecos, .admin = isAdmin(*pw)
        });
      }
      endpwent();
      return users;
    }

    std::unique_ptr<Node> avatar(const UserInfo& user, float size, float scale) {
      // First UTF-8 character: a lead byte, then its continuation bytes.
      std::size_t length = user.name.empty() ? 0 : 1;
      while (length < user.name.size() && (static_cast<unsigned char>(user.name[length]) & 0xC0U) == 0x80U) {
        ++length;
      }
      const std::string initial = length == 0 ? std::string("?") : user.name.substr(0, length);
      return ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = frameColors().avatarFill,
           .radius = size * 0.5F * scale,
           .width = size * scale,
           .height = size * scale},
          makeLabel(initial, size * 0.45F * scale, frameColors().avatarInitial, FontWeight::SemiBold)
      );
    }

    std::string roleText(const UserInfo& user) {
      return i18n::tr(user.admin ? "settings.users.admin" : "settings.users.standard");
    }

    std::unique_ptr<Node> formInput(
        std::string value, std::string placeholder, bool password, float scale, std::function<void(std::string)> onChange,
        std::function<void()> onSubmit
    ) {
      constexpr float kFieldWidth = 240.0F;
      return ui::input({
          .value = std::move(value),
          .placeholder = std::move(placeholder),
          .passwordMode = password,
          .width = kFieldWidth * scale,
          .onChange = [onChange = std::move(onChange)](const std::string& text) { onChange(text); },
          .onSubmit = [onSubmit = std::move(onSubmit)](const std::string&) { onSubmit(); },
      });
    }

    void addFormRow(Flex& card, std::string_view label, std::unique_ptr<Node> control, float scale) {
      auto r = row(scale);
      r->addChild(title(std::string(label), scale));
      r->addChild(std::move(control));
      card.addChild(std::move(r));
    }

    void addNewUserPage(Flex& content, const SettingsUsersContext& ctx) {
      const float scale = ctx.scale;
      NewUserDraft* draft = ctx.draft;
      Flex* card = addCard(content, scale);
      // The account name follows the full name until the user types in it; the input is set in place, as a
      // rebuild per key would drop the focus.
      auto login = formInput(draft->login, {}, false, scale, [draft](std::string t) {
        draft->login = std::move(t);
        draft->loginEdited = true;
      }, ctx.create);
      auto* loginInput = static_cast<Input*>(login.get());
      addFormRow(*card, i18n::tr("settings.users.full-name"),
                 formInput(draft->fullName, {}, false, scale, [draft, loginInput](std::string t) {
                   draft->fullName = std::move(t);
                   if (!draft->loginEdited) {
                     draft->login = loginFromName(draft->fullName);
                     loginInput->setValue(draft->login);
                   }
                 }, ctx.create),
                 scale);
      addFormRow(*card, i18n::tr("settings.users.account-name"), std::move(login), scale);
      addFormRow(*card, i18n::tr("settings.users.account-type"),
                 ui::segmented({
                     .options = std::vector<ui::SegmentedOption>{
                         ui::SegmentedOption{.label = i18n::tr("settings.users.standard")},
                         ui::SegmentedOption{.label = i18n::tr("settings.users.administrator")}},
                     .selectedIndex = draft->admin ? std::size_t{1} : std::size_t{0},
                     .scale = scale,
                     .onChange = [draft](std::size_t index) { draft->admin = index == 1; },
                 }),
                 scale);

      Flex* secret = addCard(content, scale);
      addFormRow(*secret, i18n::tr("settings.users.password"),
                 formInput(draft->password, {}, true, scale, [draft](std::string t) { draft->password = std::move(t); },
                           ctx.create),
                 scale);
      addFormRow(*secret, i18n::tr("settings.users.verify"),
                 formInput(draft->verify, {}, true, scale, [draft](std::string t) { draft->verify = std::move(t); },
                           ctx.create),
                 scale);

      auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      const std::string status = draft->busy ? i18n::tr("settings.users.creating") : draft->error;
      actions->addChild(ui::label({
          .text = status,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(draft->busy ? ColorRole::Label2 : ColorRole::DangerText),
          .maxLines = 2,
          .flexGrow = 1.0F,
      }));
      if (!draft->busy) {
        actions->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, scale, ctx.back));
        actions->addChild(plainButton(i18n::tr("settings.users.create"), ButtonVariant::Primary, scale, ctx.create));
      }
      content.addChild(std::move(actions));
    }

    void addUserPage(Flex& content, const SettingsUsersContext& ctx, const std::vector<UserInfo>& users) {
      const float scale = ctx.scale;
      const std::string_view key = std::string_view(ctx.detail).substr(5);
      const auto tab = key.find('\t');
      const uid_t uid = tab == std::string_view::npos ? 0 : static_cast<uid_t>(std::stoul(std::string(key.substr(tab + 1))));
      const auto user = std::ranges::find(users, uid, &UserInfo::uid);
      if (tab == std::string_view::npos || user == users.end() || uid == getuid()) {
        return;
      }
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      r->setMinHeight(64.0F * scale);
      r->setGap(Style::spaceMd * scale);
      r->addChild(avatar(*user, 48.0F, scale));
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(makeLabel(user->name, Tokens::typeTitle3.size * scale, colorSpecFromRole(ColorRole::Label1),
                               FontWeight::SemiBold));
      copy->addChild(makeLabel(user->login + " · " + roleText(*user), Style::fontSizeCaption * scale,
                               colorSpecFromRole(ColorRole::Label2)));
      r->addChild(std::move(copy));
      card->addChild(std::move(r));

      auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      const std::string status = ctx.deleteBusy ? i18n::tr("settings.users.creating") : ctx.deleteError;
      actions->addChild(ui::label({
          .text = status,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ctx.deleteError.empty() ? ColorRole::Label2 : ColorRole::DangerText),
          .maxLines = 2,
          .flexGrow = 1.0F,
      }));
      if (!ctx.deleteBusy) {
        actions->addChild(plainButton(i18n::tr("settings.users.delete-ellipsis"), ButtonVariant::Destructive, scale,
                                      [confirm = ctx.confirmDelete, uid, login = user->login] { confirm(uid, login); }));
      }
      content.addChild(std::move(actions));
    }

  } // namespace

  std::string loginFromName(const std::string& fullName) {
    std::string login;
    for (const char c : fullName.substr(0, fullName.find(' '))) {
      const auto u = static_cast<unsigned char>(c);
      if (std::isalnum(u) != 0 && u < 0x80U) {
        login.push_back(static_cast<char>(std::tolower(u)));
      }
    }
    return validLogin(login) ? login : std::string{};
  }

  bool validLogin(const std::string& login) {
    constexpr std::size_t kMaxLogin = 32;
    if (login.empty() || login.size() > kMaxLogin || std::islower(static_cast<unsigned char>(login[0])) == 0) {
      return false;
    }
    return std::ranges::all_of(login, [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
  }

  void addSettingsUsers(Flex& content, const SettingsUsersContext& ctx) {
    const float scale = ctx.scale;
    if (ctx.detail.starts_with("users:") && ctx.draft != nullptr) {
      addNewUserPage(content, ctx);
      return;
    }
    const auto users = loginUsers();
    if (ctx.detail.starts_with("user:")) {
      addUserPage(content, ctx, users);
      return;
    }
    const uid_t me = getuid();
    const auto self = std::ranges::find(users, me, &UserInfo::uid);
    if (self != users.end()) {
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      r->setMinHeight(64.0F * scale);
      r->setGap(Style::spaceMd * scale);
      r->addChild(avatar(*self, 48.0F, scale));
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(makeLabel(self->name, Tokens::typeTitle3.size * scale, colorSpecFromRole(ColorRole::Label1),
                               FontWeight::SemiBold));
      copy->addChild(makeLabel(std::string(self->login) + " · " + roleText(*self), Style::fontSizeCaption * scale,
                               colorSpecFromRole(ColorRole::Label2)));
      r->addChild(std::move(copy));
      card->addChild(std::move(r));
    }

    std::vector<UserInfo> others;
    std::ranges::copy_if(users, std::back_inserter(others), [me](const UserInfo& u) { return u.uid != me; });
    addHeader(content, i18n::tr("settings.users.others"), nullptr, scale);
    Flex* card = addCard(content, scale);
    if (others.empty()) {
      auto r = row(scale);
      r->addChild(ui::label({.text = i18n::tr("settings.users.none"), .fontSize = Style::fontSizeBody * scale,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
      card->addChild(std::move(r));
    }
    for (const auto& user : others) {
      auto r = row(scale);
      r->addChild(avatar(user, 24.0F, scale));
      r->addChild(title(user.name, scale));
      r->addChild(ui::label({.text = roleText(user), .fontSize = Style::fontSizeBody * scale,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
      if (ctx.deleteUser) {
        r->addChild(iconButton("chevron-right", i18n::tr("settings.wifi.details"), scale,
                               [open = ctx.openDetail, detail = "user:" + user.name + "\t" + std::to_string(user.uid)] {
                                 open(detail);
                               }));
      }
      card->addChild(std::move(r));
    }
    if (ctx.draft != nullptr) {
      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .fillWidth = true});
      actions->addChild(plainButton(i18n::tr("settings.users.add"), ButtonVariant::Default, scale,
                                    [open = ctx.openDetail] { open("users:" + i18n::tr("settings.users.new-user")); }));
      content.addChild(std::move(actions));
    }
  }

} // namespace settings

#include "shell/settings/settings_content_users.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
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

  } // namespace

  void addSettingsUsers(Flex& content, float scale) {
    const auto users = loginUsers();
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
      return;
    }
    for (const auto& user : others) {
      auto r = row(scale);
      r->addChild(avatar(user, 24.0F, scale));
      r->addChild(title(user.name, scale));
      r->addChild(ui::label({.text = roleText(user), .fontSize = Style::fontSizeBody * scale,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
      card->addChild(std::move(r));
    }
  }

} // namespace settings

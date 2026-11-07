#include "system/app_permissions.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <algorithm>
#include <map>
#include <sdbus-c++/sdbus-c++.h>

namespace {

  constexpr Logger kLog("app-permissions");
  const sdbus::ServiceName kStoreName{"org.freedesktop.impl.portal.PermissionStore"};
  const sdbus::ObjectPath kStorePath{"/org/freedesktop/impl/portal/PermissionStore"};
  constexpr auto kStoreInterface = "org.freedesktop.impl.portal.PermissionStore";

  using Table = std::map<std::string, std::vector<std::string>>;

  // "no" for devices and screenshots, "NONE" for a location accuracy.
  bool allows(const std::vector<std::string>& values) {
    return !values.empty() && values.front() != "no" && values.front() != "NONE";
  }

  void addGrant(AppPermissions::Grants& grants, const std::string& app, const std::vector<std::string>& values,
                const std::string& id) {
    auto it = std::ranges::find(grants, app, &AppPermissions::Grant::app);
    if (it == grants.end()) {
      grants.push_back({.app = app, .allowed = allows(values), .ids = {id}, .values = {values}});
      return;
    }
    it->allowed = it->allowed || allows(values);
    it->ids.push_back(id);
    it->values.push_back(values);
  }

} // namespace

AppPermissions::AppPermissions(SessionBus& bus) {
  try {
    m_store = sdbus::createProxy(bus.connection(), kStoreName, kStorePath);
  } catch (const sdbus::Error& e) {
    kLog.info("no permission store: {}", e.what());
  }
}

AppPermissions::~AppPermissions() { *m_alive = false; }

const std::vector<AppPermissions::Kind>& AppPermissions::kinds() {
  static const std::vector<Kind> kKinds = {
      {.table = "devices", .id = "camera"},
      {.table = "location", .id = "location"},
      {.table = "screenshot", .id = "screenshot"},
      {.table = "screencast", .id = {}},
      {.table = "remote-desktop", .id = {}},
  };
  return kKinds;
}

void AppPermissions::load(const Kind& kind, std::function<void(Grants)> done) {
  if (m_store == nullptr) {
    done({});
    return;
  }
  // Lookups of every entry fill one shared result; the last reply hands it over.
  struct Pending {
    Grants grants;
    std::size_t left = 0;
    std::function<void(Grants)> done;
  };
  auto lookup = [this, kind](const std::shared_ptr<Pending>& pending, const std::string& id) {
    std::weak_ptr<bool> alive = m_alive;
    m_store->callMethodAsync("Lookup")
        .onInterface(kStoreInterface)
        .withArguments(kind.table, id)
        .uponReplyInvoke([pending, id, alive](std::optional<sdbus::Error> e, const Table& table, const sdbus::Variant&) {
          if (alive.expired()) {
            return;
          }
          if (!e.has_value()) {
            for (const auto& [app, values] : table) {
              addGrant(pending->grants, app, values, id);
            }
          }
          if (--pending->left == 0) {
            pending->done(std::move(pending->grants));
          }
        });
  };
  try {
    if (!kind.id.empty()) {
      auto pending = std::make_shared<Pending>(Pending{.left = 1, .done = std::move(done)});
      lookup(pending, kind.id);
      return;
    }
    std::weak_ptr<bool> alive = m_alive;
    m_store->callMethodAsync("List")
        .onInterface(kStoreInterface)
        .withArguments(kind.table)
        .uponReplyInvoke([alive, lookup, done = std::move(done)](std::optional<sdbus::Error> e,
                                                                 const std::vector<std::string>& ids) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value() || ids.empty()) {
            done({});
            return;
          }
          // sdbus calls the handler as const: copy `done`, do not move it.
          auto pending = std::make_shared<Pending>(Pending{.left = ids.size(), .done = done});
          for (const auto& id : ids) {
            try {
              lookup(pending, id);
            } catch (const sdbus::Error&) {
              if (--pending->left == 0) {
                pending->done(std::move(pending->grants));
              }
            }
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.info("permission store {} not read: {}", kind.table, e.what());
    done({});
  }
}

void AppPermissions::setAllowed(const Kind& kind, const Grant& grant, bool allowed, std::function<void(bool ok)> done) {
  if (m_store == nullptr || grant.ids.empty() || (kind.id.empty() && allowed)) {
    done(false);
    return;
  }
  auto pending = std::make_shared<std::pair<std::size_t, bool>>(grant.ids.size(), true);
  auto reply = [pending, done, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> e) {
    if (alive.expired()) {
      return;
    }
    if (e.has_value()) {
      kLog.info("permission not revoked: {}", e->what());
      pending->second = false;
    }
    if (--pending->first == 0) {
      done(pending->second);
    }
  };
  const std::string answer = kind.table == "location" ? (allowed ? "EXACT" : "NONE") : (allowed ? "yes" : "no");
  try {
    for (std::size_t i = 0; i < grant.ids.size(); ++i) {
      const std::string& id = grant.ids[i];
      // The location portal reads exactly two values: accuracy and the last-use timestamp, which stays.
      std::vector<std::string> stored{answer};
      if (kind.table == "location") {
        const auto& old = i < grant.values.size() ? grant.values[i] : std::vector<std::string>{};
        stored.push_back(old.size() > 1 ? old[1] : std::string{"0"});
      }
      if (kind.id.empty()) {
        m_store->callMethodAsync("Delete").onInterface(kStoreInterface).withArguments(kind.table, id).uponReplyInvoke(
            reply
        );
      } else {
        m_store->callMethodAsync("SetPermission")
            .onInterface(kStoreInterface)
            .withArguments(kind.table, false, id, grant.app, stored)
            .uponReplyInvoke(reply);
      }
    }
  } catch (const sdbus::Error& e) {
    kLog.info("permission not revoked: {}", e.what());
    done(false);
  }
}

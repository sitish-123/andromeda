#include "dbus/accounts/accounts_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "dbus/system_interactive_call.h"

#include <gio/gio.h>
#include <pwd.h>
#include <crypt.h>
#include <cstdint>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

  using dbus::callInteractive;

  constexpr Logger kLog("accounts");

  const sdbus::ServiceName kAccountsBusName{"org.freedesktop.Accounts"};
  const sdbus::ObjectPath kAccountsObjectPath{"/org/freedesktop/Accounts"};
  constexpr auto kAccountsInterface = "org.freedesktop.Accounts";
  constexpr auto kUserInterface = "org.freedesktop.Accounts.User";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  template <typename T>
  T getPropertyOr(sdbus::IProxy& proxy, std::string_view iface, std::string_view propertyName, T fallback) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(iface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

} // namespace

AccountsService::AccountsService(SystemBus& bus) : m_bus(bus) {
  m_sessionUid = ::getuid();
  if (m_sessionUid == static_cast<uid_t>(-1)) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "failed to resolve session uid via getuid()"
    );
  }

  m_accountsProxy = sdbus::createProxy(m_bus.connection(), kAccountsBusName, kAccountsObjectPath);
  const std::weak_ptr<int> alive = m_aliveToken;
  m_accountsProxy->callMethodAsync("FindUserById")
      .onInterface(kAccountsInterface)
      .withArguments(static_cast<std::int64_t>(m_sessionUid))
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, sdbus::ObjectPath userPath) {
        if (alive.expired()) {
          return;
        }
        if (error.has_value()) {
          kLog.info("accounts service disabled: {}", error->getMessage());
          return;
        }
        bindUser(userPath);
      });
}

void AccountsService::bindUser(const std::string& userPath) {
  m_pendingUserProxy = sdbus::createProxy(m_bus.connection(), kAccountsBusName, sdbus::ObjectPath{userPath});
  const std::weak_ptr<int> alive = m_aliveToken;
  m_pendingUserProxy->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string(kUserInterface))
      .uponReplyInvoke([this,
                        alive](std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> properties) {
        if (alive.expired()) {
          return;
        }
        std::optional<std::uint64_t> uid;
        std::string iconFile;
        try {
          if (!error.has_value()) {
            if (const auto it = properties.find("Uid"); it != properties.end()) {
              uid = it->second.get<std::uint64_t>();
            }
            if (const auto it = properties.find("IconFile"); it != properties.end()) {
              iconFile = it->second.get<std::string>();
            }
          }
        } catch (const sdbus::Error& e) {
          error = e;
        }
        if (error.has_value() || uid != m_sessionUid) {
          kLog.warn(
              "accounts service disabled: {}",
              error.has_value() ? error->getMessage() : "user Uid does not match session uid"
          );
          return;
        }
        m_userProxy = std::move(m_pendingUserProxy);
        m_userProxy->uponSignal("PropertiesChanged")
            .onInterface(kPropertiesInterface)
            .call([this](
                      const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                      const std::vector<std::string>& /*invalidatedProperties*/
                  ) {
              if (interfaceName != kUserInterface) {
                return;
              }
              const auto it = changedProperties.find("IconFile");
              if (it == changedProperties.end()) {
                return;
              }
              try {
                emitChangedIfNeeded(it->second.get<std::string>());
              } catch (const sdbus::Error& e) {
                kLog.warn("failed to decode IconFile property change: {}", e.what());
              }
            });
        kLog.info("session user avatar path: {}", iconFile.empty() ? "<unset>" : iconFile);
        emitChangedIfNeeded(iconFile);
      });
}

void AccountsService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

bool AccountsService::setIconFile(std::string_view filename) {
  if (filename.empty()) {
    kLog.warn("SetIconFile rejected: filename is empty");
    return false;
  }
  if (m_userProxy == nullptr) {
    kLog.warn("SetIconFile rejected: user proxy is unavailable");
    return false;
  }

  try {
    m_userProxy->callMethod("SetIconFile").onInterface(kUserInterface).withArguments(std::string(filename));
    const std::string previousIconFile = m_iconFile;
    readIconFile();
    if (m_iconFile == previousIconFile && m_changeCallback) {
      m_changeCallback();
    }
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("SetIconFile failed: {}", e.what());
    return false;
  }
}

void AccountsService::readIconFile() {
  if (m_userProxy == nullptr) {
    return;
  }
  const auto iconFile = getPropertyOr<std::string>(*m_userProxy, kUserInterface, "IconFile", std::string{});
  emitChangedIfNeeded(iconFile);
}

void AccountsService::emitChangedIfNeeded(std::string_view nextIconFile) {
  const std::string next(nextIconFile);
  if (next == m_iconFile) {
    return;
  }
  m_iconFile = next;
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void AccountsService::createUser(
    const std::string& login, const std::string& realName, bool admin, const std::string& password,
    std::function<void(std::string)> done
) {
  const char* salt = crypt_gensalt_ra(nullptr, 0, nullptr, 0);
  crypt_data data{};
  const char* hashed = salt != nullptr ? crypt_r(password.c_str(), salt, &data) : nullptr;
  std::free(const_cast<char*>(salt));
  const std::string hash = hashed != nullptr && hashed[0] != '*' ? std::string(hashed) : std::string{};
  explicit_bzero(&data, sizeof(data));
  if (hash.empty()) {
    done("could not hash the password");
    return;
  }
  callInteractive(
      "org.freedesktop.Accounts", kAccountsObjectPath, kAccountsInterface, "CreateUser",
      g_variant_new("(ssi)", login.c_str(), realName.c_str(), admin ? 1 : 0),
      [hash, login, done = std::move(done)](GVariant* created, std::string error) {
        if (created == nullptr) {
          kLog.warn("CreateUser failed: {}", error);
          done(std::move(error));
          return;
        }
        const char* userPath = nullptr;
        g_variant_get(created, "(&o)", &userPath);
        callInteractive(
            "org.freedesktop.Accounts", userPath, kUserInterface, "SetPassword",
            g_variant_new("(ss)", hash.c_str(), ""), [login, done](GVariant* set, std::string passwordError) {
              if (set != nullptr) {
                done({});
                return;
              }
              kLog.warn("SetPassword failed: {}", passwordError);
              // An account without a password cannot log in and blocks a retry with the same name: remove it.
              const passwd* pw = getpwnam(login.c_str());
              if (pw == nullptr) {
                done(std::move(passwordError));
                return;
              }
              callInteractive(
                  "org.freedesktop.Accounts", kAccountsObjectPath, kAccountsInterface, "DeleteUser",
                  g_variant_new("(xb)", static_cast<gint64>(pw->pw_uid), TRUE),
                  [done, passwordError](GVariant* deleted, std::string deleteError) {
                    if (deleted == nullptr) {
                      kLog.warn("DeleteUser after a failed SetPassword failed: {}", deleteError);
                    }
                    done(passwordError);
                  }
              );
            }
        );
      }
  );
}

void AccountsService::deleteUser(uid_t uid, std::function<void(std::string)> done) {
  callInteractive(
      "org.freedesktop.Accounts", kAccountsObjectPath, kAccountsInterface, "DeleteUser",
      g_variant_new("(xb)", static_cast<gint64>(uid), TRUE),
      [done = std::move(done)](GVariant* deleted, std::string error) {
        if (deleted == nullptr) {
          kLog.warn("DeleteUser failed: {}", error);
        }
        done(deleted == nullptr ? std::move(error) : std::string{});
      }
  );
}

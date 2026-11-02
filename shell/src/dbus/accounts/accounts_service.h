#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class AccountsService {
public:
  using ChangeCallback = std::function<void()>;

  explicit AccountsService(SystemBus& bus);

  void setChangeCallback(ChangeCallback callback);

  [[nodiscard]] uid_t sessionUid() const noexcept { return m_sessionUid; }
  [[nodiscard]] const std::string& iconFile() const noexcept { return m_iconFile; }

  [[nodiscard]] bool setIconFile(std::string_view filename);

  // Creates a login user, then sets its password. Async: the polkit prompt comes from this process.
  // `done` gets an empty string on success, else the error text.
  void createUser(
      const std::string& login, const std::string& realName, bool admin, const std::string& password,
      std::function<void(std::string)> done
  );

private:
  void readIconFile();
  void emitChangedIfNeeded(std::string_view nextIconFile);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_accountsProxy;
  std::unique_ptr<sdbus::IProxy> m_userProxy;
  uid_t m_sessionUid = 0;
  std::string m_iconFile;
  ChangeCallback m_changeCallback;
};

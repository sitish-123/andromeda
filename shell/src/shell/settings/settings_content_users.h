#pragma once

#include <functional>
#include <string>

class Flex;

namespace settings {

  struct NewUserDraft {
    std::string fullName;
    std::string login;
    std::string password;
    std::string verify;
    bool admin = false;
    bool loginEdited = false;
    bool busy = false;
    std::string error;
  };

  struct SettingsUsersContext {
    float scale = 1.0F;
    // "users:<title>" for the New User page, "user:<name>\t<uid>" for one user's page, else empty.
    std::string detail;
    // Null when AccountsService is missing: no Add User button.
    NewUserDraft* draft = nullptr;
    // The user page asks in an alert before it deletes; the text is the error of a failed delete.
    bool deleteBusy = false;
    std::string deleteError;
    std::function<void(unsigned uid, std::string login)> confirmDelete;
    std::function<void(unsigned uid)> deleteUser;
    std::function<void(std::string)> openDetail;
    // Validates the draft, then creates the user.
    std::function<void()> create;
    std::function<void()> back;
    std::function<void()> rebuild;
  };

  // Users page (direction A, .agent/DESIGN.md): the current user as the hero card, then the other login users;
  // or the New User page.
  void addSettingsUsers(Flex& content, const SettingsUsersContext& ctx);

  // Lowercase ASCII login from a full name ("Ada Lovelace" -> "ada"); empty when none can be made.
  [[nodiscard]] std::string loginFromName(const std::string& fullName);
  [[nodiscard]] bool validLogin(const std::string& login);

} // namespace settings

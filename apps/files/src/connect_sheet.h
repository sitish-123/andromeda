#pragma once

#include "server_url.h"

#include <functional>
#include <memory>
#include <string>

class Button;
class Label;
class Node;

// Connect to Server sheet (mockup files-v2-live.html §10, FILES-UI-SPEC §9; rulebook 9.10 dialog): an address field
// with examples, user and password, and "Remember in Keychain". The host mounts; the sheet shows its error or busy
// state until the host closes it.
class ConnectSheet {
public:
  // remember: the password goes to the Secret Service keyring (gvfs stores it); reconnect: mount again at login.
  using Connect = std::function<void(const ServerUrl& url, const std::string& user, const std::string& password,
                                     bool remember, bool reconnect)>;

  ConnectSheet(std::string address, Connect connect, std::function<void()> close, std::function<void()> rebuild);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void submit();
  // The host's mount failed: the message shows under the fields and Connect works again.
  void setError(std::string message);
  // The server asks for a password: the password field shows and takes focus on the next build.
  void askPassword(std::string user, std::string message);
  // The server asks a question (an unknown SSH host key): its text shows as a caption and the primary button reads
  // "Accept"; pressing it is the user's answer. A change to the address drops the question.
  void setQuestion(std::string text);

private:
  void update();

  std::string m_address;
  std::string m_user;
  std::string m_password;
  bool m_remember = false;
  bool m_reconnect = false;
  bool m_showPassword = false;
  bool m_busy = false;
  std::string m_error;
  std::string m_question;
  Connect m_connect;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;

  Label* m_note = nullptr;
  Label* m_questionNote = nullptr;
  Button* m_ok = nullptr;
};

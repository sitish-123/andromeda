#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Button;
class Input;
class Label;
class Node;
class InputDispatcher;

// New Vault and Unlock sheets (mockup files-v2-live.html §17; rulebook 9.10 dialog). Create asks a name and the
// password twice; Unlock asks the password. The host runs gocryptfs and answers with done() or setError().
class VaultSheet {
public:
  enum class Mode : std::uint8_t { Create, Unlock };
  using Submit = std::function<void(const std::string& name, const std::string& password)>;

  VaultSheet(Mode mode, std::string name, Submit submit, std::function<void()> close, std::function<void()> rebuild,
             InputDispatcher* input);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void submit();
  // The password field's text, then Unlock (the sheet.password action).
  void submitPassword(const std::string& password);
  void setError(std::string message);

private:
  void update();

  Mode m_mode;
  std::string m_name;
  std::string m_password;
  std::string m_confirm;
  std::string m_error;
  bool m_busy = false;
  bool m_focused = false;
  Submit m_submit;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  InputDispatcher* m_input;

  Label* m_note = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_ok = nullptr;
};

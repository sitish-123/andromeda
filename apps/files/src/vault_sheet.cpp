#include "vault_sheet.h"

#include "core/deferred_call.h"

#include <format>
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace {
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;
  constexpr std::size_t kMinPassword = 8;
} // namespace

VaultSheet::VaultSheet(Mode mode, std::string name, Submit submit, std::function<void()> close,
                       std::function<void()> rebuild, InputDispatcher* input)
    : m_mode(mode), m_name(std::move(name)), m_submit(std::move(submit)), m_close(std::move(close)),
      m_rebuild(std::move(rebuild)), m_input(input) {}

std::unique_ptr<Node> VaultSheet::build(float scale) {
  const float s = scale;
  const bool create = m_mode == Mode::Create;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  auto header = ui::column({.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = Tokens::spaceP4 * s,
                            .paddingH = Tokens::componentDialogPadding * s,
                            .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = create ? std::string("New Vault") : std::format("Unlock “{}”", m_name),
                              .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                              .ellipsize = TextEllipsize::Middle}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  body->addChild(ui::label({.text = create ? std::string("Files you put in a vault are encrypted. Without the "
                                                         "password nobody can open them, you included.")
                                           : std::string("Enter the password."),
                            .fontSize = Style::fontSizeCaption * s, .color = colorSpecFromRole(ColorRole::Label2),
                            .maxLines = 3}));
  const auto fieldRow = [&](const char* label) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = kFieldLabel * s}));
    return row;
  };
  // The first field takes focus once, on the first build.
  const auto focusFirst = [this](Input& input) {
    if (!m_focused && m_input != nullptr) {
      m_focused = true;
      m_input->setFocus(input.inputArea());
    }
  };
  if (create) {
    auto name = fieldRow("Name");
    name->addChild(ui::input({.value = m_name, .placeholder = std::string("Private"),
                              .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                              .flexGrow = 1.0F,
                              .onChange = [this](const std::string& v) {
                                m_name = v;
                                m_error.clear();
                                update();
                              },
                              .onSubmit = [this](const std::string&) { submit(); },
                              .configure = m_error.empty() ? focusFirst : std::function<void(Input&)>()}));
    body->addChild(std::move(name));
  }
  auto password = fieldRow("Password");
  password->addChild(ui::input({.out = &m_passwordInput, .value = m_password, .placeholder = std::string(create ? "8 characters or more" : ""),
                                .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                                .passwordMode = true, .flexGrow = 1.0F,
                                .onChange = [this](const std::string& v) {
                                  m_password = v;
                                  m_error.clear();
                                  update();
                                },
                                .onSubmit = [this](const std::string&) { submit(); },
                                .configure = create && m_error.empty() ? std::function<void(Input&)>() : focusFirst}));
  body->addChild(std::move(password));
  if (create) {
    auto confirm = fieldRow("Verify");
    confirm->addChild(ui::input({.value = m_confirm, .fontSize = Style::fontSizeBody * s,
                                 .controlHeight = Style::controlHeight * s, .passwordMode = true, .flexGrow = 1.0F,
                                 .onChange = [this](const std::string& v) {
                                   m_confirm = v;
                                   m_error.clear();
                                   update();
                                 },
                                 .onSubmit = [this](const std::string&) { submit(); }}));
    body->addChild(std::move(confirm));
  }
  body->addChild(ui::label({.out = &m_note, .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 3}));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  footer->addChild(ui::button({.out = &m_ok, .text = std::string(create ? "Create" : "Unlock"),
                               .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLater([this] { submit(); }); }}));
  sheet->addChild(std::move(footer));
  update();
  return sheet;
}

void VaultSheet::update() {
  if (m_ok != nullptr) {
    const bool ready = m_mode == Mode::Create ? !m_name.empty() && !m_password.empty() && !m_confirm.empty()
                                              : !m_password.empty();
    m_ok->setEnabled(ready && !m_busy);
    if (m_busy) {
      m_ok->setText(m_mode == Mode::Create ? "Creating…" : "Unlocking…");
    }
  }
  if (m_note != nullptr) {
    m_note->setText(m_error);
    m_note->setVisible(!m_error.empty());
  }
}

void VaultSheet::submit() {
  if (m_busy) {
    return;
  }
  if (m_mode == Mode::Create) {
    if (m_password.size() < kMinPassword) {
      setError("Use a password of 8 characters or more.");
      return;
    }
    if (m_password != m_confirm) {
      setError("The passwords are not the same.");
      return;
    }
  } else if (m_password.empty()) {
    return;
  }
  m_busy = true;
  m_error.clear();
  update();
  m_submit(m_name, m_password);
}

void VaultSheet::submitPassword(const std::string& password) {
  m_password = password;
  if (m_mode == Mode::Create) {
    m_confirm = password;
  }
  submit();
}

void VaultSheet::setError(std::string message) {
  m_busy = false;
  m_error = std::move(message);
  // In place, no rebuild: the focused password field empties and keeps the focus for the next try.
  m_password.clear();
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
  }
  update();
}

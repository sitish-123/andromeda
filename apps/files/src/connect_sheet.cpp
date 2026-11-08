#include "connect_sheet.h"

#include "core/deferred_call.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace {
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;
  constexpr const char* kExamples = "smb://nas.local/share · sftp://user@host:22/path · davs://cloud.example.com";
} // namespace

ConnectSheet::ConnectSheet(std::string address, Connect connect, std::function<void()> close,
                           std::function<void()> rebuild)
    : m_address(std::move(address)), m_connect(std::move(connect)), m_close(std::move(close)),
      m_rebuild(std::move(rebuild)) {
  if (const auto url = parseServerUrl(m_address); url.has_value()) {
    m_user = url->user;
  }
}

std::unique_ptr<Node> ConnectSheet::build(float scale) {
  const float s = scale;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = std::string("Connect to Server"), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  const auto fieldRow = [&](const char* label) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label2), .width = kFieldLabel * s}));
    return row;
  };

  auto address = fieldRow("Server");
  address->addChild(ui::input({.value = m_address, .placeholder = std::string("smb://server/share"),
                               .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                               .flexGrow = 1.0F,
                               .onChange = [this](const std::string& v) {
                                 m_address = v;
                                 m_error.clear();
                                 m_question.clear(); // another address: its question is not answered here
                                 update();
                               },
                               .onSubmit = [this](const std::string&) { submit(); }}));
  body->addChild(std::move(address));
  body->addChild(ui::label({.text = std::string(kExamples), .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::Label3), .maxLines = 2}));

  auto user = fieldRow("User");
  user->addChild(ui::input({.value = m_user, .placeholder = std::string("Guest when empty"),
                            .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                            .flexGrow = 1.0F,
                            .onChange = [this](const std::string& v) { m_user = v; },
                            .onSubmit = [this](const std::string&) { submit(); }}));
  body->addChild(std::move(user));

  auto password = fieldRow("Password");
  password->addChild(ui::input({.value = m_password, .placeholder = std::string(m_showPassword ? "Required" : "Optional"),
                                .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                                .passwordMode = true, .flexGrow = 1.0F,
                                .onChange = [this](const std::string& v) { m_password = v; },
                                .onSubmit = [this](const std::string&) { submit(); }}));
  body->addChild(std::move(password));

  auto remember = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  remember->addChild(ui::row({.width = kFieldLabel * s})); // aligns with the fields
  remember->addChild(ui::checkbox({.checked = m_remember, .scale = s,
                                   .onChange = [this](bool on) { m_remember = on; }}));
  remember->addChild(ui::label({.text = std::string("Remember password in the keyring"),
                                .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1)}));
  body->addChild(std::move(remember));

  auto reconnect = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  reconnect->addChild(ui::row({.width = kFieldLabel * s}));
  reconnect->addChild(ui::checkbox({.checked = m_reconnect, .scale = s,
                                    .onChange = [this](bool on) { m_reconnect = on; }}));
  reconnect->addChild(ui::label({.text = std::string("Connect again at login"),
                                 .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1)}));
  body->addChild(std::move(reconnect));

  body->addChild(ui::label({.out = &m_questionNote, .text = m_question, .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 8}));
  body->addChild(ui::label({.out = &m_note, .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 3}));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  footer->addChild(ui::button({.out = &m_ok, .text = std::string(m_busy ? "Connecting…" : (m_question.empty() ? "Connect" : "Accept")),
                               .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLater([this] { submit(); }); }}));
  sheet->addChild(std::move(footer));
  update();
  return sheet;
}

void ConnectSheet::update() {
  const auto url = parseServerUrl(m_address);
  if (m_ok != nullptr) {
    m_ok->setEnabled(url.has_value() && !m_busy);
    m_ok->setText(m_busy ? "Connecting…" : (m_question.empty() ? "Connect" : "Accept"));
  }
  if (m_note != nullptr) {
    // A typing error shows only after Connect; the host's mount error shows until the address changes.
    m_note->setText(m_error);
    m_note->setVisible(!m_error.empty());
  }
  if (m_questionNote != nullptr) {
    m_questionNote->setVisible(!m_question.empty());
  }
}

void ConnectSheet::submit() {
  const auto url = parseServerUrl(m_address);
  if (!url.has_value()) {
    m_error = url.error();
    update();
    return;
  }
  if (m_busy) {
    return;
  }
  m_busy = true;
  m_error.clear();
  m_question.clear();
  m_connect(*url, m_user.empty() ? url->user : m_user, m_password, m_remember, m_reconnect);
  if (m_rebuild) {
    m_rebuild();
  }
}

void ConnectSheet::setError(std::string message) {
  m_busy = false;
  m_error = std::move(message);
  if (m_rebuild) {
    m_rebuild();
  }
}

void ConnectSheet::setQuestion(std::string text) {
  m_busy = false;
  m_error.clear();
  m_question = std::move(text);
  if (m_rebuild) {
    m_rebuild();
  }
}

void ConnectSheet::askPassword(std::string user, std::string message) {
  m_busy = false;
  m_showPassword = true;
  if (!user.empty() && m_user.empty()) {
    m_user = std::move(user);
  }
  m_error = std::move(message);
  if (m_rebuild) {
    m_rebuild();
  }
}

#include "shell/control_center/tabs/network_tab.h"

#include "core/ui_phase.h"
#include "dbus/modem/modem_manager_service.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/network/nm_hotspot.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  TOKENS_LIVE(kRowMinHeight, Style::controlHeightLg);

  // Label and wire value in one row, so the visible dropdown order and the value
  // sent to NetworkManager cannot drift apart.
  struct EapOption {
    std::string_view label;
    network_enterprise::EapMethod method;
  };

  struct Phase2Option {
    std::string_view label;
    network_enterprise::Phase2Auth auth;
  };

  constexpr std::array kEapOptions{
      EapOption{"PEAP", network_enterprise::EapMethod::Peap},
      EapOption{"TTLS", network_enterprise::EapMethod::Ttls},
  };

  // MSCHAPv2 first: it is the only inner method PEAP accepts, and PEAP is the
  // default EAP method.
  constexpr std::array kPhase2Options{
      Phase2Option{"MSCHAPv2", network_enterprise::Phase2Auth::MsChapV2},
      Phase2Option{"PAP", network_enterprise::Phase2Auth::Pap},
      Phase2Option{"MSCHAP", network_enterprise::Phase2Auth::MsChap},
      Phase2Option{"CHAP", network_enterprise::Phase2Auth::Chap},
  };

  constexpr std::size_t phase2IndexOf(network_enterprise::Phase2Auth auth) {
    for (std::size_t index = 0U; index < kPhase2Options.size(); ++index) {
      if (kPhase2Options[index].auth == auth) {
        return index;
      }
    }
    return kPhase2Options.size();
  }

  // Both dropdowns open on their first option, and PEAP accepts MSCHAPv2 only, so
  // choosing PEAP pins the inner dropdown to this row.
  constexpr std::size_t kDefaultOptionIndex = 0U;
  constexpr std::size_t kPeapPhase2Index = phase2IndexOf(network_enterprise::Phase2Auth::MsChapV2);
  static_assert(kPeapPhase2Index < kPhase2Options.size());
  static_assert(kEapOptions[kDefaultOptionIndex].method == network_enterprise::EapMethod::Peap);
  static_assert(kPeapPhase2Index == kDefaultOptionIndex);

  template <typename Options> std::vector<std::string> optionLabels(const Options& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) {
      labels.emplace_back(option.label);
    }
    return labels;
  }

  // A Select with no selection reports npos; the first option is the documented
  // default for both dropdowns.
  template <typename Options> const auto& optionAt(const Options& options, std::size_t index) {
    return index < options.size() ? options[index] : options.front();
  }

  std::string validationMessage(network_enterprise::Validation problem) {
    switch (problem) {
    case network_enterprise::Validation::MissingIdentity:
      return i18n::tr("control-center.network.error-missing-identity");
    case network_enterprise::Validation::MissingPassword:
      return i18n::tr("control-center.network.error-missing-password");
    case network_enterprise::Validation::UnsupportedPhase2:
      return i18n::tr("control-center.network.error-unsupported-phase2");
    case network_enterprise::Validation::CaCertNotAbsolute:
      return i18n::tr("control-center.network.error-ca-not-absolute");
    case network_enterprise::Validation::Ok:
      break;
    }
    return {};
  }

  std::string currentDetail(const NetworkState& s, const std::string& externalIp, const CellularModemInfo* modem) {
    if (!s.connected) {
      if (s.kind == NetworkConnectivity::Cellular && modem != nullptr) {
        return cellularStateText(modem->state);
      }
      return s.wirelessEnabled ? i18n::tr("control-center.network.wifi-on")
                               : i18n::tr("control-center.network.wifi-off");
    }
    std::string out;
    const auto append = [&out](std::string_view part) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += part;
    };
    if (!s.ipv4.empty()) {
      append(s.ipv4);
    }
    if (s.kind == NetworkConnectivity::Wireless) {
      if (s.signalStrength > 0) {
        append(std::to_string(static_cast<int>(s.signalStrength)) + "%");
      }
      if (const char* band = network_display::wifiFrequencyBandLabel(s.frequencyMhz); band != nullptr) {
        append(band);
      }
    }
    if (s.kind == NetworkConnectivity::Cellular && modem != nullptr) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += std::to_string(static_cast<int>(modem->signalQuality)) + "%";
      if (const char* tech = cellularAccessTechnologyName(modem->accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
    }
    if (!externalIp.empty()) {
      append(i18n::tr("control-center.network.external-ip", "ip", externalIp));
    }
    return out;
  }

  const char* cellularGlyphFor(const CellularModemInfo& modem) {
    return modem.enabled() ? network_display::cellularGlyphForSignal(modem.signalQuality)
                           : network_display::cellularOffGlyph();
  }

  std::string cellularTitleFor(const CellularModemInfo& modem) {
    if (!modem.operatorName.empty()) {
      return modem.operatorName;
    }
    if (!modem.name.empty()) {
      return modem.name;
    }
    return i18n::tr("control-center.network.cellular");
  }

  std::string cellularDetailFor(const CellularModemInfo& modem) {
    std::string out = cellularStateText(modem.state);
    if (modem.enabled()) {
      if (const char* tech = cellularAccessTechnologyName(modem.accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
      out += "  •  " + std::to_string(static_cast<int>(modem.signalQuality)) + "%";
    }
    return out;
  }


  // Active first, then by signal band, then by name. Ordering on the raw strength —
  // as the services do — reshuffles rows on every scan update, moving the row you are
  // aiming at out from under the pointer.
  std::vector<AccessPointInfo> sortedAccessPoints(std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_display::wifiSignalBand(a.strength);
      const int bandB = network_display::wifiSignalBand(b.strength);
      if (bandA != bandB) {
        return bandA > bandB;
      }
      return a.ssid < b.ssid;
    });
    return aps;
  }

} // namespace

namespace {

  TOKENS_LIVE(kRoundIcon, Tokens::componentRoundIconSize);

  // A Button's own hit area has z-index 1 and covers its children; nested buttons must sit above it.
  constexpr std::int32_t kAboveRowArea = 2;

  struct RowModel {
    std::string ssid;
    std::string glyph;
    bool on = false;
    std::string title;
    std::string caption;
    std::string lock;   // glyph name, empty for none
    std::string signal; // glyph name, empty for none; live, kept out of the structure key
    std::string extra;  // structural state that is not drawn in the row
    bool current = false;
    bool forget = false;
    std::function<void()> onClick;
    std::function<void()> onForget;
  };

  struct RowUi {
    Button* icon = nullptr;
    Label* title = nullptr;
    Label* caption = nullptr;
    Glyph* signal = nullptr;
  };

  std::unique_ptr<Label> makeLabel(Label*& out, const Tokens::Type& type, float scale, ColorRole role) {
    auto label = std::make_unique<Label>();
    label->setFontSize(type.size * scale);
    label->setLineHeight(type.line * scale);
    label->setFontWeight(static_cast<FontWeight>(Style::glassWeight(type.weight)));
    label->setColor(colorSpecFromRole(role));
    label->setMaxLines(1);
    out = label.get();
    return label;
  }

  std::unique_ptr<Button> makeGlyphButton(
      Button*& out, std::string_view glyph, float scale, std::function<void()> click, float size, ButtonVariant variant
  ) {
    auto button = std::make_unique<Button>();
    button->setVariant(variant);
    button->setGlyph(glyph);
    button->setGlyphSize(Tokens::componentRoundIconGlyph * scale);
    button->setControlHeight(size * scale);
    button->setMinWidth(size * scale);
    button->setPadding(0.0F);
    button->setRadius(size * 0.5F * scale);
    button->setOnClick(std::move(click));
    out = button.get();
    return button;
  }

  std::unique_ptr<Flex> makeCard(float scale, float opacity) {
    auto card = std::make_unique<Flex>();
    applySectionCardStyle(*card, scale, opacity);
    card->setFill(colorSpecFromRole(ColorRole::Fill1, opacity));
    card->clearBorder();
    card->setRadius(Tokens::componentTileRadius * scale);
    card->setPadding(Tokens::componentTilePadding * scale);
    card->setGap(Tokens::spaceP12 * scale);
    return card;
  }

  std::unique_ptr<Flex> makeColumn(float gap) {
    auto column = std::make_unique<Flex>();
    column->setDirection(FlexDirection::Vertical);
    column->setAlign(FlexAlign::Stretch);
    column->setGap(gap);
    return column;
  }

  // Round icon, title and caption, then lock and signal glyphs, as a quick-sheet row.
  std::unique_ptr<Button> makeRow(const RowModel& model, float scale, RowUi& ui) {
    auto row = std::make_unique<Button>();
    row->setVariant(ButtonVariant::Ghost);
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setPadding(0.0F);
    row->setMinHeight(kRoundIcon * scale);
    row->setGap(Tokens::spaceP12 * scale);
    row->setContentAlign(ButtonContentAlign::Start);
    row->setOnClick(model.onClick);

    auto icon = makeGlyphButton(
        ui.icon, model.glyph, scale, model.onClick, kRoundIcon, model.on ? ButtonVariant::Primary : ButtonVariant::Tonal
    );
    icon->setZIndex(kAboveRowArea);
    row->addChild(std::move(icon));

    auto text = std::make_unique<Flex>();
    text->setDirection(FlexDirection::Vertical);
    text->setAlign(FlexAlign::Start);
    text->setFlexGrow(1.0F);
    text->addChild(makeLabel(ui.title, Tokens::typeHeadline, scale, ColorRole::Label1));
    text->addChild(makeLabel(ui.caption, Tokens::typeFootnote, scale, ColorRole::Label2));
    ui.title->setText(model.title);
    ui.caption->setText(model.caption);
    ui.caption->setVisible(!model.caption.empty());
    row->addChild(std::move(text));

    const auto addGlyph = [&](const std::string& name, ColorRole role) {
      return ui::glyph({
          .glyph = name,
          .glyphSize = Tokens::componentRoundIconGlyph * scale,
          .color = colorSpecFromRole(role),
      });
    };
    if (!model.lock.empty()) {
      row->addChild(addGlyph(model.lock, ColorRole::Label2));
    }
    if (!model.signal.empty()) {
      auto signal = addGlyph(model.signal, ColorRole::Label1);
      ui.signal = signal.get();
      row->addChild(std::move(signal));
    }
    if (model.forget) {
      Button* unused = nullptr;
      auto forget = makeGlyphButton(unused, "trash", scale, model.onForget, kRoundIcon * 0.75F, ButtonVariant::Ghost);
      forget->setTooltip(i18n::tr("control-center.network.forget-network"));
      forget->setZIndex(kAboveRowArea);
      row->addChild(std::move(forget));
    }
    return row;
  }

  void appendKey(std::string& key, const RowModel& m) {
    for (const std::string* part : {&m.ssid, &m.glyph, &m.title, &m.caption, &m.lock, &m.extra}) {
      key += *part;
      key.push_back('\x1f');
    }
    key += m.on ? '1' : '0';
    key += m.forget ? '1' : '0';
    key.push_back('\n');
  }

} // namespace
NetworkTab::NetworkTab(
    INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp, ModemManagerService* modem,
    NmHotspot* hotspot
)
    : m_network(network), m_secrets(secrets), m_externalIpService(externalIp), m_modem(modem), m_hotspot(hotspot) {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
      showPasswordPrompt(request);
      PanelManager::instance().refresh();
    });
  }
}

NetworkTab::~NetworkTab() {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback(nullptr);
    m_secrets->cancelSecret();
  }
}

std::unique_ptr<Flex> NetworkTab::makePasswordCard() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  auto card = makeColumn(Tokens::spaceP8 * scale);
  m_passwordCard = card.get();
  card->setVisible(false);

  card->addChild(
      ui::label({
          .out = &m_passwordTitle,
          .fontSize = Tokens::typeHeadline.size * scale,
          .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
          .color = colorSpecFromRole(ColorRole::Label1),
      })
  );

  card->addChild(
      ui::label({
          .out = &m_credentialError,
          .fontSize = Tokens::typeFootnote.size * scale,
          .color = colorSpecFromRole(ColorRole::DangerText),
          .maxLines = 4,
          .visible = false,
      })
  );

  const auto submitFromForm = [this](const std::string& /*value*/) {
    submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{});
  };

  auto enterpriseFields = ui::column({
      .out = &m_enterpriseFields,
      // Flex defaults to Center on the cross axis, which collapses each field to its natural width.
      .align = FlexAlign::Stretch,
      .visible = false,
      .configure = [scale](Flex& column) { column.setGap(Tokens::spaceP8 * scale); },
  });

  enterpriseFields->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale},
          ui::select({
              .out = &m_eapSelect,
              .options = optionLabels(kEapOptions),
              .selectedIndex = std::size_t{0},
              .placeholder = i18n::tr("control-center.network.eap-method"),
              .surfaceOpacity = opacity,
              .flexGrow = 1.0F,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view /*text*/) {
                    // PEAP tunnels MSCHAPv2 only: pin and lock the inner method so the user cannot
                    // assemble a pair that fails at association time.
                    if (m_phase2Select == nullptr) {
                      return;
                    }
                    const bool pinsInnerMethod =
                        optionAt(kEapOptions, index).method == network_enterprise::EapMethod::Peap;
                    if (pinsInnerMethod) {
                      m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
                    }
                    m_phase2Select->setEnabled(!pinsInnerMethod);
                  },
          }),
          ui::select({
              .out = &m_phase2Select,
              .options = optionLabels(kPhase2Options),
              .selectedIndex = kPeapPhase2Index,
              .placeholder = i18n::tr("control-center.network.phase2-auth"),
              .enabled = false,
              .surfaceOpacity = opacity,
              .flexGrow = 1.0F,
          })
      )
  );

  const auto addField = [&](Input*& out, const char* placeholderKey) {
    enterpriseFields->addChild(
        ui::input({
            .out = &out,
            .placeholder = i18n::tr(placeholderKey),
            .surfaceOpacity = opacity,
            .surfaceRole = ColorRole::SolidRegular,
            .onSubmit = submitFromForm,
        })
    );
  };
  addField(m_identityInput, "control-center.network.identity");
  addField(m_anonymousIdentityInput, "control-center.network.anonymous-identity");
  addField(m_domainMatchInput, "control-center.network.domain-suffix-match");
  addField(m_caCertInput, "control-center.network.ca-certificate");
  card->addChild(std::move(enterpriseFields));

  card->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale},
          ui::input({
              .out = &m_passwordInput,
              .placeholder = i18n::tr("control-center.network.password"),
              .passwordMode = true,
              .surfaceOpacity = opacity,
              .surfaceRole = ColorRole::SolidRegular,
              .flexGrow = 1.0F,
              .onSubmit = [this](const std::string& value) { submitPasswordPrompt(value); },
          }),
          ui::button({
              .out = &m_passwordRevealButton,
              .glyph = "eye",
              .glyphSize = Tokens::componentRoundIconGlyph * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.show-password"),
              .minWidth = Style::controlHeight * scale,
              .minHeight = Style::controlHeight * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick =
                  [this] {
                    if (m_passwordInput == nullptr) {
                      return;
                    }
                    const bool revealed = !m_passwordInput->passwordRevealed();
                    m_passwordInput->setPasswordRevealed(revealed);
                    if (m_passwordRevealButton != nullptr) {
                      m_passwordRevealButton->setGlyph(revealed ? "eye-off" : "eye");
                    }
                  },
          })
      )
  );

  auto spacer = std::make_unique<Flex>();
  spacer->setFlexGrow(1.0F);
  card->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale},
          std::move(spacer),
          ui::button({
              .text = i18n::tr("common.actions.cancel"),
              .variant = ButtonVariant::Ghost,
              .onClick = [this] { cancelPasswordPrompt(); },
          }),
          ui::button({
              .out = &m_joinButton,
              .text = i18n::tr("control-center.network.join"),
              .enabled = false,
              .variant = ButtonVariant::Primary,
              .onClick =
                  [this] {
                    submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{});
                  },
          })
      )
  );
  return card;
}

std::unique_ptr<Flex> NetworkTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Tokens::componentSheetGap * scale,
  });

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  Flex* scrollContent = listScroll->content();
  scrollContent->setDirection(FlexDirection::Vertical);
  scrollContent->setAlign(FlexAlign::Stretch);
  scrollContent->setGap(Tokens::componentSheetGap * scale);
  scrollContent->addChild(
      ui::column({
          .out = &m_list,
          .align = FlexAlign::Stretch,
          .gap = Tokens::componentSheetGap * scale,
      })
  );
  tab->addChild(std::move(listScroll));

  tab->addChild(
      ui::button({
          .text = i18n::tr("control-center.network.network-settings"),
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Tokens::componentTilePadding * scale,
          .onClick = [] { PanelManager::instance().openSettingsWindow(); },
          .configure = [scale](Button& button) {
            if (button.label() != nullptr) {
              button.label()->setColor(colorSpecFromRole(ColorRole::Label2));
              button.label()->setFontSize(Tokens::typeHeadline.size * scale);
            }
          },
      })
  );

  m_passwordHolder = makePasswordCard();
  return tab;
}

std::unique_ptr<Flex> NetworkTab::createHeaderActions() {
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * contentScale()},
      ui::toggle({
          .out = &m_wifiToggle,
          .checkedImmediate = m_network != nullptr && m_network->state().wirelessEnabled,
          .toggleSize = ToggleSize::Medium,
          .scale = contentScale(),
          .onChange = [this](bool checked) { requestWirelessEnabled(checked); },
      })
  );
}
void NetworkTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_active && m_network != nullptr) {
    m_network->requestScan();
  }
}

void NetworkTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCellularCard();
  syncCurrentCard();
  m_rootLayout->layout(renderer);
}

void NetworkTab::doUpdate(Renderer& renderer) {
  syncPasswordCard();
  rebuildApList(renderer);
  focusPendingPasswordPrompt();
  // A cellular caption changes width, so the list has to be laid out again.
  bool listChanged = syncApRows();
  listChanged = syncCellularCard() || listChanged;
  if (listChanged && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncCurrentCard();
  if (m_joinButton != nullptr) {
    const bool hasPassword = m_passwordInput != nullptr && !m_passwordInput->value().empty();
    m_joinButton->setEnabled(m_pendingEnterprise || hasPassword);
  }
}

void NetworkTab::onClose() {
  if (m_hasPendingSecret && !m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  m_hasPendingSecret = false;
  m_pendingEnterprise = false;
  m_focusPasswordPrompt = false;
  m_pendingSsid.clear();
  m_rootLayout = nullptr;
  m_passwordHolder.reset();
  m_passwordCard = nullptr;
  m_passwordTitle = nullptr;
  m_passwordInput = nullptr;
  m_passwordRevealButton = nullptr;
  m_joinButton = nullptr;
  m_enterpriseFields = nullptr;
  m_eapSelect = nullptr;
  m_phase2Select = nullptr;
  m_identityInput = nullptr;
  m_anonymousIdentityInput = nullptr;
  m_caCertInput = nullptr;
  m_domainMatchInput = nullptr;
  m_credentialError = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_rescanButton = nullptr;
  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_currentExpanded = false;
  m_cellularRows.clear();
  m_cellularTogglePending = false;
  m_cellularTogglePendingTimer.stop();
  m_apSignals.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
  m_pendingAccessPoint.reset();
  m_active = false;
  m_actionPending = false;
  m_actionPendingTimer.stop();
  m_wifiTogglePending = false;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  ++m_wifiToggleRequestGeneration;
}
void NetworkTab::syncPasswordCard() {
  if (m_hasPendingSecret
      && !m_pendingAccessPoint.has_value()
      && m_secrets != nullptr
      && !m_secrets->hasPendingRequest()) {
    clearPasswordPrompt();
  }
  if (m_passwordCard == nullptr) {
    return;
  }
  m_passwordCard->setVisible(m_hasPendingSecret);
  if (m_enterpriseFields != nullptr) {
    m_enterpriseFields->setVisible(m_hasPendingSecret && m_pendingEnterprise);
  }
  if (m_hasPendingSecret && m_passwordTitle != nullptr) {
    if (m_pendingSsid.empty()) {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt"));
    } else if (m_pendingEnterprise) {
      m_passwordTitle->setText(i18n::tr("control-center.network.enterprise-prompt-for", "ssid", m_pendingSsid));
    } else {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt-for", "ssid", m_pendingSsid));
    }
  }
}

void NetworkTab::focusPendingPasswordPrompt() {
  if (!std::exchange(m_focusPasswordPrompt, false) || !m_hasPendingSecret) {
    return;
  }
  // The enterprise form starts with the identity; a plain prompt only has the password.
  Input* target = m_pendingEnterprise ? m_identityInput : m_passwordInput;
  if (target != nullptr) {
    PanelManager::instance().focusArea(target->inputArea());
  }
}

void NetworkTab::setCredentialError(const std::string& message) {
  if (m_credentialError == nullptr) {
    return;
  }
  m_credentialError->setText(message);
  m_credentialError->setVisible(!message.empty());
  // Showing or hiding the message changes the card's height, and the card is
  // above the network list.
  PanelManager::instance().requestLayout();
}

network_enterprise::EnterpriseCredentials NetworkTab::collectEnterpriseCredentials(const std::string& password) const {
  const auto selectedIndexOf = [](const Select* select) -> std::size_t {
    return select != nullptr ? select->selectedIndex() : kDefaultOptionIndex;
  };
  const auto valueOf = [](const Input* input) -> std::string {
    return input != nullptr ? input->value() : std::string{};
  };

  network_enterprise::EnterpriseCredentials credentials;
  credentials.eap = optionAt(kEapOptions, selectedIndexOf(m_eapSelect)).method;
  credentials.phase2 = optionAt(kPhase2Options, selectedIndexOf(m_phase2Select)).auth;
  credentials.identity = valueOf(m_identityInput);
  credentials.anonymousIdentity = valueOf(m_anonymousIdentityInput);
  credentials.password = password;
  credentials.caCertPath = valueOf(m_caCertInput);
  credentials.domainSuffixMatch = valueOf(m_domainMatchInput);
  return credentials;
}

std::string NetworkTab::enterpriseBlockReason(const AccessPointInfo& ap) const {
  if (m_network == nullptr || !m_network->supportsEnterprise()) {
    return i18n::tr("control-center.network.error-enterprise-unsupported");
  }
  if (!network_enterprise::passwordAuthUsable(ap.keyManagement)) {
    return i18n::tr("control-center.network.error-certificate-required");
  }
  return {};
}

// True while the access point being joined is still a row the user can see.
bool NetworkTab::pendingAccessPointListed() const {
  if (!m_pendingAccessPoint.has_value() || !networkAvailable() || !m_network->state().wirelessEnabled) {
    return false;
  }
  return std::ranges::any_of(m_network->accessPoints(), [this](const AccessPointInfo& ap) {
    return !ap.active && ap.ssid == m_pendingAccessPoint->ssid;
  });
}

void NetworkTab::showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request) {
  // A form the user is filling outranks NM's own secret request: letting it
  // through would replace the card mid-typing and discard what has been entered.
  // Decline instead, so NM stops waiting rather than blocking on a prompt that
  // will never be answered.
  if (m_hasPendingSecret && m_pendingAccessPoint.has_value() && pendingAccessPointListed()) {
    if (m_secrets != nullptr) {
      m_secrets->cancelSecret();
    }
    return;
  }

  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = request.ssid;
  // NM is asking for one secret against a profile it already holds, so only the
  // password is missing; the rest of the 802.1X form would have nothing to fill.
  m_pendingEnterprise = false;
  m_focusPasswordPrompt = m_active;
  if (m_listScroll != nullptr) {
    m_listScroll->setScrollOffset(0.0F);
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::showPasswordPrompt(const AccessPointInfo& ap) {
  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = ap.ssid;
  m_pendingAccessPoint = ap;
  m_pendingEnterprise = ap.isEnterprise();
  m_focusPasswordPrompt = m_active;
  if (m_pendingEnterprise) {
    // Say up front when this network cannot be joined with a password, rather
    // than after the user has filled in the whole form.
    setCredentialError(enterpriseBlockReason(ap));
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::submitPasswordPrompt(const std::string& value) {
  if (m_pendingAccessPoint.has_value() && m_pendingEnterprise) {
    if (m_network == nullptr) {
      return;
    }
    // Every rejection below leaves the card open with a reason showing, so the
    // user can fix the field rather than watch the prompt close and nothing happen.
    if (const auto blocked = enterpriseBlockReason(*m_pendingAccessPoint); !blocked.empty()) {
      setCredentialError(blocked);
      PanelManager::instance().refresh();
      return;
    }
    const auto credentials = collectEnterpriseCredentials(value);
    const auto problem = network_enterprise::validate(credentials);
    if (problem != network_enterprise::Validation::Ok) {
      setCredentialError(validationMessage(problem));
      PanelManager::instance().refresh();
      return;
    }
    m_network->activateEnterpriseAccessPoint(*m_pendingAccessPoint, credentials);
  } else if (m_pendingAccessPoint.has_value()) {
    if (value.empty()) {
      return;
    }
    if (m_network != nullptr) {
      m_network->activateAccessPoint(*m_pendingAccessPoint, value);
    }
  } else if (m_secrets != nullptr) {
    m_secrets->submitSecret(value);
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::cancelPasswordPrompt() {
  if (!m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::clearPasswordPrompt() {
  m_hasPendingSecret = false;
  m_pendingEnterprise = false;
  m_pendingSsid.clear();
  m_pendingAccessPoint.reset();
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordRevealed(false);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  for (Input* input : {m_identityInput, m_anonymousIdentityInput, m_caCertInput, m_domainMatchInput}) {
    if (input != nullptr) {
      input->setValue("");
    }
  }
  if (m_eapSelect != nullptr) {
    m_eapSelect->setSelectedIndexSilently(kDefaultOptionIndex);
  }
  if (m_phase2Select != nullptr) {
    m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
    m_phase2Select->setEnabled(false); // PEAP is the default, and it pins MSCHAPv2.
  }
  setCredentialError({});
}

bool NetworkTab::networkAvailable() const noexcept { return m_network != nullptr && m_network->available(); }

void NetworkTab::syncCurrentCard() {
  if (!networkAvailable()) {
    if (m_wifiToggle != nullptr) {
      m_wifiToggle->setChecked(false);
      m_wifiToggle->setEnabled(false);
    }
    return;
  }
  const NetworkState& s = m_network->state();
  if (m_actionPending) {
    const bool flipped = s.connected != m_actionPendingConnected;
    const bool timedOut = std::chrono::steady_clock::now() - m_actionPendingSince > kActionPendingTimeout;
    if (flipped || timedOut) {
      m_actionPending = false;
      m_actionPendingTimer.stop();
    }
  }
  if (m_wifiTogglePending) {
    if (s.wirelessEnabled == m_wifiToggleTarget) {
      m_wifiToggleTargetObserved = true;
    }
    if (m_wifiToggleWriteComplete && m_wifiToggleTargetObserved) {
      m_wifiTogglePending = false;
      m_wifiToggleWriteComplete = false;
      m_wifiToggleTargetObserved = false;
    }
  }
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setChecked(m_wifiTogglePending ? m_wifiToggleTarget : s.wirelessEnabled);
    m_wifiToggle->setEnabled(!m_wifiTogglePending);
  }
  if (m_scanSpinner != nullptr) {
    m_scanSpinner->setVisible(s.scanning);
    if (s.scanning && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!s.scanning && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
  if (m_rescanButton != nullptr) {
    m_rescanButton->setVisible(!s.scanning);
  }
}
void NetworkTab::beginPendingAction(bool wasConnected) {
  m_actionPending = true;
  m_actionPendingConnected = wasConnected;
  m_actionPendingSince = std::chrono::steady_clock::now();
  m_actionPendingTimer.start(kActionPendingTimeout + std::chrono::milliseconds(50), [] {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

void NetworkTab::requestWirelessEnabled(bool enabled) {
  m_wifiTogglePending = true;
  m_wifiToggleTarget = enabled;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  const std::uint64_t generation = ++m_wifiToggleRequestGeneration;
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setEnabled(false);
  }
  if (m_network == nullptr) {
    handleWirelessEnabledCompletion(generation, false);
    return;
  }
  m_network->setWirelessEnabled(enabled, [this, generation](bool success) {
    handleWirelessEnabledCompletion(generation, success);
  });
}

void NetworkTab::handleWirelessEnabledCompletion(std::uint64_t generation, bool success) {
  if (!m_wifiTogglePending || generation != m_wifiToggleRequestGeneration) {
    return;
  }
  m_wifiToggleWriteComplete = success;
  if (!success) {
    m_wifiTogglePending = false;
    m_wifiToggleTargetObserved = false;
  }
  PanelManager::instance().requestUpdateOnly();
  PanelManager::instance().requestRedraw();
}

void NetworkTab::rebuildApList(Renderer& renderer) {
  uiAssertNotRendering("NetworkTab::rebuildApList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  const bool available = networkAvailable();
  const NetworkState state = available ? m_network->state() : NetworkState{};

  if (m_hasPendingSecret && m_pendingAccessPoint.has_value() && !pendingAccessPointListed()) {
    clearPasswordPrompt();
  }
  std::vector<AccessPointInfo> aps;
  if (available) {
    aps = sortedAccessPoints(m_network->accessPoints());
    if (m_hasPendingSecret) {
      // Keep row order fixed while a form is open, so a signal change never moves the form's row.
      std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) { return a.ssid < b.ssid; });
    }
  }
  const AccessPointInfo* activeAp = nullptr;
  for (const auto& ap : aps) {
    if (ap.active) {
      activeAp = &ap;
    }
  }

  const auto refresh = [] { PanelManager::instance().refresh(); };
  const std::string onText = i18n::tr("control-center.quick.on");
  const std::string offText = i18n::tr("control-center.quick.off");

  // Card 1: the current connection, wired link, hotspot, VPNs and cellular modems.
  std::vector<RowModel> card1;
  const bool wifiConnected = state.kind == NetworkConnectivity::Wireless && state.connected && !state.ssid.empty();
  if (available) {
    RowModel wifi;
    if (!state.wirelessEnabled) {
      wifi.glyph = "wifi-off";
      wifi.title = i18n::tr("control-center.network.wifi-off-state");
    } else if (wifiConnected) {
      const char* band = network_display::wifiFrequencyBandLabel(state.frequencyMhz);
      wifi.glyph = "wifi";
      wifi.on = true;
      wifi.current = true;
      wifi.title = state.ssid;
      wifi.caption = band != nullptr ? i18n::tr("control-center.network.connected-band", "band", band)
                                     : i18n::tr("control-center.network.connected");
      wifi.signal = network_display::wifiGlyphForSignal(activeAp != nullptr ? activeAp->strength : state.signalStrength);
      wifi.lock = activeAp != nullptr && activeAp->secured ? "lock" : "";
      wifi.ssid = state.ssid;
      if (m_currentExpanded) {
        static const std::string kNoExternalIp;
        const std::string& externalIp =
            m_externalIpService != nullptr ? m_externalIpService->externalIp() : kNoExternalIp;
        wifi.extra = std::string(m_network->hasSavedConnection(state.ssid) ? "1" : "0") +
                     (m_actionPending ? "1" : "0") +
                     currentDetail(state, externalIp, m_modem != nullptr ? m_modem->primaryModem() : nullptr);
      }
      wifi.onClick = [this, refresh] {
        m_currentExpanded = !m_currentExpanded;
        refresh();
      };
    } else {
      wifi.glyph = "wifi";
      wifi.title = i18n::tr("control-center.network.not-connected");
      wifi.caption = i18n::tr("control-center.network.wifi-on");
    }
    card1.push_back(std::move(wifi));

    const bool wiredConnected = state.kind == NetworkConnectivity::Wired && state.connected;
    if (wiredConnected || m_network->canActivateWiredConnection()) {
      RowModel wired;
      wired.glyph = wiredConnected ? "ethernet" : "ethernet-off";
      wired.on = wiredConnected;
      wired.title = i18n::tr("control-center.network.ethernet");
      wired.caption = wiredConnected && !state.ipv4.empty()
          ? i18n::tr("control-center.network.connected") + " · " + state.ipv4
          : i18n::tr(wiredConnected ? "control-center.network.connected" : "control-center.network.not-connected");
      wired.onClick = [this, wiredConnected, refresh] {
        if (m_network == nullptr || m_actionPending) {
          return;
        }
        if (wiredConnected) {
          m_network->disconnect();
        } else if (!m_network->activateWiredConnection()) {
          return;
        }
        beginPendingAction(wiredConnected);
        refresh();
      };
      card1.push_back(std::move(wired));
    }

    if (m_hotspot != nullptr) {
      const bool active = m_hotspot->active();
      RowModel hotspot;
      hotspot.glyph = active ? "access-point" : "access-point-off";
      hotspot.on = active;
      hotspot.title = i18n::tr("control-center.network.hotspot");
      hotspot.caption = active ? onText : offText;
      hotspot.onClick = [this, refresh] {
        (void)m_hotspot->setEnabled(!m_hotspot->active());
        refresh();
      };
      card1.push_back(std::move(hotspot));
    }

    for (const auto& vpn : m_network->vpnConnections()) {
      RowModel row;
      row.glyph = network_display::vpnGlyph();
      row.on = vpn.active;
      row.title = vpn.name;
      row.caption = vpn.active ? onText : offText;
      row.onClick = [this, vpn, refresh] {
        if (m_network != nullptr) {
          vpn.active ? m_network->deactivateVpnConnection(vpn) : m_network->activateVpnConnection(vpn);
        }
        refresh();
      };
      card1.push_back(std::move(row));
    }
  }

  std::vector<CellularModemInfo> modems;
  if (available && m_modem != nullptr) {
    modems = m_modem->modems();
  }

  // Card 2: the networks that can be joined.
  std::vector<RowModel> card2;
  if (available && state.wirelessEnabled) {
    for (const auto& ap : aps) {
      if (ap.active) {
        continue;
      }
      const bool saved = m_network->hasSavedConnection(ap.ssid);
      RowModel row;
      row.ssid = ap.ssid;
      row.glyph = "wifi";
      row.title = ap.ssid;
      row.lock = ap.secured ? "lock" : "";
      row.signal = network_display::wifiGlyphForSignal(ap.strength);
      row.forget = saved;
      row.extra = std::string(1, static_cast<char>('0' + static_cast<int>(ap.keyManagement))) +
                  (ap.requiresCredentials() ? "1" : "0");
      row.onClick = [this, ap, refresh] {
        if (m_network == nullptr) {
          return;
        }
        if (ap.requiresCredentials() && !m_network->hasSavedConnection(ap.ssid)) {
          if (m_pendingAccessPoint.has_value() && m_pendingAccessPoint->ssid == ap.ssid) {
            cancelPasswordPrompt();
          } else {
            showPasswordPrompt(ap);
            refresh();
          }
          return;
        }
        m_network->activateAccessPoint(ap);
      };
      row.onForget = [this, ssid = ap.ssid, refresh] {
        if (m_network != nullptr) {
          m_network->forgetSsid(ssid);
        }
        refresh();
      };
      card2.push_back(std::move(row));
    }
  }

  std::string key;
  key += available ? "avail\n" : "unavail\n";
  key += state.wirelessEnabled ? "wifi\n" : "nowifi\n";
  key += state.scanning ? "scan\n" : "idle\n";
  key += m_currentExpanded ? "expanded\n" : "collapsed\n";
  for (const auto& row : card1) {
    appendKey(key, row);
  }
  key += "---\n";
  for (const auto& row : card2) {
    appendKey(key, row);
  }
  key += "---\n";
  // Cellular rows live-sync their state and signal; only which modems exist is structural.
  for (const auto& modem : modems) {
    key += modem.path + ':' + modem.name + '\n';
  }
  if (m_hasPendingSecret) {
    key += "pending:" + m_pendingSsid + (m_pendingAccessPoint.has_value() ? ":ap" : ":agent") +
           (m_pendingEnterprise ? ":eap" : ":psk") + '\n';
  }
  if (listWidth == m_lastListWidth && key == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = key;

  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_cellularRows.clear();
  m_apSignals.clear();

  if (m_passwordCard != nullptr && m_passwordCard->parent() != nullptr) {
    auto detached = m_passwordCard->parent()->removeChild(m_passwordCard);
    m_passwordHolder.reset(static_cast<Flex*>(detached.release()));
  }
  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (!available) {
    auto card = makeCard(scale, opacity);
    Label* title = nullptr;
    Label* detail = nullptr;
    card->addChild(makeLabel(title, Tokens::typeHeadline, scale, ColorRole::Label1));
    card->addChild(makeLabel(detail, Tokens::typeFootnote, scale, ColorRole::Label2));
    detail->setMaxLines(4);
    title->setText(i18n::tr("control-center.network.unavailable-title"));
    detail->setText(i18n::tr("control-center.network.unavailable-detail"));
    m_list->addChild(std::move(card));
    if (m_hasPendingSecret && !m_pendingAccessPoint.has_value() && m_passwordHolder != nullptr) {
      auto box = makeCard(scale, opacity);
      m_passwordTitle->setVisible(true);
      box->addChild(std::move(m_passwordHolder));
      m_list->addChild(std::move(box));
    }
    m_list->layout(renderer);
    return;
  }

  const float indent = (kRoundIcon + Tokens::spaceP12) * scale;
  const bool agentPrompt = m_hasPendingSecret && !m_pendingAccessPoint.has_value();
  bool formPlaced = false;
  if (m_passwordTitle != nullptr) {
    m_passwordTitle->setVisible(agentPrompt);
  }

  auto card1Box = makeCard(scale, opacity);
  for (const auto& model : card1) {
    RowUi ui;
    auto wrapper = makeColumn(Tokens::spaceP8 * scale);
    wrapper->addChild(makeRow(model, scale, ui));
    if (model.current && ui.signal != nullptr) {
      m_apSignals[model.ssid] = ui.signal;
    }
    if (model.current && m_currentExpanded) {
      auto details = makeColumn(Tokens::spaceP8 * scale);
      details->setPadding(0.0F, 0.0F, 0.0F, indent);
      Label* detailText = nullptr;
      details->addChild(makeLabel(detailText, Tokens::typeFootnote, scale, ColorRole::Label2));
      detailText->setMaxLines(3);
      static const std::string kNoExternalIp;
      const std::string& externalIp = m_externalIpService != nullptr ? m_externalIpService->externalIp() : kNoExternalIp;
      detailText->setText(currentDetail(state, externalIp, m_modem != nullptr ? m_modem->primaryModem() : nullptr));

      auto actions = std::make_unique<Flex>();
      actions->setDirection(FlexDirection::Horizontal);
      actions->setAlign(FlexAlign::Center);
      actions->setGap(Tokens::spaceP8 * scale);
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.network.disconnect"),
              .enabled = !m_actionPending,
              .variant = ButtonVariant::Secondary,
              .onClick = [this, refresh] {
                if (m_network != nullptr && !m_actionPending) {
                  m_network->disconnect();
                  beginPendingAction(true);
                }
                m_currentExpanded = false;
                refresh();
              },
          })
      );
      if (m_network->hasSavedConnection(state.ssid)) {
        actions->addChild(
            ui::button({
                .text = i18n::tr("control-center.network.forget"),
                .variant = ButtonVariant::Ghost,
                .onClick = [this, ssid = state.ssid, refresh] {
                  if (m_network != nullptr) {
                    m_network->forgetSsid(ssid);
                  }
                  m_currentExpanded = false;
                  refresh();
                },
            })
        );
      }
      details->addChild(std::move(actions));
      wrapper->addChild(std::move(details));
    }
    card1Box->addChild(std::move(wrapper));
  }
  for (const auto& modem : modems) {
    RowModel model;
    model.glyph = cellularGlyphFor(modem);
    model.on = cellularToggleDisplayChecked();
    model.title = cellularTitleFor(modem);
    model.caption = cellularDetailFor(modem);
    model.onClick = [this, refresh] {
      requestCellularEnabled(!cellularToggleDisplayChecked());
      refresh();
    };
    RowUi ui;
    card1Box->addChild(makeRow(model, scale, ui));
    m_cellularRows.push_back({.icon = ui.icon, .title = ui.title, .caption = ui.caption});
  }
  m_list->addChild(std::move(card1Box));

  if (agentPrompt && m_passwordHolder != nullptr) {
    auto box = makeCard(scale, opacity);
    box->addChild(std::move(m_passwordHolder));
    m_list->addChild(std::move(box));
    formPlaced = true;
  }

  if (!state.wirelessEnabled) {
    m_list->layout(renderer);
    return;
  }

  auto header = std::make_unique<Flex>();
  header->setDirection(FlexDirection::Horizontal);
  header->setAlign(FlexAlign::Center);
  header->setGap(Tokens::spaceP8 * scale);
  header->setPadding(0.0F, 0.0F, 0.0F, Tokens::componentTilePadding * scale);
  Label* sectionTitle = nullptr;
  auto sectionLabel = makeLabel(sectionTitle, Tokens::typeFootnote, scale, ColorRole::Label2);
  sectionLabel->setFlexGrow(1.0F);
  sectionTitle->setText(i18n::tr("control-center.network.other-networks"));
  header->addChild(std::move(sectionLabel));
  header->addChild(
      ui::spinner({
          .out = &m_scanSpinner,
          .color = colorSpecFromRole(ColorRole::Label2),
          .spinnerSize = Tokens::componentRoundIconGlyph * scale,
          .visible = false,
      })
  );
  header->addChild(
      ui::button({
          .out = &m_rescanButton,
          .glyph = "refresh",
          .glyphSize = Tokens::componentRoundIconGlyph * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.network.refresh"),
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this] {
            if (m_network != nullptr) {
              m_network->requestScan();
            }
          },
      })
  );
  m_list->addChild(std::move(header));

  auto card2Box = makeCard(scale, opacity);
  if (card2.empty()) {
    Label* empty = nullptr;
    card2Box->addChild(makeLabel(empty, Tokens::typeFootnote, scale, ColorRole::Label2));
    empty->setText(i18n::tr("control-center.network.no-networks-short"));
  }
  for (const auto& model : card2) {
    RowUi ui;
    auto wrapper = makeColumn(Tokens::spaceP8 * scale);
    wrapper->addChild(makeRow(model, scale, ui));
    if (ui.signal != nullptr) {
      m_apSignals[model.ssid] = ui.signal;
    }
    if (!formPlaced && m_hasPendingSecret && m_pendingAccessPoint.has_value() && m_pendingAccessPoint->ssid == model.ssid &&
        m_passwordHolder != nullptr) {
      wrapper->addChild(std::move(m_passwordHolder));
      formPlaced = true;
    }
    card2Box->addChild(std::move(wrapper));
  }
  m_list->addChild(std::move(card2Box));

  // Live state (spinner, switch) belongs to syncCurrentCard(); this builds structure only.
  m_list->layout(renderer);
}

bool NetworkTab::syncApRows() {
  if (m_network == nullptr || m_apSignals.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apSignals.find(ap.ssid);
    if (it != m_apSignals.end() && it->second->setGlyph(network_display::wifiGlyphForSignal(ap.strength))) {
      changed = true;
    }
  }
  return changed;
}

bool NetworkTab::syncCellularCard() {
  if (m_modem == nullptr) {
    return false;
  }
  if (m_cellularTogglePending) {
    // Bringing mobile data up walks Enabling -> Searching -> Registered -> Connecting, so the requested
    // position is held instead of snapping back while the modem works.
    const bool reached = cellularToggleChecked() == m_cellularToggleTarget;
    const bool timedOut = std::chrono::steady_clock::now() - m_cellularTogglePendingSince > kCellularPendingTimeout;
    if (reached || timedOut) {
      m_cellularTogglePending = false;
      m_cellularTogglePendingTimer.stop();
    }
  }
  const auto& modems = m_modem->modems();
  bool changed = false;
  const std::size_t count = std::min(m_cellularRows.size(), modems.size());
  for (std::size_t i = 0; i < count; ++i) {
    auto& row = m_cellularRows[i];
    if (row.icon == nullptr) {
      continue;
    }
    row.icon->setVariant(cellularToggleDisplayChecked() ? ButtonVariant::Primary : ButtonVariant::Tonal);
    row.icon->setGlyph(cellularGlyphFor(modems[i]));
    changed = row.title->setText(cellularTitleFor(modems[i])) || changed;
    changed = row.caption->setText(cellularDetailFor(modems[i])) || changed;
  }
  return changed;
}
bool NetworkTab::cellularToggleChecked() const {
  if (m_network != nullptr && m_network->canActivateCellularConnection()) {
    return m_network->state().cellularActive;
  }
  if (m_modem != nullptr) {
    return std::ranges::any_of(m_modem->modems(), [](const CellularModemInfo& modem) { return modem.enabled(); });
  }
  return false;
}

// The requested position while a toggle request is in flight, the observed one
// otherwise.
bool NetworkTab::cellularToggleDisplayChecked() const {
  return m_cellularTogglePending ? m_cellularToggleTarget : cellularToggleChecked();
}

void NetworkTab::requestCellularEnabled(bool enabled) {
  const bool nmCellular = m_network != nullptr && m_network->canActivateCellularConnection();
  if (enabled) {
    // GNOME parity: powering the modem is a prerequisite; the gsm connection is
    // what actually brings up mobile data.
    if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(true);
    }
    if (nmCellular) {
      m_network->activateCellularConnection();
    }
  } else {
    // Data off while the modem stays registered, like GNOME's mobile-data
    // switch. Without an NM gsm profile there is only modem power to cut.
    if (nmCellular) {
      m_network->deactivateCellularConnection();
    } else if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(false);
    }
  }
  m_cellularTogglePending = true;
  m_cellularToggleTarget = enabled;
  m_cellularTogglePendingSince = std::chrono::steady_clock::now();
  // Nothing else wakes the panel if the request never changes any state.
  m_cellularTogglePendingTimer.start(kCellularPendingTimeout + std::chrono::milliseconds(50), [] {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

void NetworkTab::onPanelCardOpacityChanged(float opacity) {
  if (m_passwordInput != nullptr) {
    m_passwordInput->setSurfaceOpacity(opacity);
  }
}

#pragma once

#include "design_tokens.h"
#include "shell/panel/panel.h"
#include "shell/polkit/auth_dialog_state.h"
#include "system/icon_resolver.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

class Button;
class Flex;
class Glyph;
class Image;
class Input;
class InputArea;
class Label;
class Node;
class AskpassBroker;
class PolkitAgent;
struct PolkitRequest;
class Renderer;
class Select;
class ConfigService;

class PolkitPanel : public Panel {
public:
  // `authMode` says which request the dialog shows; the panel reports opens, closes and cancels to `authEvent`.
  PolkitPanel(
      ConfigService* config, std::function<PolkitAgent*()> agentProvider, AskpassBroker* askpass,
      std::function<AuthDialogMode()> authMode, std::function<void(AuthDialogEvent)> authEvent
  );

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void clearInput();

  [[nodiscard]] float preferredWidth() const override { return scaled(Tokens::componentDialogWidth); }
  // Fits its content (the prompt grows when a password is needed); this is the maximum.
  [[nodiscard]] float preferredHeight() const override { return scaled(640.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit(std::string_view response = {});
  void cancelAuth();
  bool handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void resolveIcon(Renderer& renderer, const PolkitRequest& request);
  void updateIdentity(const PolkitRequest& request);
  void updateAskpass();
  [[nodiscard]] bool showsAskpass() const { return m_authMode && m_authMode() == AuthDialogMode::Askpass; }

  ConfigService* m_config = nullptr;
  std::function<PolkitAgent*()> m_agentProvider;
  Flex* m_rootLayout = nullptr;
  InputArea* m_focusArea = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_messageLabel = nullptr;
  Label* m_promptLabel = nullptr;
  Label* m_identityLabel = nullptr;
  Select* m_identitySelect = nullptr;
  // Offered unix-user uids, in m_identitySelect option order.
  std::vector<std::uint32_t> m_identityUids;
  Glyph* m_faceGlyph = nullptr;
  Label* m_supplementaryLabel = nullptr;
  Input* m_input = nullptr;
  Button* m_submitButton = nullptr;
  Button* m_cancelButton = nullptr;
  Node* m_iconContainer = nullptr;
  Image* m_icon = nullptr;
  Glyph* m_fallbackIcon = nullptr;
  IconResolver m_iconResolver;
  std::string m_lastIconName;
  bool m_iconResolved = false;
  bool m_lastResponseRequired = false;
  AskpassBroker* m_askpass = nullptr;
  std::function<AuthDialogMode()> m_authMode;
  std::function<void(AuthDialogEvent)> m_authEvent;
};

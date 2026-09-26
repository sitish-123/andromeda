#pragma once

#include "shell/panel/panel.h"
#include "wayland/mirror_control.h"
#include "wayland/output_management.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Button;
class Flex;
class Label;
class Renderer;
class WaylandConnection;

// Super+P: pick how the laptop panel and a second display share the picture.
class DisplayModePanel : public Panel {
public:
  enum class Mode : std::uint8_t {
    InternalOnly,
    Duplicate,
    Extend,
    ExternalOnly,
  };

  explicit DisplayModePanel(WaylandConnection& wayland) : m_wayland(wayland) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] std::string panelScreenPosition() const override { return "center"; }

private:
  static constexpr std::size_t kModeCount = 4;
  static constexpr std::uint8_t kMaxApplyAttempts = 3;

  void doLayout(Renderer& renderer, float width, float height) override;
  void choose(Mode mode);
  void startRequested();
  // Advances the pending mode one step each time the compositor reports new state: clear a mirror, apply the output
  // layout, then start a mirror. Finishing closes the panel.
  void step();
  void fail(std::string message);
  void refreshVisuals();
  [[nodiscard]] const OutputHead* internalHead() const;
  [[nodiscard]] const OutputHead* externalHead() const;

  WaylandConnection& m_wayland;
  std::unique_ptr<OutputManagement> m_outputs;
  std::unique_ptr<MirrorControl> m_mirrors;
  Flex* m_rootLayout = nullptr;
  Label* m_status = nullptr;
  std::array<Button*, kModeCount> m_buttons{};
  std::optional<Mode> m_pending;
  // Mode named by the open context (panel-open display-mode extend), started once state arrives.
  std::optional<Mode> m_requested;
  std::size_t m_selected = 0;
  std::uint8_t m_applyAttempts = 0;
  bool m_mirrorRequested = false;
  std::string m_statusText;
};

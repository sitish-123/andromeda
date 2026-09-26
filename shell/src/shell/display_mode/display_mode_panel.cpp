#include "shell/display_mode/display_mode_panel.h"

#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_content_displays.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr float kButtonWidth = 176.0F;
  constexpr float kButtonHeight = 112.0F;

  struct ModeMeta {
    DisplayModePanel::Mode mode;
    std::string_view context; // panel-open display-mode <context>
    std::string_view labelKey;
    std::string_view glyph;
  };

  constexpr std::array<ModeMeta, 4> kModes = {{
      {DisplayModePanel::Mode::InternalOnly, "internal-only", "display-mode.internal-only", "device-laptop"},
      {DisplayModePanel::Mode::Duplicate, "duplicate", "display-mode.duplicate", "copy"},
      {DisplayModePanel::Mode::Extend, "extend", "display-mode.extend", "layout-columns"},
      {DisplayModePanel::Mode::ExternalOnly, "external-only", "display-mode.external-only", "device-desktop"},
  }};

  // Connector prefixes of built-in panels.
  bool isInternal(std::string_view connector) {
    return connector.starts_with("eDP") || connector.starts_with("LVDS") || connector.starts_with("DSI");
  }

  zwlr_output_mode_v1* modeToEnable(const OutputHead& head) {
    if (head.currentMode != nullptr) {
      return head.currentMode;
    }
    const auto preferred = std::ranges::find_if(head.modes, &OutputMode::preferred);
    if (preferred != head.modes.end()) {
      return preferred->handle;
    }
    return head.modes.empty() ? nullptr : head.modes.front().handle;
  }

} // namespace

const OutputHead* DisplayModePanel::internalHead() const {
  const auto& heads = m_outputs->heads();
  const auto it = std::ranges::find_if(heads, [](const OutputHead& h) { return isInternal(h.name); });
  // Two external monitors: the first one plays the laptop's part.
  return it != heads.end() ? &*it : (heads.empty() ? nullptr : &heads.front());
}

const OutputHead* DisplayModePanel::externalHead() const {
  const OutputHead* internal = internalHead();
  for (const OutputHead& head : m_outputs->heads()) {
    if (&head != internal) {
      return &head;
    }
  }
  return nullptr;
}

float DisplayModePanel::preferredWidth() const {
  return scaled(kButtonWidth * kModeCount + Style::spaceMd * (kModeCount - 1) + Style::panelPadding * 2.0F);
}

float DisplayModePanel::preferredHeight() const {
  return std::ceil(scaled(kButtonHeight + Style::spaceMd + Style::fontSizeBody * 1.6F + Style::panelPadding * 2.0F));
}

void DisplayModePanel::create() {
  const float scale = contentScale();
  auto rootLayout = ui::column({.out = &m_rootLayout, .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale});
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  for (std::size_t i = 0; i < kModeCount; ++i) {
    row->addChild(
        ui::button({
            .out = &m_buttons[i],
            .text = i18n::tr(kModes[i].labelKey),
            .glyph = std::string(kModes[i].glyph),
            .fontSize = (Style::fontSizeBody + 1.0F) * scale,
            .glyphSize = 28.0F * scale,
            .contentAlign = ButtonContentAlign::Center,
            .surfaceOpacity = panelCardOpacity(),
            .badge = std::to_string(i + 1),
            .minWidth = kButtonWidth * scale,
            .minHeight = kButtonHeight * scale,
            .gap = Style::spaceSm * scale,
            .radius = Style::scaledRadiusLg(scale),
            .flexGrow = 1.0F,
            .onClick = [this, i]() { choose(kModes[i].mode); },
            .configure =
                [](Button& control) {
                  control.setDirection(FlexDirection::Vertical);
                  control.setAlign(FlexAlign::Center);
                  control.setJustify(FlexJustify::Center);
                  control.setTabStop(false);
                },
        })
    );
  }
  rootLayout->addChild(std::move(row));
  rootLayout->addChild(
      ui::label({
          .out = &m_status,
          .text = m_statusText,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  setRoot(std::move(rootLayout));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
  refreshVisuals();
}

void DisplayModePanel::onOpen(std::string_view context) {
  m_requested.reset();
  if (const auto meta = std::ranges::find(kModes, context, &ModeMeta::context); meta != kModes.end()) {
    m_requested = meta->mode;
  }
  m_pending.reset();
  m_selected = 0;
  m_statusText.clear();
  const auto [outputName, outputVersion] = m_wayland.outputManagerGlobal();
  m_outputs = std::make_unique<OutputManagement>(m_wayland.registry(), outputName, outputVersion, [this]() {
    refreshVisuals();
    startRequested();
    step();
  });
  if (const auto [mirrorName, mirrorVersion] = m_wayland.desktopOutputGlobal(); mirrorName != 0) {
    m_mirrors = std::make_unique<MirrorControl>(m_wayland.registry(), mirrorName, mirrorVersion, [this]() {
      if (!m_mirrors->lastFailure().empty() && m_pending) {
        fail(m_mirrors->lastFailure());
        return;
      }
      refreshVisuals();
      startRequested();
      step();
    });
  }
}

void DisplayModePanel::startRequested() {
  if (m_requested && m_outputs->ready() && (m_mirrors == nullptr || m_mirrors->ready())) {
    const Mode mode = *m_requested;
    m_requested.reset();
    choose(mode);
  }
}

void DisplayModePanel::onClose() {
  m_outputs.reset();
  m_mirrors.reset();
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_buttons = {};
  clearReleasedRoot();
}

void DisplayModePanel::choose(Mode mode) {
  if (m_outputs == nullptr || !m_outputs->ready() || externalHead() == nullptr) {
    return;
  }
  if (mode == Mode::Duplicate && m_mirrors == nullptr) {
    fail(i18n::tr("display-mode.no-mirroring"));
    return;
  }
  m_pending = mode;
  m_applyAttempts = 0;
  m_mirrorRequested = false;
  step();
}

void DisplayModePanel::step() {
  if (!m_pending
      || m_outputs == nullptr
      || !m_outputs->ready()
      || m_outputs->busy()
      || (m_mirrors != nullptr && !m_mirrors->ready())) {
    return;
  }
  const OutputHead* internal = internalHead();
  const OutputHead* external = externalHead();
  if (internal == nullptr || external == nullptr) {
    fail(i18n::tr("display-mode.no-external"));
    return;
  }
  const Mode mode = *m_pending;
  const bool mirrored = m_mirrors != nullptr && m_mirrors->mirrors().contains(external->name);

  if (mode != Mode::Duplicate && mirrored) {
    // Clearing changes the output state; the next done event carries the serial the layout apply needs.
    if (!m_mirrorRequested) {
      m_mirrorRequested = true;
      m_mirrors->clearMirror(external->name);
    }
    return;
  }
  m_mirrorRequested = mode == Mode::Duplicate && m_mirrorRequested;

  OutputHeadConfig in = OutputManagement::currentConfig(*internal);
  OutputHeadConfig ex = OutputManagement::currentConfig(*external);
  in.enabled = mode != Mode::ExternalOnly;
  ex.enabled = mode != Mode::InternalOnly;
  if (in.enabled && in.mode == nullptr) {
    in.mode = modeToEnable(*internal);
  }
  if (ex.enabled && ex.mode == nullptr) {
    ex.mode = modeToEnable(*external);
  }
  if (mode == Mode::Extend) {
    std::tie(ex.x, ex.y) = settings::displayPlacementPosition(
        in, settings::displayLogicalSize(*internal, in), settings::displayLogicalSize(*external, ex),
        settings::DisplayPlacement::RightOf
    );
  } else if (mode == Mode::ExternalOnly) {
    ex.x = 0;
    ex.y = 0;
  }

  const bool layoutMatches = OutputManagement::currentConfig(*internal).enabled == in.enabled
      && OutputManagement::currentConfig(*external).enabled == ex.enabled
      && (mode != Mode::Extend || (external->x == ex.x && external->y == ex.y));
  if (!layoutMatches) {
    if (++m_applyAttempts > kMaxApplyAttempts) {
      fail(i18n::tr("display-mode.failed"));
      return;
    }
    const std::vector<OutputHeadConfig> config = {in, ex};
    (void)m_outputs->apply(config, /*testOnly=*/false, [this](OutputApplyResult result) {
      if (result == OutputApplyResult::Failed) {
        fail(i18n::tr("display-mode.failed"));
      }
      // Success and cancellation both end in a done event, which calls step() again.
    });
    return;
  }

  if (mode == Mode::Duplicate && !mirrored) {
    if (!m_mirrorRequested) {
      m_mirrorRequested = true;
      m_mirrors->setMirror(external->name, internal->name);
    }
    return;
  }

  m_pending.reset();
  DeferredCall::callLater([]() { PanelManager::instance().closePanel(); });
}

void DisplayModePanel::fail(std::string message) {
  m_pending.reset();
  m_statusText = std::move(message);
  refreshVisuals();
}

void DisplayModePanel::refreshVisuals() {
  const bool hasExternal = m_outputs != nullptr && m_outputs->ready() && externalHead() != nullptr;
  if (m_statusText.empty() && m_outputs != nullptr && m_outputs->ready() && !hasExternal) {
    m_statusText = i18n::tr("display-mode.no-external");
  }
  for (std::size_t i = 0; i < kModeCount; ++i) {
    if (Button* button = m_buttons[i]) {
      button->setEnabled(hasExternal);
      button->setSelected(i == m_selected);
    }
  }
  if (m_status != nullptr) {
    (void)m_status->setText(m_statusText);
  }
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
  PanelManager::instance().refresh();
}

bool DisplayModePanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }
  if (sym >= XKB_KEY_1 && sym < XKB_KEY_1 + kModeCount) {
    m_selected = sym - XKB_KEY_1;
    choose(kModes[m_selected].mode);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    m_selected = (m_selected + 1) % kModeCount;
    refreshVisuals();
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    m_selected = (m_selected + kModeCount - 1) % kModeCount;
    refreshVisuals();
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    choose(kModes[m_selected].mode);
    return true;
  }
  return false;
}

void DisplayModePanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  for (Button* button : m_buttons) {
    if (button != nullptr) {
      button->updateInputArea();
    }
  }
}

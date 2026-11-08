#include "shell/session/power_overlay.h"

#include "cli/schema_msg.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/logind/power_config_service.h"
#include "dbus/portal/inhibit_portal.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_guard.h"
#include "ui/builders.h"
#include "compositors/andromeda/andromeda_runtime.h"
#include "compositors/compositor_platform.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "util/file_utils.h"
#include "config/config_service.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>

namespace {

  constexpr Logger kLog("power-overlay");
  constexpr float kButton = 88.0F;
  constexpr float kGlyph = 28.0F;
  TOKENS_LIVE(kGap, Tokens::spaceP32);
  constexpr float kRing = 6.0F;
  constexpr float kHoldMs = 2000.0F;

  struct ChoiceSpec {
    const char* action;
    const char* glyph;
    const char* label;
  };
  constexpr std::array<ChoiceSpec, 4> kChoices{{
      {"lock", nullptr, "control-center.power-menu.lock"},
      {"suspend", "moon", "control-center.power-menu.sleep"},
      {"reboot", nullptr, "control-center.power-menu.restart"},
      {"shutdown", nullptr, "control-center.power-menu.shut-down"},
  }};
  constexpr std::size_t kShutDown = 3;
  constexpr int kCountdownSeconds = 60;
  constexpr auto kCloseWait = std::chrono::seconds(5);
  constexpr float kIcon = 40.0F;

  const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
    const auto& outputs = wayland.outputs();
    const auto it = std::ranges::find_if(outputs, [output](const WaylandOutput& o) { return o.output == output; });
    return it == outputs.end() ? nullptr : &*it;
  }

  std::string reopenPath() { return FileUtils::stateDir() + "/session-reopen.json"; }

} // namespace

void PowerOverlay::initialize(
    WaylandConnection& wayland, RenderContext* renderContext, ConfigService* config, IpcService* ipc,
    CompositorPlatform* platform
) {
  m_platform = platform;
  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_config = config;
  m_ipc = ipc;
  if (std::ifstream in(reopenPath()); in) {
    m_reopen = nlohmann::json::parse(in, nullptr, false).value("reopen", true);
  }
}

void PowerOverlay::saveReopen(const std::vector<std::string>& apps) const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(reopenPath()).parent_path(), ec);
  std::ofstream out(reopenPath());
  out << nlohmann::json{{"reopen", m_reopen}, {"apps", m_reopen ? apps : std::vector<std::string>{}}}.dump();
}

std::vector<std::string> PowerOverlay::takeReopenApps() {
  std::ifstream in(reopenPath());
  if (!in) {
    return {};
  }
  nlohmann::json state = nlohmann::json::parse(in, nullptr, false);
  if (!state.is_object() || !state.contains("apps") || !state["apps"].is_array()) {
    return {};
  }
  std::vector<std::string> apps = state["apps"].get<std::vector<std::string>>();
  in.close();
  state["apps"] = nlohmann::json::array();
  std::ofstream(reopenPath()) << state.dump();
  return apps;
}

void PowerOverlay::setPowerKeyHandling(SystemBus* bus, const PowerConfigService* power, std::function<bool()> locked) {
  m_power = power;
  m_locked = std::move(locked);
  if (bus == nullptr || m_logind != nullptr) {
    return;
  }
  try {
    m_logind = sdbus::createProxy(
        bus->connection(), sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"}
    );
    // Async: a slow logind must not hold the shell's first frame. The proxy owns the call, so it dies with this.
    m_logind->callMethodAsync("Inhibit")
        .onInterface("org.freedesktop.login1.Manager")
        .withArguments(
            std::string("handle-power-key"), std::string("Andromeda"),
            std::string("The power key opens the power menu"), std::string("block")
        )
        .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::UnixFd fd) {
          if (error.has_value()) {
            // Without the block logind keeps acting on the key itself.
            kLog.warn("power key stays with logind: {}", error->getMessage());
            return;
          }
          m_powerKeyInhibit = std::move(fd);
        });
  } catch (const sdbus::Error& e) {
    // Without the block logind keeps acting on the key itself.
    kLog.warn("power key stays with logind: {}", e.getMessage());
    m_logind.reset();
  }
}

void PowerOverlay::powerKeyDown() {
  const std::string choice = m_power != nullptr ? m_power->state().powerKey : std::string{};
  if (choice == "ignore") {
    return;
  }
  const bool menu = choice != "suspend" && choice != "hibernate" && choice != "suspend-then-hibernate"
      && choice != "lock";
  // logind spells the choice with dashes; the shell's session actions use underscores.
  const auto sessionAction = [this](std::string action) {
    std::ranges::replace(action, '-', '_');
    if (m_ipc != nullptr) {
      (void)m_ipc->execute("session " + action);
    }
  };
  if (m_locked && m_locked()) {
    // The lock screen covers the menu: a press sleeps, as phones and laptops do on their lock screen.
    if (choice != "lock") {
      sessionAction(menu ? std::string("suspend") : choice);
    }
    return;
  }
  if (!menu) {
    sessionAction(choice);
    return;
  }
  // A session end runs its own sheet; the key does not start a second action over it.
  if (m_mode != Mode::Choices) {
    return;
  }
  m_openBeforePress = isActive();
  if (!isActive()) {
    show(nullptr);
  }
  if (isActive()) {
    startHold();
  }
}

void PowerOverlay::powerKeyUp() {
  if (!m_holding || m_holdAnim == 0) {
    // The ring already filled: the fade and shutdown go on.
    return;
  }
  cancelHold();
  if (m_openBeforePress) {
    hide();
  }
}

void PowerOverlay::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::powerOverlay, [this](const std::string& args) -> std::string {
    if (args == "key-down") {
      powerKeyDown();
      return "ok\n";
    }
    if (args == "key-up") {
      powerKeyUp();
      return "ok\n";
    }
    if (args.starts_with("end ")) {
      const std::string action = args.substr(4);
      if (action != "reboot" && action != "shutdown" && action != "logout") {
        return "error: power-overlay end takes reboot, shutdown or logout\n";
      }
      // Without a layer shell (the shell inside another desktop) no sheet or countdown can show, so no Cancel either:
      // a session end the user cannot see never starts.
      if (m_wayland == nullptr || !m_wayland->hasLayerShell()) {
        kLog.warn("{} refused: the compositor has no layer shell for the session-end sheet", action);
        return "error: power-overlay end needs a layer-shell compositor\n";
      }
      beginEnd(action);
      return "ok\n";
    }
    // While a session end runs, the toggle only brings its sheet back (after Show); it never cancels the end.
    if (m_mode == Mode::Closing || m_mode == Mode::Ending) {
      return "ok\n";
    }
    if (m_mode != Mode::Choices) {
      if (!isActive()) {
        restoreSheet();
      }
      return "ok\n";
    }
    if (isActive()) {
      hide();
      return "ok\n";
    }
    show(nullptr);
    return "ok\n";
  });
}

void PowerOverlay::show(wl_output* output) {
  if (m_wayland == nullptr || m_renderContext == nullptr || isActive()) {
    return;
  }
  if (output == nullptr && m_platform != nullptr) {
    output = m_platform->preferredInteractiveOutput();
  }
  if (output == nullptr && !m_wayland->outputs().empty()) {
    output = m_wayland->outputs().front().output;
  }
  const WaylandOutput* info = findOutput(*m_wayland, output);
  if (info == nullptr || !info->hasUsableGeometry()) {
    return;
  }
  m_output = output;
  m_focused = 0;
  m_holding = false;
  m_holdProgress = 0.0F;
  m_surface = std::make_unique<LayerSurface>(
      *m_wayland,
      LayerSurfaceConfig{
          .nameSpace = "noctalia-power-overlay",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .exclusiveZone = -1,
          .keyboard = LayerShellKeyboard::Exclusive,
          .defaultWidth = static_cast<std::uint32_t>(info->effectiveLogicalWidth()),
          .defaultHeight = static_cast<std::uint32_t>(info->effectiveLogicalHeight()),
      }
  );
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });
  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) { prepareFrame(needsUpdate, needsLayout); });
  m_surface->setClosedCallback([this] { DeferredCall::callLater([this] { onSurfaceLost(); }); });
  if (!m_surface->initialize(output)) {
    kLog.warn("failed to initialize the power overlay");
    m_surface.reset();
  }
}

std::vector<std::pair<std::string, std::string>> PowerOverlay::appBlockers() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (m_portal != nullptr) {
    for (const InhibitPortal::LogoutInhibitor& entry : m_portal->logoutInhibitors()) {
      out.emplace_back(entry.appId, entry.reason);
    }
  }
  return out;
}

void PowerOverlay::setPortalState(int state) {
  if (m_portal != nullptr) {
    m_portal->setSessionState(static_cast<InhibitPortal::SessionState>(state));
  }
}

void PowerOverlay::watchGuard(bool on) {
  if (m_guard == nullptr || m_guardWatched == on) {
    return;
  }
  m_guardWatched = on;
  m_guard->watch(on);
}

void PowerOverlay::onGuardChanged() {
  // The countdown alert names other users' sessions; rebuild it when that list changes.
  if (m_mode == Mode::Countdown && isActive()) {
    setMode(Mode::Countdown);
  }
}

void PowerOverlay::hide() {
  watchGuard(false);
  if (m_mode != Mode::Choices && m_mode != Mode::Ending) {
    setPortalState(static_cast<int>(InhibitPortal::SessionState::Running));
  }
  m_appBlockers.clear();
  m_closeTimer.stop();
  m_tickTimer.stop();
  m_mode = Mode::Choices;
  m_blockers.clear();
  hideSurface();
}

void PowerOverlay::hideSurface() {
  if (m_surface == nullptr) {
    return;
  }
  m_input.setSceneRoot(nullptr);
  m_animations.cancelAll();
  m_holdAnim = 0;
  m_holding = false;
  m_countLabel = nullptr;
  m_level = nullptr;
  m_surface->setClosedCallback(nullptr);
  m_surface->setPrepareFrameCallback(nullptr);
  m_surface.reset();
  m_root.reset();
  m_backdrop = nullptr;
  m_content = nullptr;
  m_hint = nullptr;
  m_ring = nullptr;
  m_choices = {};
  m_pointerInside = false;
}

void PowerOverlay::onOutputChange() {
  if (m_wayland != nullptr && isActive() && findOutput(*m_wayland, m_output) == nullptr) {
    onSurfaceLost();
  }
}

void PowerOverlay::onSurfaceLost() {
  if (m_mode == Mode::Countdown || m_mode == Mode::Blockers) {
    restoreSheet();
  } else {
    hide();
  }
}

void PowerOverlay::restoreSheet() {
  const Mode mode = m_mode;
  hideSurface();
  m_countPaused = false;
  show(nullptr);
  if (!isActive()) {
    // No output to show it on: nobody can answer, so the chosen action goes on.
    finishEnd();
    return;
  }
  setMode(mode);
}

void PowerOverlay::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_surface == nullptr || m_renderContext == nullptr) {
    return;
  }
  const auto width = static_cast<float>(m_surface->width());
  const auto height = static_cast<float>(m_surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(m_surface->renderTarget());
  const bool rebuild = m_root == nullptr || m_root->width() != width || m_root->height() != height;
  if (rebuild) {
    UiPhaseScope phase(UiPhase::Layout);
    buildScene(width, height);
    m_surface->setBlurRegion({InputRect{.x = 0, .y = 0, .width = static_cast<int>(width), .height = static_cast<int>(height)}});
  }
  if ((rebuild || needsLayout || needsUpdate) && m_content != nullptr) {
    UiPhaseScope phase(UiPhase::Layout);
    syncFocus();
    auto* content = static_cast<Flex*>(m_content);
    content->layout(m_surface->renderTarget().renderer());
    content->setPosition(std::round((width - content->width()) * 0.5F), std::round((height - content->height()) * 0.5F));
    for (const Choice& choice : m_choices) {
      if (choice.area != nullptr) {
        choice.area->setFrameSize(choice.area->width(), choice.area->height());
      }
    }
    if (m_level != nullptr && m_level->parent() != nullptr) {
      // The lighter fill sweeps out of the primary button with the count (mockup section 2).
      const Node* button = m_level->parent();
      m_level->setSize(std::round(button->width() * static_cast<float>(m_secondsLeft) / kCountdownSeconds), button->height());
    }
  }
}

void PowerOverlay::buildScene(float width, float height) {
  const float s = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0F;
  m_root = ui::node({});
  m_root->setSize(width, height);
  m_root->addChild(ui::inputArea({
      .out = &m_backdrop,
      .width = width,
      .height = height,
      .onClick =
          [this](const InputArea::PointerData&) {
            if (m_mode == Mode::Choices) {
              DeferredCall::callLater([this] { hide(); });
            }
          },
  }));
  auto dim = ui::box({});
  dim->setFill(fixedColorSpec(rgba(0.0F, 0.0F, 0.0F, 0.32F)));
  dim->setSize(width, height);
  dim->setHitTestVisible(false);
  m_root->addChild(std::move(dim));
  switch (m_mode) {
  case Mode::Choices:
    buildChoices(*m_root, s);
    break;
  case Mode::Countdown:
    buildCountdown(*m_root, s);
    break;
  case Mode::Blockers:
    buildBlockers(*m_root, s);
    break;
  case Mode::Closing:
    break;
  }
  m_surface->setSceneRoot(m_root.get());
  m_input.setSceneRoot(m_root.get());
}

void PowerOverlay::buildChoices(Node& root, float s) {
  auto content = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP48 * s});
  auto row = ui::row({.align = FlexAlign::Start, .gap = kGap * s});
  for (std::size_t i = 0; i < kChoices.size(); ++i) {
    const ChoiceSpec& spec = kChoices[i];
    Choice& choice = m_choices[i];
    const char* glyph = spec.glyph != nullptr ? spec.glyph : session_action::defaultGlyph(spec.action);
    auto column = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    auto area = ui::inputArea({
        .out = &choice.area,
        .hitShape = InputArea::HitShape::Circle,
        .width = kButton * s,
        .height = kButton * s,
        .onEnter = [this, i](const InputArea::PointerData&) {
          m_focused = i;
          syncFocus();
        },
        .onClick = [this, i](const InputArea::PointerData&) { pick(i); },
    });
    auto circle = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .radius = kButton * 0.5F * s,
                           .width = kButton * s, .height = kButton * s},
                          ui::glyph({.out = &choice.glyph, .glyph = glyph, .glyphSize = kGlyph * s}));
    circle->setHitTestVisible(false);
    choice.circle = circle.get();
    area->addChild(std::move(circle));
    if (i == kShutDown) {
      auto ring = std::make_unique<CountdownRingNode>();
      ring->setSize((kButton + kRing + kRing) * s, (kButton + kRing + kRing) * s);
      ring->setPosition(-kRing * s, -kRing * s);
      ring->setThickness(Tokens::spaceP4 * s);
      ring->setColor(rgba(1.0F, 1.0F, 1.0F, 0.95F));
      ring->setProgress(0.0F);
      ring->setVisible(false);
      m_ring = static_cast<CountdownRingNode*>(area->addChild(std::move(ring)));
    }
    column->addChild(std::move(area));
    column->addChild(ui::label({.out = &choice.label, .text = i18n::tr(spec.label),
                                .fontSize = Tokens::typeHeadline.size * s,
                                .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                                .color = fixedColorSpec(rgba(1.0F, 1.0F, 1.0F, 1.0F)), .maxLines = 1}));
    row->addChild(std::move(column));
  }
  content->addChild(std::move(row));
  content->addChild(ui::label({.out = &m_hint, .text = i18n::tr("control-center.power-menu.hold-hint"),
                               .fontSize = Tokens::typeFootnote.size * s,
                               .color = fixedColorSpec(rgba(1.0F, 1.0F, 1.0F, 0.72F)), .maxLines = 1}));
  m_content = root.addChild(std::move(content));
}

void PowerOverlay::syncFocus() {
  for (std::size_t i = 0; i < m_choices.size(); ++i) {
    Flex* circle = m_choices[i].circle;
    if (circle == nullptr || m_choices[i].glyph == nullptr) {
      continue;
    }
    const bool focused = i == m_focused;
    // Holding the power key leaves only Shut Down (mockup section 1, right).
    const bool shown = !m_holding || i == kShutDown;
    if (Node* column = circle->parent() != nullptr ? circle->parent()->parent() : nullptr; column != nullptr) {
      column->setOpacity(shown ? 1.0F : 0.0F);
    }
    circle->setFill(fixedColorSpec(focused && !m_holding ? rgba(1.0F, 1.0F, 1.0F, 0.94F) : rgba(1.0F, 1.0F, 1.0F, 0.18F)));
    m_choices[i].glyph->setColor(
        fixedColorSpec(focused && !m_holding ? rgba(0.08F, 0.08F, 0.09F, 1.0F) : rgba(1.0F, 1.0F, 1.0F, 1.0F))
    );
  }
  if (m_choices[kShutDown].label != nullptr) {
    m_choices[kShutDown].label->setText(
        i18n::tr(m_holding ? "control-center.power-menu.shutting-down" : "control-center.power-menu.shut-down")
    );
  }
  if (m_hint != nullptr) {
    m_hint->setText(i18n::tr(m_holding ? "control-center.power-menu.hold-cancel" : "control-center.power-menu.hold-hint"));
  }
  if (m_ring != nullptr) {
    m_ring->setVisible(m_holding);
    m_ring->setProgress(m_holdProgress);
  }
}

void PowerOverlay::pick(std::size_t index) {
  if (index >= kChoices.size()) {
    return;
  }
  const std::string action = kChoices[index].action;
  if (action == "reboot" || action == "shutdown") {
    DeferredCall::callLater([this, action] {
      // A block inhibitor (pacman, firmware) would refuse the action after every window closed: wait for it instead.
      if (m_guard != nullptr && !m_guard->blockers().empty()) {
        m_guard->queue(action);
        hide();
        return;
      }
      beginEnd(action);
    });
    return;
  }
  // Deferred: running it here would free the clicked area inside its own click handler.
  DeferredCall::callLater([this, index] { run(kChoices[index].action); });
}

void PowerOverlay::run(const char* action) {
  const std::string line = std::string("session ") + action;
  hide();
  if (m_ipc != nullptr) {
    (void)m_ipc->execute(line);
  }
}

void PowerOverlay::startHold() {
  if (m_holding) {
    return;
  }
  m_holding = true;
  m_focused = kShutDown;
  m_holdProgress = 0.0F;
  m_holdAnim = m_animations.animateTimer(
      0.0F, 1.0F, kHoldMs, Easing::Linear,
      [this](float v) {
        m_holdProgress = v;
        if (m_surface != nullptr) {
          m_surface->requestUpdate();
        }
      },
      [this] {
        m_holdAnim = 0;
        // The screen fades out, then the machine powers off.
        m_animations.animateSpring(
            1.0F, 0.0F,
            desk::motion::SpringConfig{.damping = Tokens::springFade.damping, .stiffness = Tokens::springFade.stiffness},
            [this](float v) {
              if (m_root != nullptr) {
                m_root->setOpacity(v);
              }
            },
            [this] { DeferredCall::callLater([this] { run("shutdown"); }); }
        );
      }
  );
  syncFocus();
  m_surface->requestUpdate();
}

void PowerOverlay::cancelHold() {
  if (!m_holding || m_holdAnim == 0) {
    return;
  }
  m_animations.cancel(m_holdAnim);
  m_holdAnim = 0;
  m_holding = false;
  m_holdProgress = 0.0F;
  syncFocus();
  m_surface->requestUpdate();
}

bool PowerOverlay::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isActive()) {
    return false;
  }
  if (event.sym == XKB_KEY_XF86PowerOff) {
    // The hold-to-shut-down ring belongs to the choices; a session end already runs its own action.
    if (m_mode != Mode::Choices) {
      return true;
    }
    if (event.pressed && !event.repeat) {
      startHold();
    } else if (!event.pressed) {
      cancelHold();
    }
    return true;
  }
  if (!event.pressed || event.preedit || m_holding) {
    return true;
  }
  if (m_mode != Mode::Choices) {
    const bool enter = event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter;
    if (m_mode == Mode::Ending || event.repeat) {
      return true;
    }
    // Cancel is the default in the blocker sheet; the countdown's default is the action (mockup sections 2, 3).
    if (event.sym == XKB_KEY_Escape || (m_mode == Mode::Blockers && enter)) {
      hide();
    } else if (m_mode == Mode::Countdown && enter) {
      finishEnd();
    }
    return true;
  }
  switch (event.sym) {
  case XKB_KEY_Escape:
    hide();
    break;
  case XKB_KEY_Left:
  case XKB_KEY_Up:
    m_focused = (m_focused + kChoices.size() - 1) % kChoices.size();
    break;
  case XKB_KEY_Right:
  case XKB_KEY_Down:
  case XKB_KEY_Tab:
    m_focused = (m_focused + 1) % kChoices.size();
    break;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
  case XKB_KEY_space:
    pick(m_focused);
    return true;
  default:
    return true;
  }
  if (m_surface != nullptr) {
    syncFocus();
    m_surface->requestUpdate();
  }
  return true;
}

bool PowerOverlay::onPointerEvent(const PointerEvent& event) {
  if (!isActive()) {
    return false;
  }
  const bool onTarget = event.surface != nullptr && event.surface == m_surface->wlSurface();
  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onTarget) {
      m_pointerInside = true;
      m_input.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    return onTarget;
  case PointerEvent::Type::Leave:
    if (onTarget || m_pointerInside) {
      m_pointerInside = false;
      m_countPaused = false;
      m_input.pointerLeave();
    }
    return onTarget;
  case PointerEvent::Type::Motion:
    if (onTarget) {
      m_input.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
      // The count pauses while the pointer is on the alert.
      if (m_mode == Mode::Countdown && m_content != nullptr) {
        const auto x = static_cast<float>(event.sx);
        const auto y = static_cast<float>(event.sy);
        m_countPaused = x >= m_content->x() && y >= m_content->y() && x < m_content->x() + m_content->width()
            && y < m_content->y() + m_content->height();
      }
    }
    return onTarget;
  case PointerEvent::Type::Button:
    if (m_mode == Mode::Ending) {
      return true;
    }
    if (onTarget) {
      return m_input.pointerButton(static_cast<float>(event.sx), static_cast<float>(event.sy), event.button,
                                   event.pressed, event.serial, event.time, event.touch);
    }
    return false;
  case PointerEvent::Type::Axis:
    return onTarget;
  }
  return false;
}

void PowerOverlay::setMode(Mode mode) {
  m_mode = mode;
  if (m_holdAnim != 0) {
    m_animations.cancel(m_holdAnim);
    m_holdAnim = 0;
  }
  m_holding = false;
  m_input.setSceneRoot(nullptr);
  if (m_surface != nullptr) {
    m_surface->setSceneRoot(nullptr);
  }
  m_root.reset();
  m_content = nullptr;
  m_hint = nullptr;
  m_ring = nullptr;
  m_choices = {};
  m_countLabel = nullptr;
  m_level = nullptr;
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

std::vector<ToplevelInfo> PowerOverlay::openWindows() const {
  std::vector<ToplevelInfo> windows;
  if (m_platform == nullptr) {
    return windows;
  }
  for (const std::string& appId : m_platform->runningAppIds()) {
    for (ToplevelInfo& window : m_platform->windowsForApp(appId, {})) {
      windows.push_back(std::move(window));
    }
  }
  for (ToplevelInfo& window : m_platform->windowsWithoutAppId()) {
    windows.push_back(std::move(window));
  }
  return windows;
}

void PowerOverlay::beginEnd(const std::string& action) {
  watchGuard(true);
  m_endAction = action;
  // Taken before the windows close, so the next login can open the same apps again.
  m_reopenApps = m_platform != nullptr ? m_platform->runningAppIds() : std::vector<std::string>{};
  m_blockers.clear();
  m_tickTimer.stop();
  // Apps that watch the session (Inhibit portal monitors) save now; ones that must not end yet hold a logout inhibitor.
  setPortalState(static_cast<int>(InhibitPortal::SessionState::QueryEnd));
  const std::vector<ToplevelInfo> windows = openWindows();
  m_appBlockers = appBlockers();
  if (windows.empty() && m_appBlockers.empty()) {
    startCountdown();
    return;
  }
  // Nothing covers the windows while they close, so a "Save changes?" dialog they raise takes input.
  hideSurface();
  m_mode = Mode::Closing;
  for (const ToplevelInfo& window : windows) {
    m_platform->closeToplevelInfo(window);
  }
  m_closeTimer.start(kCloseWait, [this] {
    if (m_mode != Mode::Closing) {
      return;
    }
    m_blockers = openWindows();
    m_appBlockers = appBlockers();
    if (m_blockers.empty() && m_appBlockers.empty()) {
      finishEnd();
      return;
    }
    m_mode = Mode::Blockers;
    readUnresponsive();
    restoreSheet();
  });
}

void PowerOverlay::startCountdown() {
  if (!isActive()) {
    show(nullptr);
  }
  if (!isActive()) {
    run(m_endAction.c_str());
    return;
  }
  setMode(Mode::Countdown);
  m_secondsLeft = kCountdownSeconds;
  m_countPaused = false;
  m_tickTimer.startRepeating(std::chrono::seconds(1), [this] { tickCountdown(); });
}

void PowerOverlay::onToplevelChange() {
  if (m_mode == Mode::Closing || m_mode == Mode::Blockers) {
    DeferredCall::callLater([this] { checkWindows(); });
  }
}

void PowerOverlay::checkWindows() {
  if (m_mode != Mode::Closing && m_mode != Mode::Blockers) {
    return;
  }
  std::vector<ToplevelInfo> windows = openWindows();
  std::vector<std::pair<std::string, std::string>> apps = appBlockers();
  if (windows.empty() && !apps.empty() && m_mode == Mode::Closing) {
    // The windows are gone but an app still holds the logout: the close wait shows it as a blocker.
    return;
  }
  if (windows.empty() && apps.empty()) {
    m_closeTimer.stop();
    if (m_mode == Mode::Closing) {
      // Every window closed in time: the calm countdown (section 2).
      startCountdown();
    } else {
      // The blocker list emptied: the action goes on by itself (section 3).
      finishEnd();
    }
    return;
  }
  // The list updates live; after Show the sheet comes back once the open windows change.
  const auto ids = [](const std::vector<ToplevelInfo>& list) {
    std::vector<std::string> out;
    for (const ToplevelInfo& window : list) {
      out.push_back(window.identifier);
    }
    return out;
  };
  if (m_mode == Mode::Blockers && (ids(windows) != ids(m_blockers) || apps != m_appBlockers)) {
    m_blockers = std::move(windows);
    m_appBlockers = std::move(apps);
    readUnresponsive();
    if (isActive()) {
      setMode(Mode::Blockers);
    } else {
      restoreSheet();
    }
  }
}

void PowerOverlay::tickCountdown() {
  if (m_mode != Mode::Countdown || m_countPaused) {
    return;
  }
  --m_secondsLeft;
  if (m_secondsLeft <= 0) {
    finishEnd();
    return;
  }
  if (m_countLabel != nullptr) {
    m_countLabel->setText(i18n::tr(endText("body").c_str(), "seconds", std::to_string(m_secondsLeft)));
  }
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

void PowerOverlay::finishEnd() {
  if (m_mode == Mode::Ending) {
    return;
  }
  m_mode = Mode::Ending;
  setPortalState(static_cast<int>(InhibitPortal::SessionState::Ending));
  saveReopen(m_reopenApps);
  const std::string action = m_endAction;
  m_closeTimer.stop();
  m_tickTimer.stop();
  if (m_platform != nullptr) {
    for (const ToplevelInfo& window : m_blockers) {
      if (window.exactIdentity) {
        (void)m_platform->andromedaRuntime().requestAction("window-kill:" + window.identifier);
      }
    }
  }
  DeferredCall::callLater([this, action] { run(action.c_str()); });
}

void PowerOverlay::readUnresponsive() {
  m_unresponsive.clear();
  if (m_platform == nullptr) {
    return;
  }
  if (const auto reply = m_platform->andromedaRuntime().requestCommand("windows");
      reply.has_value() && reply->contains("ok") && reply->at("ok").is_array()) {
    for (const auto& entry : reply->at("ok")) {
      if (entry.value("not_responding", false)) {
        m_unresponsive.push_back(entry.value("id", std::string{}));
      }
    }
  }
}

std::string PowerOverlay::endText(const char* kind) const {
  return std::string("control-center.power-menu.end.") + m_endAction + "." + kind;
}

namespace {

  std::unique_ptr<Flex> alertCard(float s, float width) {
    auto card = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                            .padding = Tokens::componentDialogPadding * s,
                            .fill = colorSpecFromRole(ColorRole::Popover), .radius = Tokens::componentDialogRadius * s,
                            .width = width * s});
    card->setBorder(colorSpecFromRole(ColorRole::Stroke), 0.5F * s);
    return card;
  }

  std::unique_ptr<Flex> alertIcon(float s, const char* glyph, ColorRole role) {
    return ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                    .fill = colorSpecFromRole(role, 0.24F), .radius = kIcon * 0.5F * s, .width = kIcon * s,
                    .height = kIcon * s},
                   ui::glyph({.glyph = glyph, .glyphSize = Tokens::componentRoundIconGlyph * s,
                              .color = colorSpecFromRole(role)}));
  }

  std::unique_ptr<Label> alertText(const std::string& text, const Tokens::Type& type, float s, ColorRole role) {
    auto label = ui::label({.text = text, .fontSize = type.size * s,
                            .fontWeight = static_cast<FontWeight>(Style::glassWeight(type.weight)),
                            .color = colorSpecFromRole(role)});
    label->setMaxWidth(0.0F);
    return label;
  }

  std::unique_ptr<Button> alertButton(const std::string& text, ButtonVariant variant, float s, std::function<void()> click) {
    auto button = std::make_unique<Button>();
    button->setText(text);
    button->setVariant(variant);
    button->setControlHeight(Tokens::componentButtonHeightM * s);
    button->setRadius(Tokens::componentButtonHeightM * 0.5F * s);
    button->setFlexGrow(1.0F);
    button->setOnClick(std::move(click));
    return button;
  }

} // namespace

void PowerOverlay::buildCountdown(Node& root, float s) {
  auto card = alertCard(s, Tokens::componentDialogAlertWidth);
  const char* glyph = session_action::defaultGlyph(m_endAction);
  card->addChild(ui::row({.align = FlexAlign::Start}, alertIcon(s, glyph, ColorRole::Accent)));
  card->addChild(alertText(i18n::tr(endText("title").c_str()), Tokens::typeHeadline, s, ColorRole::Label1));
  auto body = alertText(i18n::tr(endText("body").c_str(), "seconds", std::to_string(m_secondsLeft)), Tokens::typeFootnote,
                        s, ColorRole::Label2);
  m_countLabel = body.get();
  card->addChild(std::move(body));
  if (m_guard != nullptr && !m_guard->otherUsers().empty()) {
    std::string users;
    for (const std::string& user : m_guard->otherUsers()) {
      users += (users.empty() ? "" : ", ") + user;
    }
    card->addChild(alertText(i18n::tr("control-center.power-menu.other-users", "users", users), Tokens::typeFootnote, s,
                             ColorRole::Label2));
  }
  // DESIGN.md: a checkbox row, not a toggle card.
  card->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s},
      ui::checkbox({.checked = m_reopen, .scale = s, .onChange = [this](bool on) {
                      m_reopen = on;
                      // Only the choice: the app list is written once the session really ends.
                      saveReopen({});
                    }}),
      ui::label({.text = i18n::tr("control-center.power-menu.reopen"), .fontSize = Tokens::typeFootnote.size * s,
                 .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2, .flexGrow = 1.0F})
  ));
  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  buttons->addChild(alertButton(i18n::tr("control-center.power-menu.cancel"), ButtonVariant::Default, s,
                                [this] { DeferredCall::callLater([this] { hide(); }); }));
  auto primary = alertButton(i18n::tr(endText("button").c_str()), ButtonVariant::Primary, s,
                             [this] { DeferredCall::callLater([this] { finishEnd(); }); });
  auto sweep = ui::box({});
  sweep->setFill(fixedColorSpec(rgba(1.0F, 1.0F, 1.0F, 0.18F)));
  sweep->setRadius(Tokens::componentButtonHeightM * 0.5F * s);
  sweep->setParticipatesInLayout(false);
  sweep->setHitTestVisible(false);
  m_level = primary->addChild(std::move(sweep));
  buttons->addChild(std::move(primary));
  card->addChild(std::move(buttons));
  m_content = root.addChild(std::move(card));
}

void PowerOverlay::buildBlockers(Node& root, float s) {
  auto card = alertCard(s, Tokens::componentDialogWidth);
  card->addChild(ui::row({.align = FlexAlign::Start}, alertIcon(s, session_action::defaultGlyph(m_endAction), ColorRole::Accent)));
  // An app's logout inhibitor gives its window's reason; an app without a window gets a row of its own.
  const auto reasonFor = [this](const std::string& appId) -> const std::string* {
    const auto it = std::ranges::find_if(m_appBlockers, [&](const auto& entry) { return entry.first == appId; });
    return it == m_appBlockers.end() || it->second.empty() ? nullptr : &it->second;
  };
  // The compositor pings each window it asks to close; one that missed the ping says so (D3 blocker reasons).
  const std::vector<std::string>& unresponsive = m_unresponsive;
  std::vector<std::pair<std::string, std::string>> windowless;
  for (const auto& entry : m_appBlockers) {
    if (std::ranges::none_of(m_blockers, [&](const ToplevelInfo& window) { return window.appId == entry.first; })) {
      windowless.push_back(entry);
    }
  }
  card->addChild(alertText(i18n::tr(endText("blockers").c_str(), "count",
                                    std::to_string(m_blockers.size() + windowless.size())),
                           Tokens::typeHeadline, s, ColorRole::Label1));
  card->addChild(alertText(i18n::tr(endText("blockers-body").c_str()), Tokens::typeFootnote, s, ColorRole::Label2));
  auto list = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s, .padding = Tokens::spaceP8 * s,
                          .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::componentTileRadius * s});
  for (const ToplevelInfo& window : m_blockers) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP4 * s,
                        .paddingH = Tokens::spaceP4 * s});
    auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    text->addChild(ui::label({.text = window.title.empty() ? window.appId : window.title,
                              .fontSize = Tokens::typeHeadline.size * s,
                              .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
    const std::string* reason = reasonFor(window.appId);
    const bool hung = window.exactIdentity && std::ranges::contains(unresponsive, window.identifier);
    text->addChild(ui::label({.text = reason != nullptr ? *reason
                                  : hung            ? i18n::tr("control-center.power-menu.not-responding")
                                                    : i18n::tr("control-center.power-menu.still-open"),
                              .fontSize = Tokens::typeFootnote.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                              .maxLines = 1}));
    row->addChild(std::move(text));
    const ToplevelInfo copy = window;
    auto show = alertButton(i18n::tr("control-center.power-menu.show"), ButtonVariant::Default, s, [this, copy] {
      // Show drops the sheet but keeps the session end: closing that window lets the action go on.
      DeferredCall::callLater([this, copy] {
        hideSurface();
        if (m_platform != nullptr) {
          m_platform->activateToplevelInfo(copy);
        }
      });
    });
    show->setFlexGrow(0.0F);
    row->addChild(std::move(show));
    if (window.exactIdentity && m_platform != nullptr) {
      auto quit = alertButton(i18n::tr("control-center.power-menu.force-quit"), ButtonVariant::Default, s, [this, copy] {
        (void)m_platform->andromedaRuntime().requestAction("window-kill:" + copy.identifier);
      });
      quit->setFlexGrow(0.0F);
      row->addChild(std::move(quit));
    }
    list->addChild(std::move(row));
  }
  for (const auto& [appId, reason] : windowless) {
    list->addChild(ui::row(
        {.align = FlexAlign::Center, .paddingV = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP4 * s},
        ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F},
                   ui::label({.text = appId.empty() ? i18n::tr("control-center.power-menu.an-app") : appId,
                              .fontSize = Tokens::typeHeadline.size * s,
                              .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}),
                   ui::label({.text = reason.empty() ? i18n::tr("control-center.power-menu.busy") : reason,
                              .fontSize = Tokens::typeFootnote.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                              .maxLines = 2}))
    ));
  }
  card->addChild(std::move(list));
  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  buttons->addChild(alertButton(i18n::tr("control-center.power-menu.cancel"), ButtonVariant::Default, s,
                                [this] { DeferredCall::callLater([this] { hide(); }); }));
  buttons->addChild(alertButton(i18n::tr(endText("anyway").c_str()), ButtonVariant::Destructive, s,
                                [this] { DeferredCall::callLater([this] { finishEnd(); }); }));
  card->addChild(std::move(buttons));
  m_content = root.addChild(std::move(card));
}

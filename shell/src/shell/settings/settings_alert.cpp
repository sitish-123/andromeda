#include "shell/settings/settings_alert.h"

#include "core/deferred_call.h"
#include "design_tokens.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace settings {

  namespace {

    struct AlertState {
      SettingsAlertRequest request;
      std::optional<SettingsModalHost::ModalId> id;
      Node* root = nullptr;
      Button* defaultButton = nullptr;
    };

    std::unique_ptr<Node> alertButton(SettingsModalHost& host, const std::shared_ptr<AlertState>& state, std::size_t index) {
      const SettingsAlertButton& spec = state->request.buttons[index];
      const float scale = state->request.scale;
      const float height = Tokens::sizeControlM * scale;
      const bool capsule = spec.variant == ButtonVariant::Primary || spec.variant == ButtonVariant::Destructive;
      return ui::button({
          .out = index == state->request.defaultIndex ? &state->defaultButton : nullptr,
          .text = spec.text,
          .fontSize = Style::fontSizeBody * scale,
          .variant = spec.variant,
          .minHeight = height,
          .paddingV = 0.0F,
          .paddingH = Style::spaceMd * scale,
          .radius = capsule ? height * 0.5F : Style::scaledRadiusMd(scale),
          .flexGrow = 1.0F,
          .onClick =
              [&host, weak = std::weak_ptr<AlertState>(state), index] {
                // Deferred: popping destroys this button while its click is still dispatching.
                DeferredCall::callLater([&host, weak, index] {
                  const auto alive = weak.lock();
                  if (alive == nullptr || !alive->id.has_value()) {
                    return;
                  }
                  auto action = alive->request.buttons[index].onClick;
                  (void)host.pop(*alive->id);
                  if (action) {
                    action();
                  }
                });
              },
      });
    }

    std::unique_ptr<Node> build(SettingsModalHost& host, const std::shared_ptr<AlertState>& state) {
      const SettingsAlertRequest& request = state->request;
      const float scale = request.scale;
      state->defaultButton = nullptr;

      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
          ui::label({
              .text = request.title,
              .fontSize = Tokens::typeTitle3.size * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 3,
          })
      );
      if (!request.body.empty()) {
        text->addChild(ui::label({
            .text = request.body,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
            .maxLines = 8,
        }));
      }

      // Two buttons sit side by side at equal widths; three or more stack at full width.
      auto buttons = request.buttons.size() <= 2
          ? ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true})
          : ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
      for (std::size_t i = 0; i < request.buttons.size(); ++i) {
        buttons->addChild(alertButton(host, state, i));
      }

      auto root = ui::column(
          {.align = FlexAlign::Stretch, .gap = Tokens::componentDialogPadding * scale}, std::move(text), std::move(buttons)
      );
      state->root = root.get();
      return root;
    }

  } // namespace

  void showSettingsAlert(SettingsModalHost& host, SettingsAlertRequest request) {
    auto state = std::make_shared<AlertState>();
    state->request = std::move(request);
    state->request.defaultIndex = std::min(state->request.defaultIndex, state->request.buttons.size() - 1);
    const float scale = state->request.scale;
    state->id = host.push(
        SettingsModalRequest{
            .build = [&host, state] { return build(host, state); },
            .measure =
                [state, scale](Renderer& renderer, const SettingsModalLayoutSpace& space) {
                  const float width = std::min(
                      (Tokens::componentDialogAlertWidth - 2.0F * Tokens::componentDialogPadding) * scale,
                      space.maxContentWidth
                  );
                  if (state->root == nullptr) {
                    return LayoutSize{.width = width, .height = 1.0F};
                  }
                  state->root->setSize(width, 0.0F);
                  LayoutConstraints constraints;
                  constraints.setExactWidth(width);
                  return LayoutSize{.width = width, .height = state->root->measure(renderer, constraints).height};
                },
            .initialFocusArea = [state] {
              return state->defaultButton != nullptr ? state->defaultButton->inputArea() : nullptr;
            },
            .requestClose =
                [&host, weak = std::weak_ptr<AlertState>(state)] {
                  if (const auto alive = weak.lock(); alive != nullptr && alive->id.has_value()) {
                    (void)host.pop(*alive->id);
                  }
                },
            .onClosed = [state] { state->root = nullptr; },
            .contentPadding = Tokens::componentDialogPadding * scale,
            .windowMargin = Tokens::componentDialogPadding * scale,
            .scale = scale,
        }
    );
  }

} // namespace settings

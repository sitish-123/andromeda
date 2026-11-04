#include "shell/settings/settings_content_sound.h"

#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <format>

namespace settings {
  namespace {

    using namespace page_rows;

    void addDeviceCard(Flex& content, bool output, const SettingsSoundContext& ctx) {
      const float scale = ctx.scale;
      PipeWireService* audio = ctx.audio;
      const AudioState& state = audio->state();
      const auto& nodes = output ? state.sinks : state.sources;
      const std::uint32_t current = output ? state.defaultSinkId : state.defaultSourceId;

      addHeader(content, i18n::tr(output ? "settings.sound.output" : "settings.sound.input"), nullptr, scale);
      Flex* card = addCard(content, scale);
      if (nodes.empty()) {
        auto r = row(scale);
        r->addChild(ui::label({.text = i18n::tr("settings.sound.no-devices"), .fontSize = Style::fontSizeBody * scale,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
        card->addChild(std::move(r));
        return;
      }
      for (const auto& node : nodes) {
        auto r = row(scale);
        r->addChild(symbol(output ? "volume" : "microphone", Tokens::sizeSymbol * scale, ColorRole::Label2));
        r->addChild(title(audioDeviceLabel(node), scale));
        if (node.id == current) {
          r->addChild(symbol("check", Tokens::sizeSymbol * scale, ColorRole::AccentText));
        } else {
          r->addChild(plainButton(i18n::tr("settings.sound.use"), ButtonVariant::Ghost, scale, [audio, output, id = node.id] {
            if (output) {
              audio->setDefaultSink(id);
            } else {
              audio->setDefaultSource(id);
            }
          }));
        }
        card->addChild(std::move(r));
      }

      const AudioNode* node = output ? audio->defaultSink() : audio->defaultSource();
      if (node == nullptr) {
        return;
      }
      auto volume = row(scale);
      volume->addChild(ui::label({.text = i18n::tr(output ? "settings.sound.output-volume" : "settings.sound.input-volume"),
                                  .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::Label1),
                                  .flexGrow = 1.0F}));
      volume->addChild(ui::slider({
          .out = output ? ctx.outputSlider : ctx.inputSlider,
          .minValue = 0.0,
          .maxValue = 1.0,
          .step = 0.01,
          .value = node->volume,
          .width = 220.0F * scale,
          .onValueChanged = [audio, output, id = node->id](double v) {
            if (output) {
              audio->setSinkVolume(id, static_cast<float>(v));
            } else {
              audio->setSourceVolume(id, static_cast<float>(v));
            }
          },
      }));
      card->addChild(std::move(volume));
      auto mute = row(scale);
      mute->addChild(title(i18n::tr("settings.sound.mute"), scale));
      mute->addChild(ui::toggle({
          .checked = node->muted,
          .scale = scale,
          .onChange = [audio, output, id = node->id](bool on) {
            if (output) {
              audio->setSinkMuted(id, on);
            } else {
              audio->setSourceMuted(id, on);
            }
          },
      }));
      card->addChild(std::move(mute));
    }

    // One row per playing program: name, its own volume, and a mute button.
    void addProgramCard(Flex& content, PipeWireService& audio, const SettingsSoundContext& ctx) {
      const float scale = ctx.scale;
      const auto& programs = audio.state().programOutputs;
      if (programs.empty()) {
        return;
      }
      addHeader(content, i18n::tr("settings.sound.apps"), nullptr, scale);
      Flex* card = addCard(content, scale);
      for (const auto& program : programs) {
        std::string name = !program.applicationName.empty() ? program.applicationName
            : !program.streamTitle.empty()                  ? program.streamTitle
                                                             : program.description;
        auto r = row(scale);
        r->addChild(title(std::move(name), scale));
        Slider* slider = nullptr;
        r->addChild(ui::slider({
            .out = &slider,
            .minValue = 0.0,
            .maxValue = 1.0,
            .step = 0.01,
            .value = program.volume,
            .width = 180.0F * scale,
            .onValueChanged = [&audio, id = program.id](double v) {
              audio.setProgramOutputVolume(id, static_cast<float>(v));
            },
        }));
        r->addChild(iconButton(program.muted ? "volume-off" : "volume",
                               i18n::tr("settings.sound.mute"), scale,
                               [&audio, id = program.id, muted = program.muted] {
                                 audio.setProgramOutputMuted(id, !muted);
                               }));
        if (ctx.programSliders != nullptr) {
          ctx.programSliders->emplace_back(program.id, slider);
        }
        card->addChild(std::move(r));
      }
    }

  } // namespace

  void addSettingsSound(Flex& content, const SettingsSoundContext& ctx) {
    if (ctx.audio == nullptr) {
      addEmptyState(content, "volume-off", i18n::tr("settings.sound.unavailable"), ctx.scale);
      return;
    }
    addDeviceCard(content, true, ctx);
    addDeviceCard(content, false, ctx);
    addProgramCard(content, *ctx.audio, ctx);
  }

  std::string soundDeviceSignature(const PipeWireService* audio) {
    if (audio == nullptr) {
      return {};
    }
    const AudioState& state = audio->state();
    std::string out = std::format("{}/{}:", state.defaultSinkId, state.defaultSourceId);
    for (const auto& node : state.sinks) {
      out += std::format("{}{},", node.id, node.muted ? "m" : "");
    }
    out += '|';
    for (const auto& node : state.sources) {
      out += std::format("{}{},", node.id, node.muted ? "m" : "");
    }
    out += '|';
    for (const auto& node : state.programOutputs) {
      out += std::format("{}{},", node.id, node.muted ? "m" : "");
    }
    return out;
  }

} // namespace settings

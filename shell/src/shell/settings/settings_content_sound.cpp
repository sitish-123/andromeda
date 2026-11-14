#include "shell/settings/settings_content_sound.h"

#include "i18n/i18n.h"
#include "pipewire/audio_route_selection.h"
#include "pipewire/pipewire_service.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <format>
#include <memory>

namespace settings {
  namespace {
    constexpr float kDeviceSliderWidth = 220.0F;
    constexpr float kAppSliderWidth = 180.0F;

    using namespace page_rows;

    void addDeviceCard(Flex& content, bool output, const SettingsSoundContext& ctx) {
      const float scale = ctx.scale;
      PipeWireService* audio = ctx.audio;
      const AudioState& state = audio->state();
      std::vector<SinkChoice> nodes;
      if (output) {
        nodes = sinkChoices(state);
      } else {
        for (const AudioNode& n : state.sources) {
          if (n.available || n.id == state.defaultSourceId) {
            nodes.push_back({.node = &n, .pickId = n.id, .current = n.id == state.defaultSourceId});
          }
        }
      }

      addHeader(content, i18n::tr(output ? "settings.sound.output" : "settings.sound.input"), nullptr, scale);
      Flex* card = addCard(content, scale);
      if (nodes.empty()) {
        auto r = row(scale);
        r->addChild(ui::label({.text = i18n::tr("settings.sound.no-devices"), .fontSize = Style::fontSizeBody * scale,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
        card->addChild(std::move(r));
        return;
      }
      for (const SinkChoice& choice : nodes) {
        auto r = row(scale);
        r->addChild(symbol(output ? "volume" : "microphone", Tokens::sizeSymbol * scale, ColorRole::Label2));
        r->addChild(title(audioDeviceLabel(*choice.node), scale));
        if (choice.current) {
          r->addChild(symbol("check", Tokens::sizeSymbol * scale, ColorRole::AccentText));
        } else {
          r->addChild(plainButton(i18n::tr("settings.sound.use"), ButtonVariant::Ghost, scale, [audio, output, id = choice.pickId] {
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
          .width = kDeviceSliderWidth * scale,
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
            .width = kAppSliderWidth * scale,
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


    std::vector<std::string> applyArgs(std::string_view profile, double loudness, double bass) {
      return {"apply", "--profile", std::string(profile), "--loudness", std::format("{}", loudness), "--bass",
              std::format("{}", bass)};
    }

    std::string styleLabel(std::string_view profile) {
      const std::string key = std::format("settings.sound.style-{}", profile);
      return i18n::tr(key);
    }

    // Speaker Sound card (.agent/DESIGN.md, approved A): switch with the tuning source, style, folded fine tune,
    // and the Windows import row when only our basic tuning exists.
    void addSpeakerCard(Flex& content, const SettingsSoundContext& ctx) {
      const SpeakerStatus* st = ctx.speakerStatus;
      if (st == nullptr || !st->speakers || ctx.speakerPhase == SpeakerPhase::Unavailable) {
        return;
      }
      const float scale = ctx.scale;
      const bool busy = ctx.speakerPhase == SpeakerPhase::Applying || ctx.speakerPhase == SpeakerPhase::Importing ||
                        ctx.speakerPhase == SpeakerPhase::Loading;
      const auto run = ctx.runSpeaker;
      const SpeakerStatus s = *st;

      addHeader(content, i18n::tr("settings.sound.speaker-sound"), nullptr, scale);
      Flex* card = addCard(content, scale);
      std::string source = s.basic() ? i18n::tr("settings.sound.basic-tuning")
                                     : i18n::tr("settings.sound.tuned-from-windows", "vendor", s.source);
      if (ctx.speakerPhase == SpeakerPhase::ApplyFailed) {
        source = ctx.speakerError;
      } else if (!s.enabled) {
        source = i18n::tr("settings.sound.off-with", "source", source);
      }
      Flex* enhance = addTwoLineRow(*card, "", i18n::tr("settings.sound.enhance-speakers"), source, scale);
      enhance->addChild(ui::toggle({
          .checked = s.enabled,
          .enabled = !busy,
          .scale = scale,
          .onChange = [run, s](bool on) {
            run(on ? applyArgs(s.basic() ? "generic" : s.profile, s.loudnessDb, s.bass) : std::vector<std::string>{"off"});
          },
      }));

      if (!s.basic() && !s.profiles.empty()) {
        std::vector<std::string> names;
        std::size_t current = 0;
        for (std::size_t i = 0; i < s.profiles.size(); ++i) {
          names.push_back(styleLabel(s.profiles[i]));
          if (s.profiles[i] == s.profile) {
            current = i;
          }
        }
        auto r = row(scale);
        r->addChild(title(i18n::tr("settings.sound.style"), scale));
        r->addChild(ui::select({
            .options = std::move(names),
            .selectedIndex = current,
            .fontSize = Style::fontSizeBody * scale,
            .enabled = !busy,
            .onSelectionChanged = [run, s](std::size_t i, std::string_view) {
              if (i < s.profiles.size() && s.profiles[i] != s.profile) {
                run(applyArgs(s.profiles[i], s.loudnessDb, s.bass));
              }
            },
        }));
        card->addChild(std::move(r));
      }

      auto fine = makeSettingsLinkRow(
          i18n::tr("settings.sound.fine-tune"),
          i18n::tr("settings.sound.fine-tune-value", "bass", std::format("{:g}", s.bass), "loudness",
                   std::format("{:g}", s.loudnessDb)),
          scale, ctx.toggleSpeakerFineTune, ctx.speakerFineTuneOpen ? "chevron-up" : "chevron-down"
      );
      card->addChild(std::move(fine));
      if (ctx.speakerFineTuneOpen) {
        // Apply on release: each apply restarts the filter chain.
        auto slide = [&](const std::string& label, double value, double max, bool isBass) {
          auto r = row(scale);
          r->addChild(ui::row({.width = Style::spaceLg * scale}));
          r->addChild(title(label, scale));
          auto latest = std::make_shared<double>(value);
          r->addChild(ui::slider({
              .minValue = 0.0,
              .maxValue = max,
              .step = 1.0,
              .value = value,
              .enabled = !busy,
              .width = kDeviceSliderWidth * scale,
              .onValueChanged = [latest](double v) { *latest = v; },
              .onDragEnd = [run, s, latest, isBass] {
                run(applyArgs(s.basic() ? "generic" : s.profile, isBass ? s.loudnessDb : *latest,
                              isBass ? *latest : s.bass));
              },
          }));
          card->addChild(std::move(r));
        };
        slide(i18n::tr("settings.sound.bass"), s.bass, 10.0, true);
        slide(i18n::tr("settings.sound.loudness"), s.loudnessDb, 12.0, false);
      }

      if (s.basic() && s.windows) {
        const bool importing = ctx.speakerPhase == SpeakerPhase::Importing;
        const bool failed = ctx.speakerPhase == SpeakerPhase::ImportFailed;
        Flex* imp = addTwoLineRow(
            *card, "", i18n::tr("settings.sound.windows-tuning"),
            importing ? i18n::tr("settings.sound.windows-reading")
                      : (failed ? ctx.speakerError : i18n::tr("settings.sound.windows-found")),
            scale
        );
        if (importing) {
          imp->addChild(ui::spinner({.spinnerSize = Tokens::sizeSymbolInline * scale, .spinning = true}));
        } else {
          imp->addChild(plainButton(i18n::tr(failed ? "settings.sound.try-again" : "settings.sound.use-tuning"),
                                    ButtonVariant::Default, scale, [run] { run({"import-windows"}); }));
        }
      }
      addSettingsRowSeparators(*card, scale);
      addFootnote(*card, i18n::tr("settings.sound.speaker-note"), scale);
    }
  } // namespace

  void addSettingsSound(Flex& content, const SettingsSoundContext& ctx) {
    if (ctx.audio == nullptr) {
      addEmptyState(content, "volume-off", i18n::tr("settings.sound.unavailable"), ctx.scale);
      return;
    }
    addDeviceCard(content, true, ctx);
    addSpeakerCard(content, ctx);
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

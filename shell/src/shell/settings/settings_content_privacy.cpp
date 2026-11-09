#include "shell/settings/settings_content_privacy.h"

#include "auth/biometric_enrollment.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <string>

namespace settings {
  namespace {
    using namespace page_rows;

    constexpr float kFieldWidth = 240.0F;

    std::unique_ptr<Node> valueLabel(std::string text, float scale) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale,
                        .color = colorSpecFromRole(ColorRole::Label2)});
    }

    void addLinkRow(Flex& card, std::string_view glyph, std::string label, std::string value, std::string detail,
                    bool enabled, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->addChild(symbol(glyph, Style::fontSizeBody * scale, ColorRole::Label2));
      r->addChild(title(label, scale));
      r->addChild(valueLabel(std::move(value), scale));
      if (enabled) {
        r->addChild(iconButton("chevron-right", std::move(label), scale,
                               [open = ctx.openDetail, detail = std::move(detail)] { open(detail); }));
      }
      card.addChild(std::move(r));
    }

    void addToggleRow(Flex& card, std::string label, bool checked, std::vector<std::string> path,
                      const SettingsPrivacyContext& ctx) {
      auto r = row(ctx.scale);
      r->addChild(title(std::move(label), ctx.scale));
      r->addChild(ui::toggle({
          .checked = checked,
          .scale = ctx.scale,
          .onChange = [set = ctx.setBool, path = std::move(path)](bool on) { set(path, on); },
      }));
      card.addChild(std::move(r));
    }

    void addStatus(Flex& content, const SettingsPrivacyContext& ctx) {
      if (ctx.status.empty()) {
        return;
      }
      auto r = row(ctx.scale);
      r->addChild(ui::label({.text = ctx.status, .fontSize = Style::fontSizeCaption * ctx.scale,
                             .color = colorSpecFromRole(ColorRole::DangerText)}));
      content.addChild(std::move(r));
    }

    // The running enrollment: prompt, progress and Cancel, in place of the list.
    void addEnrollCard(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      r->addChild(title(ctx.enrollPrompt, scale));
      r->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Default, scale, ctx.cancelEnroll));
      card->addChild(std::move(r));
      auto bar = row(scale);
      bar->addChild(ui::progressBar({
          .fill = colorSpecFromRole(ColorRole::Accent),
          .track = colorSpecFromRole(ColorRole::Fill2),
          .progress = ctx.enrollProgress,
          .height = Style::sliderTrackHeight * scale,
          .flexGrow = 1.0F,
      }));
      card->addChild(std::move(bar));
    }

    void addFacePage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      if (!ctx.facePresent) {
        addEmptyState(content, "face-id-error", i18n::tr("settings.privacy.face-unavailable"), scale);
        return;
      }
      addHeader(content, i18n::tr("settings.privacy.faces"), nullptr, scale);
      if (ctx.enrolling == 1) {
        addEnrollCard(content, ctx);
      } else {
        Flex* card = addCard(content, scale);
        for (const auto& face : ctx.faces) {
          auto r = row(scale);
          r->setGap(Style::spaceMd * scale);
          r->addChild(symbol("face-id", Style::fontSizeBody * scale, ColorRole::Label2));
          r->addChild(title(face, scale));
          r->addChild(plainButton(i18n::tr("settings.privacy.rename"), ButtonVariant::Default, scale,
                                  [open = ctx.openDetail, face] { open("privacy:" + face + "\trename"); }));
          r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                  [remove = ctx.removeFace, face] { remove(face); }));
          card->addChild(std::move(r));
        }
        auto add = row(scale);
        add->addChild(plainButton(i18n::tr("settings.privacy.add-face"), ButtonVariant::Default, scale, ctx.addFace));
        card->addChild(std::move(add));
      }
      addStatus(content, ctx);

      addHeader(content, i18n::tr("settings.privacy.face-use-for"), nullptr, scale);
      Flex* use = addCard(content, scale);
      addToggleRow(*use, i18n::tr("settings.privacy.use-unlock"), ctx.faceUnlock, {"lockscreen", "face"}, ctx);
      if (ctx.faceAdminKnown) {
        auto r = row(scale);
        r->addChild(title(i18n::tr("settings.privacy.use-admin"), scale));
        r->addChild(ui::toggle({.checked = ctx.faceAdmin, .scale = scale, .onChange = ctx.setFaceAdmin}));
        use->addChild(std::move(r));
      }
      addToggleRow(*use, i18n::tr("settings.schema.lockscreen.face-on-request.label"), ctx.faceOnRequest,
                   {"lockscreen", "face_on_request"}, ctx);
    }

    void addFingerPage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      if (!ctx.fingerPresent) {
        addEmptyState(content, "fingerprint-off",
                      i18n::tr(ctx.fprintdMissing ? "settings.privacy.finger-no-fprintd"
                                                  : "settings.privacy.finger-unavailable"),
                      scale);
        return;
      }
      addHeader(content, i18n::tr("settings.privacy.fingers"), nullptr, scale);
      if (ctx.enrolling == 2) {
        addEnrollCard(content, ctx);
      } else {
        Flex* card = addCard(content, scale);
        for (const auto& finger : ctx.fingers) {
          auto r = row(scale);
          r->setGap(Style::spaceMd * scale);
          r->addChild(symbol("fingerprint", Style::fontSizeBody * scale, ColorRole::Label2));
          r->addChild(title(BiometricEnrollment::fingerLabel(finger), scale));
          r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                  [remove = ctx.removeFinger, finger] { remove(finger); }));
          card->addChild(std::move(r));
        }
        auto add = row(scale);
        add->addChild(
            plainButton(i18n::tr("settings.privacy.add-finger"), ButtonVariant::Default, scale, ctx.addFinger)
        );
        card->addChild(std::move(add));
      }
      addStatus(content, ctx);

      addHeader(content, i18n::tr("settings.privacy.finger-use-for"), nullptr, scale);
      Flex* use = addCard(content, scale);
      addToggleRow(*use, i18n::tr("settings.privacy.use-unlock"), ctx.fingerprintUnlock, {"lockscreen", "fingerprint"},
                   ctx);
      if (ctx.fingerAdminKnown) {
        auto r = row(scale);
        r->addChild(title(i18n::tr("settings.privacy.use-admin"), scale));
        r->addChild(ui::toggle({.checked = ctx.fingerAdmin, .scale = scale, .onChange = ctx.setFingerAdmin}));
        use->addChild(std::move(r));
      }
    }

    void addRenamePage(Flex& content, const SettingsPrivacyContext& ctx, const std::string& from) {
      const float scale = ctx.scale;
      Flex* card = addCard(content, scale);
      auto draft = std::make_shared<std::string>(from);
      auto r = row(scale);
      r->addChild(title(i18n::tr("settings.privacy.face-name"), scale));
      auto submit = [rename = ctx.renameFace, from, draft] { rename(from, *draft); };
      r->addChild(ui::input({
          .value = from,
          .width = kFieldWidth * scale,
          .onChange = [draft](const std::string& text) { *draft = text; },
          .onSubmit = [submit](const std::string&) { submit(); },
      }));
      card->addChild(std::move(r));
      addStatus(content, ctx);
      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale,
                              .fillWidth = true});
      actions->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Default, scale, ctx.back));
      actions->addChild(plainButton(i18n::tr("settings.privacy.save"), ButtonVariant::Primary, scale, submit));
      content.addChild(std::move(actions));
    }

    std::unique_ptr<Node> secretInput(std::string* value, float scale, std::function<void()> submit) {
      return ui::input({
          .value = *value,
          .passwordMode = true,
          .width = kFieldWidth * scale,
          .onChange = [value](const std::string& text) { *value = text; },
          .onSubmit = [submit = std::move(submit)](const std::string&) { submit(); },
      });
    }

    void addPasswordPage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      PasswordDraft* draft = ctx.password;
      Flex* card = addCard(content, scale);
      const std::pair<const char*, std::string*> fields[] = {
          {"settings.privacy.password-current", &draft->current},
          {"settings.privacy.password-new", &draft->next},
          {"settings.privacy.password-verify", &draft->verify},
      };
      for (const auto& [key, value] : fields) {
        auto r = row(scale);
        r->addChild(title(i18n::tr(key), scale));
        r->addChild(secretInput(value, scale, ctx.changePassword));
        card->addChild(std::move(r));
      }
      if (!draft->status.empty()) {
        auto r = row(scale);
        r->addChild(ui::label({.text = draft->status, .fontSize = Style::fontSizeCaption * scale,
                               .color = colorSpecFromRole(draft->statusError ? ColorRole::DangerText
                                                                             : ColorRole::Label2)}));
        content.addChild(std::move(r));
      }
      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale,
                              .fillWidth = true});
      actions->addChild(plainButton(i18n::tr(draft->busy ? "settings.privacy.password-changing"
                                                         : "settings.privacy.password-change"),
                                    ButtonVariant::Primary, scale, draft->busy ? std::function<void()>([] {})
                                                                               : ctx.changePassword));
      content.addChild(std::move(actions));
    }

    void addAccessPage(Flex& content, const SettingsPrivacyContext& ctx, const AccessKindView& kind) {
      const float scale = ctx.scale;
      if (kind.apps.empty()) {
        addEmptyState(content, kind.glyph, i18n::tr("settings.privacy.access-none"), scale);
        return;
      }
      Flex* card = addCard(content, scale);
      for (const auto& app : kind.apps) {
        auto r = row(scale);
        r->addChild(title(app.label, scale));
        if (kind.tokens) {
          r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                  [set = ctx.setAccess, table = kind.table, id = app.id] { set(table, id, false); }));
        } else {
          r->addChild(ui::toggle({
              .checked = app.allowed,
              .scale = scale,
              .onChange = [set = ctx.setAccess, table = kind.table, id = app.id](bool on) { set(table, id, on); },
          }));
        }
        card->addChild(std::move(r));
      }
      addStatus(content, ctx);
    }

    std::string countText(std::size_t n, std::string_view one, std::string_view many) {
      return n == 1 ? i18n::tr(one) : i18n::tr(many, "count", std::to_string(n));
    }
  } // namespace

  void addSettingsPrivacy(Flex& content, const SettingsPrivacyContext& ctx) {
    const float scale = ctx.scale;
    // "privacy:<title>\t<kind>": the title shows in the page header, the kind picks the page.
    if (const auto tab = ctx.detail.find('\t'); ctx.detail.starts_with("privacy:") && tab != std::string::npos) {
      const std::string kind = ctx.detail.substr(tab + 1);
      if (kind == "face") {
        addFacePage(content, ctx);
      } else if (kind == "finger") {
        addFingerPage(content, ctx);
      } else if (kind == "password" && ctx.password != nullptr) {
        addPasswordPage(content, ctx);
      } else if (kind.starts_with("access:")) {
        const std::string table = kind.substr(7);
        if (const auto it = std::ranges::find(ctx.access, table, &AccessKindView::table); it != ctx.access.end()) {
          addAccessPage(content, ctx, *it);
        }
      } else if (kind == "rename") {
        addRenamePage(content, ctx, ctx.detail.substr(8, tab - 8));
      }
      return;
    }

    addHeader(content, i18n::tr("settings.privacy.sign-in"), nullptr, scale);
    Flex* card = addCard(content, scale);
    const std::string unavailable = i18n::tr("settings.privacy.not-available");
    std::string face = unavailable;
    if (ctx.facePresent) {
      face = ctx.faces.empty() ? i18n::tr("settings.privacy.not-set-up")
                               : countText(ctx.faces.size(), "settings.privacy.faces-one", "settings.privacy.faces-many");
      if (!ctx.faces.empty() && ctx.faceOnRequest) {
        face += " · " + i18n::tr("settings.privacy.on-tap");
      }
    }
    std::string finger = unavailable;
    if (ctx.fingerPresent) {
      finger = ctx.fingers.empty()
                   ? i18n::tr("settings.privacy.not-set-up")
                   : countText(ctx.fingers.size(), "settings.privacy.fingers-one", "settings.privacy.fingers-many");
    }
    if (ctx.password != nullptr) {
      const std::string passwordTitle = i18n::tr("settings.privacy.password");
      addLinkRow(*card, "key", passwordTitle, {}, "privacy:" + passwordTitle + "\tpassword", true, ctx);
    }
    const std::string faceTitle = i18n::tr("settings.privacy.face-unlock");
    const std::string fingerTitle = i18n::tr("settings.privacy.fingerprint");
    addLinkRow(*card, "face-id", faceTitle, std::move(face), "privacy:" + faceTitle + "\tface", ctx.biometricsKnown,
               ctx);
    addLinkRow(*card, "fingerprint", fingerTitle, std::move(finger), "privacy:" + fingerTitle + "\tfinger",
               ctx.biometricsKnown, ctx);

    if (!ctx.access.empty()) {
      addHeader(content, i18n::tr("settings.privacy.app-access"), nullptr, scale);
      Flex* access = addCard(content, scale);
      for (const auto& kind : ctx.access) {
        const auto allowed = std::ranges::count_if(kind.apps, [](const auto& app) { return app.allowed; });
        std::string value = !kind.loaded ? std::string{}
                            : allowed == 0 ? i18n::tr("settings.privacy.access-no-apps")
                                           : countText(static_cast<std::size_t>(allowed), "settings.privacy.apps-one",
                                                       "settings.privacy.apps-many");
        const std::string kindTitle = i18n::tr(kind.titleKey);
        addLinkRow(*access, kind.glyph, kindTitle, std::move(value), "privacy:" + kindTitle + "\taccess:" + kind.table,
                   kind.loaded, ctx);
      }
      auto note = row(scale);
      note->addChild(ui::label({.text = i18n::tr("settings.privacy.microphone-note"),
                                .fontSize = Style::fontSizeCaption * scale,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      content.addChild(std::move(note));
    }
  }

} // namespace settings

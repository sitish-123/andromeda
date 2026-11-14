#pragma once

#include "launcher/launcher_provider.h"

class NotificationManager;

// Launcher commands "Focus <name>", "Focus <name> for 1 hour" and "End Focus" (docs/focus.md §6 F2).
class FocusProvider : public LauncherProvider {
public:
  explicit FocusProvider(NotificationManager* notifications) : m_notifications(notifications) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "focus"; }
  [[nodiscard]] std::string_view id() const override { return "Focus"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "moon"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  [[nodiscard]] std::vector<LauncherResult> results(std::string_view text, bool emptyShowsAll) const;

  NotificationManager* m_notifications;
};

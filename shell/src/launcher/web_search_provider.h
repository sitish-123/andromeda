#pragma once

#include "launcher/launcher_provider.h"

// "Search the web for '…'": the launcher's row when nothing else matches (docs/search.md §1).
class WebSearchProvider : public LauncherProvider {
public:
  [[nodiscard]] std::string_view defaultPrefix() const override { return "web"; }
  [[nodiscard]] std::string_view id() const override { return "Web"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "world"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;
};

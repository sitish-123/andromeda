#pragma once

#include "launcher/launcher_provider.h"

#include <string>
#include <vector>

class ClipboardService;

// Kaomoji and symbols (arrows, keys, math, currency, typography, Greek) from assets/symbols.json.
class SymbolProvider : public LauncherProvider {
public:
  explicit SymbolProvider(ClipboardService* clipboard) : m_clipboard(clipboard) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "sym"; }
  [[nodiscard]] std::string_view id() const override { return "Symbols"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "math-symbols"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  void initialize() override;
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

private:
  struct Symbol {
    std::string text;
    std::string name;
    std::string group;
  };

  std::vector<Symbol> m_symbols;
  ClipboardService* m_clipboard = nullptr;
};

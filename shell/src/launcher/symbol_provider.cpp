#include "launcher/symbol_provider.h"

#include "clipboard/clipboard_service.h"
#include "i18n/i18n.h"
#include "core/files/resource_paths.h"
#include "util/string_utils.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::string_view kIdPrefix = "sym-";
} // namespace

std::string SymbolProvider::displayName() const { return i18n::tr("launcher.providers.symbols.title"); }

void SymbolProvider::initialize() {
  std::ifstream file(paths::assetPath("symbols.json"));
  if (!file.is_open()) {
    return;
  }
  try {
    for (const auto& group : nlohmann::json::parse(file)) {
      const std::string label = i18n::tr("launcher.symbols." + group.value("id", std::string()));
      for (const auto& item : group["items"]) {
        m_symbols.push_back({item.value("text", ""), item.value("name", ""), label});
      }
    }
  } catch (const nlohmann::json::exception&) {
    m_symbols.clear();
  }
}

std::vector<LauncherResult> SymbolProvider::query(std::string_view text) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  std::vector<LauncherResult> results;
  for (const Symbol& symbol : m_symbols) {
    if (!needle.empty() && !StringUtils::toLower(symbol.name).contains(needle) && symbol.text != needle) {
      continue;
    }
    LauncherResult r;
    r.id = std::string(kIdPrefix) + symbol.text;
    // One-character symbols draw in the icon slot; kaomoji are too wide and become the title.
    const bool wide = symbol.text.size() > 4;
    r.badge = wide ? std::string() : symbol.text;
    r.glyphName = wide ? "mood-smile" : std::string();
    r.title = wide ? symbol.text : symbol.name;
    r.subtitle = wide ? symbol.name : std::string();
    r.category = symbol.group;
    r.presentation = wide ? "symbol-wide" : "symbol";
    results.push_back(std::move(r));
  }
  return results;
}

bool SymbolProvider::activate(const LauncherResult& result) {
  if (m_clipboard == nullptr || !result.id.starts_with(kIdPrefix)) {
    return false;
  }
  return m_clipboard->copyText(result.id.substr(kIdPrefix.size()));
}

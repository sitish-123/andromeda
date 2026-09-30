#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class GlyphRegistry {
public:
  static void initialize();
  static char32_t lookup(std::string_view name);
  static void registerGlyph(std::string_view name, char32_t codepoint);
  [[nodiscard]] static std::optional<std::string> fontPath();
};

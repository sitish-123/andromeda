#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;

  [[nodiscard]] constexpr bool operator==(const Color& other) const noexcept {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
  [[nodiscard]] constexpr bool operator!=(const Color& other) const noexcept { return !(*this == other); }
};

constexpr Color rgba(float r, float g, float b, float a) noexcept { return {r, g, b, a}; }
constexpr Color rgb(float r, float g, float b) noexcept { return {r, g, b, 1.0f}; }

constexpr float colorByte(std::uint32_t value) noexcept { return static_cast<float>(value) / 255.0f; }

constexpr Color rgbHex(std::uint32_t value) noexcept {
  return Color{
      .r = colorByte((value >> 16U) & 0xFFU),
      .g = colorByte((value >> 8U) & 0xFFU),
      .b = colorByte(value & 0xFFU),
      .a = 1.0f,
  };
}

constexpr std::uint32_t hexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<std::uint32_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<std::uint32_t>(10 + (c - 'a'));
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<std::uint32_t>(10 + (c - 'A'));
  }
  throw std::invalid_argument("invalid hex digit");
}

constexpr std::uint32_t hexByte(char high, char low) { return (hexDigit(high) << 4U) | hexDigit(low); }

constexpr Color hex(std::string_view value) {
  if (value.empty() || value.front() != '#') {
    throw std::invalid_argument("hex color must start with '#'");
  }
  if (value.size() == 7) {
    return Color{
        .r = colorByte(hexByte(value[1], value[2])),
        .g = colorByte(hexByte(value[3], value[4])),
        .b = colorByte(hexByte(value[5], value[6])),
        .a = 1.0f,
    };
  }
  throw std::invalid_argument("unsupported hex color format");
}

Color lerpColor(const Color& a, const Color& b, float t) noexcept;

bool tryParseHexColor(std::string_view hex, Color& out) noexcept;

Color colorWithAlpha(const Color& color, float alpha) noexcept;

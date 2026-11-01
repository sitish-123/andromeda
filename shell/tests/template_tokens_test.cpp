// GTK and Qt templates take their colors from the design-token scheme of the current mode.

#include "design_tokens.h"
#include "tests/test_check.h"
#include "theme/template_engine.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

  using noctalia::theme::TemplateEngine;

  std::string render(std::string_view text, std::string mode) {
    TemplateEngine::Options options;
    options.defaultMode = std::move(mode);
    const auto result = TemplateEngine({}, options).render(text);
    TEST_CHECK(result.errorCount == 0);
    return result.text;
  }

  std::string rgbHex(std::uint32_t rgba) {
    char out[8];
    std::snprintf(out, sizeof(out), "#%06x", static_cast<unsigned>(rgba >> 8));
    return out;
  }

  std::string argbHex(std::uint32_t rgba) {
    char out[10];
    std::snprintf(out, sizeof(out), "#%02x%06x", rgba & 0xffU, static_cast<unsigned>(rgba >> 8));
    return out;
  }

} // namespace

int main() {
  TEST_CHECK(render("{{tokens.window.hex}}", "light") == rgbHex(Tokens::schemeLight.window));
  TEST_CHECK(render("{{tokens.window.hex}}", "dark") == rgbHex(Tokens::schemeDark.window));
  TEST_CHECK(render("{{tokens.label_3.hex_argb}}", "dark") == argbHex(Tokens::schemeDark.label3));

  return 0;
}

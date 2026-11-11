#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/palette.h"

#include <memory>
#include <print>
#include <string>

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "palette_tone_test: FAIL: {}", message);
      g_ok = false;
    }
  }

  std::unique_ptr<Box> labelBox() {
    auto box = std::make_unique<Box>();
    box->setFill(colorSpecFromRole(ColorRole::Label1));
    return box;
  }

} // namespace

int main() {
  const Palette light = tokenPalette(true);
  const Palette dark = tokenPalette(false);
  setDarkPalette(dark);
  setPalette(light);
  expect(light.label1 != dark.label1, "light and dark label_1 differ");

  Node darkRoot;
  darkRoot.setPaletteTone(PaletteTone::Dark);
  Box* attached = static_cast<Box*>(darkRoot.addChild(labelBox()));
  expect(attached->style().fill == dark.label1, "a box added under a Dark root resolves the dark label_1");

  Node schemeRoot;
  Box* plain = static_cast<Box*>(schemeRoot.addChild(labelBox()));
  expect(plain->style().fill == light.label1, "a box under a Scheme root resolves the scheme label_1");

  schemeRoot.setPaletteTone(PaletteTone::Dark);
  expect(plain->style().fill == dark.label1, "a root that turns Dark re-resolves its children");
  schemeRoot.setPaletteTone(std::nullopt);
  expect(plain->style().fill == light.label1, "a root that drops its tone returns to the scheme");

  setPalette(dark);
  setPalette(light);
  expect(attached->style().fill == dark.label1, "a palette change keeps the Dark subtree dark");

  return g_ok ? 0 : 1;
}

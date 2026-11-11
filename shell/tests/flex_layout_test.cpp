// Flex places children by padding, gap, grow, justify and align.
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "tests/test_check.h"
#include "ui/controls/flex.h"

#include <cstdlib>
#include <memory>

std::unique_ptr<TextureManager> createDefaultTextureManager() { return nullptr; }

namespace {

  class StubRenderer : public Renderer {
  public:
    TextMetrics measureText(std::string_view, float fontSize, const TextFont&, float, int, TextAlign, TextEllipsize, bool)
        override {
      return TextMetrics{.bottom = fontSize};
    }
    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float fontSize) override { return TextMetrics{.bottom = fontSize}; }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  Flex* addBox(Flex& parent, float width, float height, float grow = 0.0F) {
    auto box = std::make_unique<Flex>();
    box->setMinWidth(width);
    box->setMinHeight(height);
    box->setFlexGrow(grow);
    auto* raw = box.get();
    parent.addChild(std::move(box));
    return raw;
  }

  std::unique_ptr<Flex> makeRow(FlexJustify justify, FlexAlign align) {
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setPadding(10.0F);
    row->setGap(8.0F);
    row->setJustify(justify);
    row->setAlign(align);
    return row;
  }

  void growFillsTheRest(StubRenderer& renderer) {
    auto row = makeRow(FlexJustify::Start, FlexAlign::Start);
    auto* a = addBox(*row, 50.0F, 20.0F);
    auto* b = addBox(*row, 50.0F, 20.0F, 1.0F);
    auto* c = addBox(*row, 50.0F, 20.0F);
    row->setSize(300.0F, 100.0F);
    row->layout(renderer);
    TEST_CHECK(a->x() == 10.0F && a->y() == 10.0F && a->width() == 50.0F);
    TEST_CHECK(b->x() == 68.0F && b->width() == 164.0F);
    TEST_CHECK(c->x() == 240.0F && c->width() == 50.0F);
  }

  void justifyAndAlignCenter(StubRenderer& renderer) {
    auto row = makeRow(FlexJustify::Center, FlexAlign::Center);
    auto* a = addBox(*row, 50.0F, 20.0F);
    auto* b = addBox(*row, 50.0F, 40.0F);
    row->setSize(300.0F, 100.0F);
    row->layout(renderer);
    TEST_CHECK(a->x() == 96.0F && a->y() == 40.0F);
    TEST_CHECK(b->x() == 154.0F && b->y() == 30.0F);
  }

  void justifyEndColumn(StubRenderer& renderer) {
    auto column = makeRow(FlexJustify::End, FlexAlign::End);
    column->setDirection(FlexDirection::Vertical);
    auto* a = addBox(*column, 50.0F, 20.0F);
    auto* b = addBox(*column, 30.0F, 20.0F);
    column->setSize(100.0F, 200.0F);
    column->layout(renderer);
    TEST_CHECK(b->y() == 170.0F && a->y() == 142.0F);
    TEST_CHECK(a->x() == 40.0F && b->x() == 60.0F);
  }

} // namespace

int main() {
  StubRenderer renderer;
  growFillsTheRest(renderer);
  justifyAndAlignCenter(renderer);
  justifyEndColumn(renderer);
  return EXIT_SUCCESS;
}

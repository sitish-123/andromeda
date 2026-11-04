#include "render/core/color.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "tests/test_check.h"

#include <cstdint>
#include <memory>
#include <vector>

int main() {
  constexpr std::uint32_t kSize = 16;
  GlSharedContext shared;
  shared.initializeSurfaceless();
  RenderContext context;
  context.initialize(shared);

  RenderTarget target;
  target.createOffscreen(context);
  target.setContentScale(1.0F);
  target.setLogicalSize(kSize, kSize);
  target.resize(kSize, kSize);

  Node root;
  root.setSize(kSize, kSize);
  auto rect = std::make_unique<RectNode>();
  rect->setSize(kSize, kSize / 2);
  rect->setStyle({.fill = rgba(1.0F, 0.0F, 0.0F), .noAa = true});
  root.addChild(std::move(rect));

  std::vector<std::uint8_t> pixels(kSize * kSize * 4);
  TEST_CHECK(!context.readPixels(target, pixels));
  TEST_CHECK(context.renderScene(target, &root));
  TEST_CHECK(context.readPixels(target, pixels));

  const auto at = [&pixels](std::uint32_t x, std::uint32_t y) { return &pixels[(y * kSize + x) * 4]; };
  // The rect fills the top half: rows come top-down.
  TEST_CHECK(at(4, 2)[0] == 255 && at(4, 2)[1] == 0 && at(4, 2)[3] == 255);
  TEST_CHECK(at(4, 12)[3] == 0);

  target.destroy();
  context.cleanup();
  shared.cleanup();
  return 0;
}

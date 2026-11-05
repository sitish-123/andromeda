#pragma once

#include "document_model.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Request bodies for the helper's OP_ANNOT_ADD (andromeda-preview/helper/src/annotate.rs) and the saved signature.
// Coordinates are points, origin top left of the page.
namespace viewer::annot {

  struct Point {
    float x = 0.0F;
    float y = 0.0F;
  };
  using Stroke = std::vector<Point>;

  [[nodiscard]] std::vector<std::uint8_t> highlightBody(const std::vector<RectF>& rects);
  [[nodiscard]] std::vector<std::uint8_t> noteBody(Point at, const std::string& text);
  [[nodiscard]] std::vector<std::uint8_t> inkBody(const std::vector<Stroke>& strokes);
  // pixels: premultiplied BGRA (cairo ARGB32), stride width * 4.
  [[nodiscard]] std::vector<std::uint8_t> stampBody(RectF box, std::uint32_t width, std::uint32_t height,
                                                    const std::vector<std::uint8_t>& pixels);
  // A sealed memfd holding `bytes`; -1 on failure. The caller closes it.
  [[nodiscard]] int sealedMemfd(const std::vector<std::uint8_t>& bytes);

  struct Signature {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // premultiplied BGRA, stride width * 4
  };
  [[nodiscard]] std::filesystem::path signaturePath();
  [[nodiscard]] std::optional<Signature> loadSignature();
  // Draws the strokes (pad coordinates in px) in black on transparent and saves them as the signature.
  bool saveSignature(const std::vector<Stroke>& strokes, std::uint32_t width, std::uint32_t height);

} // namespace viewer::annot

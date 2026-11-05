#include "annotations.h"

#include <cairo.h>
#include <cstring>
#include <fcntl.h>
#include <glib.h>
#include <sys/mman.h>
#include <unistd.h>

namespace viewer::annot {

  namespace {
    void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
      }
    }
    void putF32(std::vector<std::uint8_t>& out, float v) {
      std::uint32_t bits = 0;
      std::memcpy(&bits, &v, sizeof(bits));
      putU32(out, bits);
    }
    void putRect(std::vector<std::uint8_t>& out, const RectF& r) {
      for (const float v : {r.x0, r.y0, r.x1, r.y1}) {
        putF32(out, v);
      }
    }
  } // namespace

  std::vector<std::uint8_t> highlightBody(const std::vector<RectF>& rects) {
    std::vector<std::uint8_t> out;
    putU32(out, static_cast<std::uint32_t>(rects.size()));
    for (const RectF& r : rects) {
      putRect(out, r);
    }
    return out;
  }

  std::vector<std::uint8_t> noteBody(Point at, const std::string& text) {
    std::vector<std::uint8_t> out;
    putF32(out, at.x);
    putF32(out, at.y);
    out.insert(out.end(), text.begin(), text.end());
    return out;
  }

  std::vector<std::uint8_t> inkBody(const std::vector<Stroke>& strokes) {
    std::vector<std::uint8_t> out;
    putU32(out, static_cast<std::uint32_t>(strokes.size()));
    for (const Stroke& s : strokes) {
      putU32(out, static_cast<std::uint32_t>(s.size()));
      for (const Point& p : s) {
        putF32(out, p.x);
        putF32(out, p.y);
      }
    }
    return out;
  }

  std::vector<std::uint8_t> stampBody(RectF box, std::uint32_t width, std::uint32_t height,
                                      const std::vector<std::uint8_t>& pixels) {
    std::vector<std::uint8_t> out;
    putRect(out, box);
    putU32(out, width);
    putU32(out, height);
    out.insert(out.end(), pixels.begin(), pixels.end());
    return out;
  }

  int sealedMemfd(const std::vector<std::uint8_t>& bytes) {
    const int fd = memfd_create("viewer-annot", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
      return -1;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
      if (n <= 0) {
        close(fd);
        return -1;
      }
      done += static_cast<std::size_t>(n);
    }
    // The helper reads from offset 0 of its own copy of the fd.
    lseek(fd, 0, SEEK_SET);
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    return fd;
  }

  std::filesystem::path signaturePath() {
    return std::filesystem::path(g_get_user_data_dir()) / "andromeda" / "signature.png";
  }

  std::optional<Signature> loadSignature() {
    cairo_surface_t* png = cairo_image_surface_create_from_png(signaturePath().c_str());
    if (cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(png);
      return std::nullopt;
    }
    const int w = cairo_image_surface_get_width(png);
    const int h = cairo_image_surface_get_height(png);
    // Repaint into ARGB32 so the stride is width * 4 whatever format the PNG had.
    cairo_surface_t* argb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(argb);
    cairo_set_source_surface(cr, png, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(argb);
    Signature sig{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), {}};
    const int stride = cairo_image_surface_get_stride(argb);
    const unsigned char* data = cairo_image_surface_get_data(argb);
    sig.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
      std::memcpy(sig.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4,
                  data + static_cast<std::ptrdiff_t>(y) * stride, static_cast<std::size_t>(w) * 4);
    }
    cairo_surface_destroy(argb);
    cairo_surface_destroy(png);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
      return std::nullopt;
    }
    return sig;
  }

  bool saveSignature(const std::vector<Stroke>& strokes, std::uint32_t width, std::uint32_t height) {
    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width), static_cast<int>(height));
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (const Stroke& s : strokes) {
      if (s.empty()) {
        continue;
      }
      cairo_move_to(cr, s.front().x, s.front().y);
      for (const Point& p : s) {
        cairo_line_to(cr, p.x, p.y);
      }
      cairo_stroke(cr);
    }
    cairo_destroy(cr);
    std::error_code ec;
    std::filesystem::create_directories(signaturePath().parent_path(), ec);
    const auto tmp = signaturePath().string() + ".tmp";
    const bool ok = cairo_surface_write_to_png(surface, tmp.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    if (ok) {
      std::filesystem::rename(tmp, signaturePath(), ec);
    }
    return ok && !ec;
  }

} // namespace viewer::annot

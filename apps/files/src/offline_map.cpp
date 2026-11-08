#include "offline_map.h"

#include "core/files/resource_paths.h"
#include "core/log.h"

#include <cairo.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <numbers>
#include <string_view>

namespace offline_map {

  namespace {
    constexpr Logger kLog("offline-map");
    constexpr double kSpanLon = 8.0;     // degrees across the map: a region, the land data has no streets
    constexpr double kNearKm = 50.0;
    constexpr double kEarthKm = 6371.0;

    struct Point {
      float lon = 0.0F;
      float lat = 0.0F;
    };
    struct Place {
      float lat = 0.0F;
      float lon = 0.0F;
      std::string name;
    };

    std::once_flag g_loaded;
    std::vector<std::vector<Point>> g_land;
    std::vector<std::vector<Point>> g_borders;
    std::vector<Place> g_places;

    bool parseFloat(std::string_view text, float& out) {
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
      return ec == std::errc() && end == text.data() + text.size();
    }

    std::vector<std::vector<Point>> readLines(const char* asset, std::size_t minPoints) {
      std::vector<std::vector<Point>> out;
      std::ifstream file(paths::assetPath(asset));
      for (std::string line; std::getline(file, line);) {
        std::vector<Point> ring;
        std::string_view rest(line);
        while (!rest.empty()) {
          const auto space = rest.find(' ');
          const std::string_view pair = rest.substr(0, space);
          rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
          const auto comma = pair.find(',');
          Point p;
          if (comma != std::string_view::npos && parseFloat(pair.substr(0, comma), p.lon) &&
              parseFloat(pair.substr(comma + 1), p.lat)) {
            ring.push_back(p);
          }
        }
        if (ring.size() >= minPoints) {
          out.push_back(std::move(ring));
        }
      }
      return out;
    }

    void load() {
      g_land = readLines("geo/land.txt", 3);
      g_borders = readLines("geo/borders.txt", 2);
      std::ifstream places(paths::assetPath("geo/places.tsv"));
      for (std::string line; std::getline(places, line);) {
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        const auto c = b == std::string::npos ? b : line.find('\t', b + 1);
        Place p;
        if (c == std::string::npos || !parseFloat(std::string_view(line).substr(0, a), p.lat) ||
            !parseFloat(std::string_view(line).substr(a + 1, b - a - 1), p.lon)) {
          continue;
        }
        p.name = line.substr(b + 1, c - b - 1) + ", " + line.substr(c + 1);
        g_places.push_back(std::move(p));
      }
      kLog.debug("loaded {} land rings, {} places", g_land.size(), g_places.size());
    }

    double radians(double deg) { return deg * std::numbers::pi / 180.0; }

    double distanceKm(double lat1, double lon1, double lat2, double lon2) {
      const double dLat = radians(lat2 - lat1);
      const double dLon = radians(lon2 - lon1);
      const double h = std::sin(dLat / 2) * std::sin(dLat / 2) +
                       std::cos(radians(lat1)) * std::cos(radians(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
      return 2.0 * kEarthKm * std::asin(std::min(1.0, std::sqrt(h)));
    }
  } // namespace

  Picture render(double latitude, double longitude, int width, int height, float scale) {
    std::call_once(g_loaded, load);
    Picture out;
    if (width <= 0 || height <= 0) {
      return out;
    }
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);
    // Mockup .gmap: a dark sea gradient; land a little lighter, its coast a hairline.
    cairo_pattern_t* sea = cairo_pattern_create_linear(0, 0, width, height);
    cairo_pattern_add_color_stop_rgb(sea, 0, 0x23 / 255.0, 0x30 / 255.0, 0x3a / 255.0);
    cairo_pattern_add_color_stop_rgb(sea, 1, 0x1a / 255.0, 0x2a / 255.0, 0x24 / 255.0);
    cairo_set_source(cr, sea);
    cairo_paint(cr);
    cairo_pattern_destroy(sea);

    // Equirectangular around the point, x scaled by cos(latitude) so shapes keep their proportions.
    const double pxPerLon = width / kSpanLon;
    const double pxPerLat = pxPerLon / std::max(0.2, std::cos(radians(latitude)));
    const double spanLat = height / pxPerLat;
    const auto x = [&](double lon) {
      double d = lon - longitude;
      d -= 360.0 * std::round(d / 360.0); // across the antimeridian
      return width / 2.0 + d * pxPerLon;
    };
    const auto y = [&](double lat) { return height / 2.0 - (lat - latitude) * pxPerLat; };
    const auto near = [&](const std::vector<Point>& points) {
      return std::ranges::any_of(points, [&](const Point& p) {
        return std::abs(p.lat - latitude) < spanLat && std::abs(x(p.lon) - width / 2.0) < width;
      });
    };
    for (const auto& ring : g_land) {
      if (!near(ring)) {
        continue;
      }
      cairo_move_to(cr, x(ring.front().lon), y(ring.front().lat));
      for (const Point& p : ring) {
        cairo_line_to(cr, x(p.lon), y(p.lat));
      }
      cairo_close_path(cr);
    }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
    cairo_set_line_width(cr, std::max(1.0F, scale));
    cairo_stroke(cr);
    // Country borders: inland places show where they are, not only a land fill.
    for (const auto& line : g_borders) {
      if (!near(line)) {
        continue;
      }
      cairo_move_to(cr, x(line.front().lon), y(line.front().lat));
      for (const Point& p : line) {
        cairo_line_to(cr, x(p.lon), y(p.lat));
      }
    }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.24);
    cairo_set_line_width(cr, std::max(1.0F, scale));
    cairo_stroke(cr);

    // The pin (mockup .gmap .pin): 24 round, white 2 ring, blue-green fill.
    const double r = 12.0 * scale;
    cairo_pattern_t* fill = cairo_pattern_create_linear(width / 2.0 - r, height / 2.0 - r, width / 2.0 + r, height / 2.0 + r);
    cairo_pattern_add_color_stop_rgb(fill, 0, 0x3b / 255.0, 0x6f / 255.0, 0xd8 / 255.0);
    cairo_pattern_add_color_stop_rgb(fill, 1, 0x7c / 255.0, 0xc3 / 255.0, 0xa3 / 255.0);
    cairo_arc(cr, width / 2.0, height / 2.0, r - scale, 0, 2 * std::numbers::pi);
    cairo_set_source(cr, fill);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(fill);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2.0 * scale);
    cairo_stroke(cr);

    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    out.width = width;
    out.height = height;
    out.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row) {
      std::copy_n(data + static_cast<std::size_t>(row) * stride, static_cast<std::size_t>(width) * 4,
                  out.bgra.begin() + static_cast<std::ptrdiff_t>(row) * width * 4);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return out;
  }

  std::string placeName(double latitude, double longitude) {
    std::call_once(g_loaded, load);
    const Place* best = nullptr;
    double bestKm = kNearKm;
    for (const Place& p : g_places) {
      if (std::abs(p.lat - latitude) > 1.0) {
        continue; // 1 degree of latitude is 111 km: far outside the 50 km radius
      }
      const double km = distanceKm(latitude, longitude, p.lat, p.lon);
      if (km < bestKm) {
        bestKm = km;
        best = &p;
      }
    }
    if (best != nullptr) {
      return best->name;
    }
    return std::format("{:.4f}° {}, {:.4f}° {}", std::abs(latitude), latitude < 0 ? 'S' : 'N', std::abs(longitude),
                       longitude < 0 ? 'W' : 'E');
  }

} // namespace offline_map

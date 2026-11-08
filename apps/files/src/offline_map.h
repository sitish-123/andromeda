#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The Gallery Info card map (mockup section 15): a regional land outline around a GPS position with a pin, drawn
// from the bundled Natural Earth land and GeoNames places (assets/geo). No network. Blocks on the first call (reads
// about 2 MB): call off the UI thread. Thread-safe.
namespace offline_map {

  struct Picture {
    std::vector<std::uint8_t> bgra; // cairo ARGB32: B, G, R, A in memory, opaque
    int width = 0;
    int height = 0;
  };

  [[nodiscard]] Picture render(double latitude, double longitude, int width, int height, float scale);
  // "Kathmandu, Nepal" for the nearest place of 15 000+ people within 50 km, else the coordinates.
  [[nodiscard]] std::string placeName(double latitude, double longitude);

} // namespace offline_map

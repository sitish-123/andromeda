// Drives default_apps::setDefault directly (the same call the Default Apps page's control makes) against a
// mimeapps.list path given on argv, then reads back currentDefault to confirm the round trip.
#include "system/default_apps.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::puts("usage: default_apps_set <mimeapps.list path> <category> <desktop-id>");
    return 1;
  }
  const std::filesystem::path path = argv[1];
  const std::string categoryName = argv[2];
  const std::string desktopId = argv[3];

  default_apps::Category category{};
  bool found = false;
  for (const auto& spec : default_apps::categories()) {
    if (categoryName == spec.mimeType) {
      category = spec.category;
      found = true;
      break;
    }
  }
  if (!found) {
    std::printf("FAIL unknown category %s\n", categoryName.c_str());
    return 1;
  }

  const bool wrote = default_apps::setDefault(path, category, desktopId);
  std::printf("%s setDefault(%s, %s)\n", wrote ? "PASS" : "FAIL", categoryName.c_str(), desktopId.c_str());

  const std::string readBack = default_apps::currentDefault(path, category);
  const bool matches = readBack == desktopId;
  std::printf("%s currentDefault reads back %s (got %s)\n", matches ? "PASS" : "FAIL", desktopId.c_str(), readBack.c_str());

  return wrote && matches ? 0 : 1;
}

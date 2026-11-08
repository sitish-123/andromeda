#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// One mounted drive for This PC (mockup section 0) and the sidebar Locations.
struct Volume {
  std::string name;               // "This PC" for the root file system, else the mount point's last part
  std::filesystem::path mountPoint;
  std::uint64_t totalBytes = 0;
  std::uint64_t freeBytes = 0;    // available to this user
  bool removable = false;         // mounted under /run/media or /media
};

// Block-device mounts a user browses: the root, /home on its own drive, and drives under /run/media, /media and /mnt.
// Reads /proc/self/mounts and statvfs only on /dev devices, so a hung network share never blocks the caller.
[[nodiscard]] std::vector<Volume> listVolumes();

#pragma once

#include "index_db.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace andromeda::index {

  struct CrawlStats {
    std::int64_t files = 0;
    std::int64_t folders = 0;
    std::int64_t skippedFolders = 0;
  };

  // Walks root breadth first and writes every entry to db, applying the skip rules. Folders in `first` are walked
  // before the rest of root, so the folders people search most are ready soonest.
  CrawlStats crawl(IndexDb& db, const std::filesystem::path& root, const std::vector<std::filesystem::path>& first = {});

} // namespace andromeda::index

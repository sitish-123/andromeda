#pragma once

#include "index_db.h"

#include <cstdint>
#include <filesystem>

namespace andromeda::index {

  struct CrawlStats {
    std::int64_t files = 0;
    std::int64_t folders = 0;
    std::int64_t skippedFolders = 0;
    std::int64_t walkMs = 0; // wall time of the parallel walk, database writes included
    std::int64_t dbMs = 0;   // writer thread busy time
    std::int64_t indexMs = 0;
  };

  // Walks root on a thread pool and writes every entry to db, which must be empty (a bulk database), applying the
  // skip rules; then builds the name index.
  CrawlStats crawl(IndexDb& db, const std::filesystem::path& root);

} // namespace andromeda::index

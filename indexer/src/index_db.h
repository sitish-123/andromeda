#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace andromeda::index {

  struct FileRow {
    std::string path;
    std::string name;
    std::int64_t mtime = 0; // seconds since the epoch
    std::int64_t size = 0;
    bool isDir = false;
  };

  // The file index: one SQLite database in WAL mode with an FTS5 trigram table over file names.
  class IndexDb {
  public:
    explicit IndexDb(const std::filesystem::path& file);
    ~IndexDb();
    IndexDb(const IndexDb&) = delete;
    IndexDb& operator=(const IndexDb&) = delete;

    [[nodiscard]] bool ok() const { return m_db != nullptr; }

    void begin();
    void commit();
    void upsert(const FileRow& row);
    // Removes path and everything below it.
    void removeTree(std::string_view path);

    // Name search: exact name first, then name prefix, then substring; newer files first within each group.
    // Queries of 1 or 2 characters match name prefixes only (the trigram index needs 3).
    [[nodiscard]] std::vector<FileRow> queryNames(std::string_view text, int limit) const;
    [[nodiscard]] std::int64_t count() const;

  private:
    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_upsert = nullptr;
  };

} // namespace andromeda::index

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

  // The file index: one SQLite database in WAL mode. Files form a tree (parent id + name, roots have parent 0 and
  // their absolute path as name), so no full path is stored; an FTS5 trigram index (detail=none) covers names.
  class IndexDb {
  public:
    // bulk: a fresh file built by one crawl; no journal until finishBulk(), so a crash leaves only a temp file.
    explicit IndexDb(const std::filesystem::path& file, bool bulk = false);
    ~IndexDb();
    IndexDb(const IndexDb&) = delete;
    IndexDb& operator=(const IndexDb&) = delete;

    [[nodiscard]] bool ok() const { return m_db != nullptr; }

    void begin();
    void commit();
    // Rebuilds the trigram name index from the files table.
    void rebuildNames();
    // Inserts a row with a known id into a bulk database.
    void insert(std::int64_t id, std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir);
    // Ends a bulk build: switches the file to WAL for normal use.
    void finishBulk();
    // Id of an absolute path, or nullopt when it is not indexed.
    [[nodiscard]] std::optional<std::int64_t> idOf(std::string_view path) const;
    // Removes path and everything below it, from the files table and the name index.
    void removeTree(std::string_view path);

    // Name search: exact name first, then name prefix, then substring; newer files first within each group.
    // Queries need 3 characters (the trigram length); shorter ones return nothing.
    [[nodiscard]] std::vector<FileRow> queryNames(std::string_view text, int limit) const;
    [[nodiscard]] std::int64_t count() const;

  private:
    [[nodiscard]] std::string pathOf(std::int64_t id) const;

    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_insert = nullptr;
    sqlite3_stmt* m_parentOf = nullptr;
    sqlite3_stmt* m_child = nullptr;
  };

} // namespace andromeda::index

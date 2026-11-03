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
    std::int64_t mtime = 0; // nanoseconds since the epoch
    std::int64_t size = 0;
    bool isDir = false;
  };

  struct Child {
    std::int64_t id = 0;
    std::string name;
    std::int64_t mtime = 0;
    std::int64_t size = 0;
    bool isDir = false;
  };

  struct ContentTask {
    std::int64_t id = 0;
    std::int64_t mtime = 0;
    std::string path;
  };

  struct Folder {
    std::int64_t id = 0;
    std::string path;
    std::int64_t mtime = 0;
  };

  // The file index: one SQLite database in WAL mode. Files form a tree (parent id + name, roots have parent 0 and
  // their absolute path as name), so no full path is stored. Name search scans an in-memory copy of all names
  // (about 50 B per entry): SQLite FTS5 costs about 1 us per match, too slow for 3-letter queries.
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
    // Inserts a row with a known id into a bulk database.
    void insert(std::int64_t id, std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir);
    // Ends a bulk build: switches the file to WAL for normal use.
    void finishBulk();
    // Live updates: add a new entry and return its id; change size and time of an entry.
    std::int64_t add(std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir);
    void update(std::int64_t id, std::int64_t mtime, std::int64_t size);
    [[nodiscard]] std::vector<Child> children(std::int64_t parent) const;
    // Every indexed folder with its absolute path, roots first.
    [[nodiscard]] std::vector<Folder> folders() const;
    // Removes an entry and everything below it.
    void removeTree(std::int64_t id);
    // Id of an absolute path, or nullopt when it is not indexed.
    [[nodiscard]] std::optional<std::int64_t> idOf(std::string_view path) const;
    // Removes path and everything below it.
    void removeTree(std::string_view path);

    // Name search, case-insensitive for ASCII: exact name first, then name prefix, then substring; newer files
    // first within each group. Queries need 3 characters; shorter ones return nothing. The first query loads the
    // names into memory; loadNames() does it ahead of time. Writes by insert() after the load are not seen.
    [[nodiscard]] std::vector<FileRow> queryNames(std::string_view text, int limit) const;
    void loadNames() const;
    [[nodiscard]] std::int64_t count() const;

    // Content index (FTS5, unicode61 without diacritics): words in file text, prefix match on the last word.
    // Files whose text is missing or older than the file, extractable by name and at most kMaxContentFile bytes.
    [[nodiscard]] std::vector<ContentTask> contentTasks() const;
    // Marks a file as being read before extraction starts, so a crash in an extractor skips the file next time.
    void markExtracting(std::int64_t id, std::int64_t mtime);
    // Replaces the text of a file; nullopt records a file that could not be read.
    void storeContent(std::int64_t id, std::int64_t mtime, const std::optional<std::string>& text);
    // Files left marked by a crashed extraction are recorded as unreadable.
    void failInterrupted();
    // Gives back the page cache of this connection.
    void releaseMemory();
    // Files whose text holds every word of text, best match first; skips the paths in excludePaths.
    [[nodiscard]] std::vector<FileRow> queryContent(std::string_view text, int limit,
                                                    const std::vector<std::string>& excludePaths = {}) const;
    static constexpr std::int64_t kMaxContentFile = 50LL * 1024 * 1024;

  private:
    // Entries sorted by id (add() never reuses an id held here, removed or not), so offsets grow too. 16 bytes:
    // ids fit 32 bits, as each id is one more than the largest so far.
    struct NameEntry {
      std::uint32_t id;
      std::uint32_t offset; // into m_folded
      std::int64_t mtime;   // kRemoved once removed
    };
    static constexpr std::int64_t kRemoved = INT64_MIN;

    [[nodiscard]] std::string pathOf(std::int64_t id) const;
    [[nodiscard]] NameEntry* entryOf(std::int64_t id) const;
    void appendName(std::int64_t id, std::string_view name, std::int64_t mtime) const;

    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_insert = nullptr;
    sqlite3_stmt* m_row = nullptr;
    sqlite3_stmt* m_parentOf = nullptr;
    sqlite3_stmt* m_child = nullptr;
    // ASCII-lowercased names, each followed by a NUL (names never hold one), so a match never spans two names.
    mutable std::string m_folded;
    mutable std::vector<NameEntry> m_entries;
    mutable std::size_t m_removed = 0;
    mutable bool m_loaded = false;
  };

} // namespace andromeda::index

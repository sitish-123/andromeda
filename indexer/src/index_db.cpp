#include "index_db.h"

#include "extract.h"

#include <emmintrin.h>
#include <sqlite3.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>

namespace andromeda::index {

  namespace {
    constexpr const char* kSchema = R"sql(
      CREATE TABLE IF NOT EXISTS files (
        id INTEGER PRIMARY KEY,
        parent INTEGER NOT NULL,
        name TEXT NOT NULL,
        mtime INTEGER NOT NULL,
        size INTEGER NOT NULL,
        is_dir INTEGER NOT NULL,
        UNIQUE (parent, name)
      );
      DROP TABLE IF EXISTS names;
      -- state: 0 being read, 1 done, 2 unreadable
      CREATE TABLE IF NOT EXISTS extracted (id INTEGER PRIMARY KEY, mtime INTEGER NOT NULL, state INTEGER NOT NULL);
      CREATE VIRTUAL TABLE IF NOT EXISTS content USING fts5(body, content='', contentless_delete=1,
                                                            tokenize='unicode61 remove_diacritics 2', prefix='3');
    )sql";
    // Version 2: content index with word positions (BM25 ranking) and a 3-letter prefix index.
    constexpr int kSchemaVersion = 2;
    constexpr const char* kDropContent = "DROP TABLE IF EXISTS content; DROP TABLE IF EXISTS extracted;";

    // An older content index is dropped; the content worker reads the documents again.
    bool migrate(sqlite3* db) {
      sqlite3_stmt* stmt = nullptr;
      sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
      const int version = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
      sqlite3_finalize(stmt);
      return version >= kSchemaVersion || sqlite3_exec(db, kDropContent, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    void extractableFunction(sqlite3_context* context, int /*argc*/, sqlite3_value** argv) {
      const auto* name = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
      sqlite3_result_int(context, name != nullptr && extractable(name) ? 1 : 0);
    }

    // An FTS5 query: every word must appear, the last one as a prefix (it may be half typed).
    std::string contentMatch(std::string_view text) {
      std::vector<std::string> words;
      std::string word;
      for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) >= 0x80) {
          word += c;
        } else if (!word.empty()) {
          words.push_back(std::move(word));
          word.clear();
        }
      }
      if (!word.empty()) {
        words.push_back(std::move(word));
      }
      std::string match;
      for (std::size_t i = 0; i < words.size(); ++i) {
        match += (i == 0 ? "\"" : " \"") + words[i] + "\"";
        if (i + 1 == words.size()) {
          match += '*';
        }
      }
      return match;
    }

    std::string trimmed(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
      }
      return std::string(text);
    }

    // First match of needle (3 bytes or more) in [at, end). An SSE2 filter on its first and last byte reads about
    // 16 bytes per step; glibc memmem manages about 3 GB/s on short needles.
    const char* findIn(const char* at, const char* end, std::string_view needle) {
      const std::size_t n = needle.size();
      const __m128i first = _mm_set1_epi8(needle.front());
      const __m128i last = _mm_set1_epi8(needle.back());
      for (; end - at >= static_cast<std::ptrdiff_t>(n + 15); at += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at + n - 1));
        auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, last))));
        for (; mask != 0; mask &= mask - 1) {
          const char* candidate = at + std::countr_zero(mask);
          if (std::memcmp(candidate + 1, needle.data() + 1, n - 2) == 0) {
            return candidate;
          }
        }
      }
      return static_cast<const char*>(memmem(at, static_cast<std::size_t>(end - at), needle.data(), n));
    }

    std::string folded(std::string_view text) {
      std::string out(text);
      for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return out;
    }
  } // namespace

  IndexDb::IndexDb(const std::filesystem::path& file, bool bulk) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const char* pragmas = bulk ? "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;"
                               : "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;";
    if (sqlite3_open(file.c_str(), &m_db) != SQLITE_OK || sqlite3_exec(m_db, pragmas, nullptr, nullptr, nullptr) != SQLITE_OK
        || !migrate(m_db) || sqlite3_exec(m_db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK
        || sqlite3_exec(m_db, std::format("PRAGMA user_version = {}", kSchemaVersion).c_str(), nullptr, nullptr, nullptr)
            != SQLITE_OK) {
      std::fprintf(stderr, "andromeda-index: %s: %s\n", file.c_str(), m_db != nullptr ? sqlite3_errmsg(m_db) : "open failed");
      sqlite3_close(m_db);
      m_db = nullptr;
      return;
    }
    sqlite3_prepare_v3(
        m_db, "INSERT INTO files(id, parent, name, mtime, size, is_dir) VALUES (?1, ?2, ?3, ?4, ?5, ?6)", -1,
        SQLITE_PREPARE_PERSISTENT, &m_insert, nullptr
    );
    sqlite3_prepare_v3(m_db, "SELECT name, mtime, size, is_dir FROM files WHERE id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &m_row, nullptr);
    sqlite3_prepare_v3(m_db, "SELECT parent, name FROM files WHERE id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &m_parentOf, nullptr);
    sqlite3_prepare_v3(m_db, "SELECT id FROM files WHERE parent = ?1 AND name = ?2", -1, SQLITE_PREPARE_PERSISTENT, &m_child, nullptr);
    sqlite3_busy_timeout(m_db, 5000); // the content worker writes on its own connection
    sqlite3_create_function(m_db, "extractable", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, extractableFunction,
                            nullptr, nullptr);
  }

  IndexDb::~IndexDb() {
    sqlite3_finalize(m_insert);
    sqlite3_finalize(m_row);
    sqlite3_finalize(m_parentOf);
    sqlite3_finalize(m_child);
    sqlite3_close(m_db);
  }

  void IndexDb::begin() { sqlite3_exec(m_db, "BEGIN", nullptr, nullptr, nullptr); }
  void IndexDb::commit() { sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr); }

  void IndexDb::insert(std::int64_t id, std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir) {
    sqlite3_bind_int64(m_insert, 1, id);
    sqlite3_bind_int64(m_insert, 2, parent);
    sqlite3_bind_text(m_insert, 3, name.data(), static_cast<int>(name.size()), SQLITE_STATIC);
    sqlite3_bind_int64(m_insert, 4, mtime);
    sqlite3_bind_int64(m_insert, 5, size);
    sqlite3_bind_int(m_insert, 6, isDir ? 1 : 0);
    sqlite3_step(m_insert);
    sqlite3_reset(m_insert);
  }

  void IndexDb::finishBulk() { sqlite3_exec(m_db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr); }

  std::string IndexDb::pathOf(std::int64_t id) const {
    std::vector<std::string> parts;
    while (id != 0) {
      sqlite3_bind_int64(m_parentOf, 1, id);
      if (sqlite3_step(m_parentOf) != SQLITE_ROW) {
        sqlite3_reset(m_parentOf);
        return {};
      }
      id = sqlite3_column_int64(m_parentOf, 0);
      parts.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(m_parentOf, 1)));
      sqlite3_reset(m_parentOf);
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      path += path.empty() || path.ends_with('/') ? *it : "/" + *it;
    }
    return path;
  }

  std::optional<std::int64_t> IndexDb::idOf(std::string_view path) const {
    // Roots carry their absolute path as name; the longest root that prefixes path wins.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT id, name FROM files WHERE parent = 0", -1, &stmt, nullptr);
    std::optional<std::int64_t> id;
    std::size_t rootLength = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const std::string_view name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      const bool prefix = path == name || (path.starts_with(name) && (name.ends_with('/') || path[name.size()] == '/'));
      if (prefix && name.size() >= rootLength) {
        id = sqlite3_column_int64(stmt, 0);
        rootLength = name.size();
      }
    }
    sqlite3_finalize(stmt);
    if (!id) {
      return std::nullopt;
    }
    std::string_view rest = path.substr(rootLength);
    while (!rest.empty()) {
      rest.remove_prefix(rest.front() == '/' ? 1 : 0);
      const std::string_view part = rest.substr(0, rest.find('/'));
      rest.remove_prefix(part.size());
      if (part.empty()) {
        continue;
      }
      sqlite3_bind_int64(m_child, 1, *id);
      sqlite3_bind_text(m_child, 2, part.data(), static_cast<int>(part.size()), SQLITE_STATIC);
      const bool found = sqlite3_step(m_child) == SQLITE_ROW;
      id = found ? std::optional(sqlite3_column_int64(m_child, 0)) : std::nullopt;
      sqlite3_reset(m_child);
      if (!found) {
        return std::nullopt;
      }
    }
    return id;
  }

  std::int64_t IndexDb::add(std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT coalesce(max(id), 0) + 1 FROM files", -1, &stmt, nullptr);
    std::int64_t id = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 1;
    sqlite3_finalize(stmt);
    // A removed newest entry still sits in memory; its id must not be given out again.
    if (m_loaded && !m_entries.empty()) {
      id = std::max<std::int64_t>(id, m_entries.back().id + 1);
    }
    insert(id, parent, name, mtime, size, isDir);
    if (m_loaded) {
      appendName(id, name, mtime);
    }
    return id;
  }

  void IndexDb::update(std::int64_t id, std::int64_t mtime, std::int64_t size) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "UPDATE files SET mtime = ?2, size = ?3 WHERE id = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int64(stmt, 3, size);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (NameEntry* entry = entryOf(id)) {
      entry->mtime = mtime;
    }
  }

  std::vector<Child> IndexDb::children(std::int64_t parent) const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT id, name, mtime, size, is_dir FROM files WHERE parent = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, parent);
    std::vector<Child> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      out.push_back({
          .id = sqlite3_column_int64(stmt, 0),
          .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
          .size = sqlite3_column_int64(stmt, 3),
          .isDir = sqlite3_column_int(stmt, 4) != 0,
      });
    }
    sqlite3_finalize(stmt);
    return out;
  }

  std::vector<Folder> IndexDb::folders() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        m_db,
        "WITH RECURSIVE t(id, path, mtime) AS (SELECT id, name, mtime FROM files WHERE parent = 0 "
        "UNION ALL SELECT f.id, t.path || '/' || f.name, f.mtime FROM files f JOIN t ON f.parent = t.id WHERE f.is_dir) "
        "SELECT id, path, mtime FROM t",
        -1, &stmt, nullptr
    );
    std::vector<Folder> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      out.push_back({
          .id = sqlite3_column_int64(stmt, 0),
          .path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
      });
    }
    sqlite3_finalize(stmt);
    return out;
  }

  void IndexDb::removeTree(std::string_view path) {
    if (const auto id = idOf(path)) {
      removeTree(*id);
    }
  }

  void IndexDb::removeTree(std::int64_t id) {
    // External-content FTS5 needs the old value to delete a row from the index.
    sqlite3_exec(m_db, "CREATE TEMP TABLE IF NOT EXISTS doomed(id INTEGER PRIMARY KEY); DELETE FROM doomed;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        m_db,
        "INSERT INTO doomed WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL SELECT f.id FROM files f JOIN sub ON f.parent = sub.id) "
        "SELECT id FROM sub",
        -1, &stmt, nullptr
    );
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (m_loaded) {
      sqlite3_prepare_v2(m_db, "SELECT id FROM doomed", -1, &stmt, nullptr);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (NameEntry* entry = entryOf(sqlite3_column_int64(stmt, 0)); entry != nullptr && entry->mtime != kRemoved) {
          entry->mtime = kRemoved;
          ++m_removed;
        }
      }
      sqlite3_finalize(stmt);
    }
    sqlite3_exec(m_db,
                 "DELETE FROM content WHERE rowid IN (SELECT id FROM doomed);"
                 "DELETE FROM extracted WHERE id IN (SELECT id FROM doomed);"
                 "DELETE FROM files WHERE id IN (SELECT id FROM doomed);",
                 nullptr, nullptr, nullptr);
    // Removed names stay in m_folded as dead space until it is half dead.
    if (m_removed * 2 > m_entries.size()) {
      m_loaded = false;
      loadNames();
    }
  }

  void IndexDb::loadNames() const {
    if (m_loaded || m_db == nullptr) {
      return;
    }
    m_folded.clear();
    m_entries.clear();
    m_removed = 0;
    sqlite3_stmt* stmt = nullptr;
    // Exact sizes up front: growth by doubling would leave up to twice the memory in the heap.
    sqlite3_prepare_v2(m_db, "SELECT count(*), coalesce(sum(length(CAST(name AS BLOB))), 0) FROM files", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      m_entries.reserve(static_cast<std::size_t>(sqlite3_column_int64(stmt, 0)) + 64);
      m_folded.reserve(static_cast<std::size_t>(sqlite3_column_int64(stmt, 0) + sqlite3_column_int64(stmt, 1)) + 4096);
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(m_db, "SELECT id, name, mtime FROM files ORDER BY id", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      appendName(sqlite3_column_int64(stmt, 0), {name, static_cast<std::size_t>(sqlite3_column_bytes(stmt, 1))},
                 sqlite3_column_int64(stmt, 2));
    }
    sqlite3_finalize(stmt);
    m_loaded = true;
  }

  void IndexDb::appendName(std::int64_t id, std::string_view name, std::int64_t mtime) const {
    m_entries.push_back({.id = static_cast<std::uint32_t>(id), .offset = static_cast<std::uint32_t>(m_folded.size()),
                         .mtime = mtime});
    m_folded += folded(name);
    m_folded += '\0';
  }

  IndexDb::NameEntry* IndexDb::entryOf(std::int64_t id) const {
    if (!m_loaded) {
      return nullptr;
    }
    const auto it = std::ranges::lower_bound(m_entries, static_cast<std::uint32_t>(id), {}, &NameEntry::id);
    return it != m_entries.end() && it->id == id ? &*it : nullptr;
  }

  std::vector<FileRow> IndexDb::queryNames(std::string_view text, int limit) const {
    const std::string needle = folded(trimmed(text));
    if (needle.size() < 3 || m_db == nullptr) {
      return {};
    }
    loadNames();
    struct Hit {
      int rank; // 0 exact name, 1 name prefix, 2 substring
      std::int64_t mtime;
      std::int64_t id;
    };
    std::vector<Hit> hits;
    const char* const begin = m_folded.data();
    const char* const end = begin + m_folded.size();
    auto entry = m_entries.begin();
    for (const char* at = begin; at < end;) {
      const char* found = findIn(at, end, needle);
      if (found == nullptr) {
        break;
      }
      const auto offset = static_cast<std::uint32_t>(found - begin);
      entry = std::ranges::upper_bound(entry, m_entries.end(), offset, {}, &NameEntry::offset) - 1;
      const auto next = entry + 1;
      at = next != m_entries.end() ? begin + next->offset : end;
      if (entry->mtime == kRemoved) {
        continue;
      }
      const int rank = offset != entry->offset ? 2 : (found[needle.size()] == '\0' ? 0 : 1);
      hits.push_back({.rank = rank, .mtime = entry->mtime, .id = entry->id});
    }
    const auto better = [](const Hit& a, const Hit& b) { return a.rank != b.rank ? a.rank < b.rank : a.mtime > b.mtime; };
    const auto keep = std::min(hits.size(), static_cast<std::size_t>(std::max(limit, 0)));
    std::ranges::partial_sort(hits, hits.begin() + static_cast<std::ptrdiff_t>(keep), better);
    std::vector<FileRow> rows;
    for (std::size_t i = 0; i < keep; ++i) {
      sqlite3_bind_int64(m_row, 1, hits[i].id);
      if (sqlite3_step(m_row) == SQLITE_ROW) {
        rows.push_back({
            .path = pathOf(hits[i].id),
            .name = reinterpret_cast<const char*>(sqlite3_column_text(m_row, 0)),
            .mtime = sqlite3_column_int64(m_row, 1),
            .size = sqlite3_column_int64(m_row, 2),
            .isDir = sqlite3_column_int(m_row, 3) != 0,
        });
      }
      sqlite3_reset(m_row);
    }
    return rows;
  }

  std::vector<ContentTask> IndexDb::contentTasks() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
                       "SELECT f.id, f.mtime FROM files f LEFT JOIN extracted e ON e.id = f.id "
                       "WHERE f.is_dir = 0 AND f.size <= ?1 AND (e.id IS NULL OR e.mtime != f.mtime) AND extractable(f.name) "
                       "ORDER BY f.mtime DESC",
                       -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, kMaxContentFile);
    std::vector<ContentTask> tasks;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      tasks.push_back({.id = sqlite3_column_int64(stmt, 0), .mtime = sqlite3_column_int64(stmt, 1), .path = {}});
    }
    sqlite3_finalize(stmt);
    for (ContentTask& task : tasks) {
      task.path = pathOf(task.id);
    }
    return tasks;
  }

  void IndexDb::markExtracting(std::int64_t id, std::int64_t mtime) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "INSERT OR REPLACE INTO extracted(id, mtime, state) VALUES (?1, ?2, 0)", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void IndexDb::storeContent(std::int64_t id, std::int64_t mtime, const std::optional<std::string>& text) {
    // IMMEDIATE: the write lock from the start, so the live index cannot remove the file between check and write.
    sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT 1 FROM files WHERE id = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!exists) {
      commit();
      return;
    }
    sqlite3_prepare_v2(m_db, "DELETE FROM content WHERE rowid = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (text && !text->empty()) {
      sqlite3_prepare_v2(m_db, "INSERT INTO content(rowid, body) VALUES (?1, ?2)", -1, &stmt, nullptr);
      sqlite3_bind_int64(stmt, 1, id);
      sqlite3_bind_text(stmt, 2, text->data(), static_cast<int>(text->size()), SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    sqlite3_prepare_v2(m_db, "INSERT OR REPLACE INTO extracted(id, mtime, state) VALUES (?1, ?2, ?3)", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int(stmt, 3, text ? 1 : 2);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    commit();
  }

  void IndexDb::releaseMemory() { sqlite3_db_release_memory(m_db); }

  void IndexDb::failInterrupted() {
    sqlite3_exec(m_db, "UPDATE extracted SET state = 2 WHERE state = 0", nullptr, nullptr, nullptr);
  }

  std::vector<FileRow> IndexDb::queryContent(std::string_view text, int limit,
                                             const std::vector<std::string>& excludePaths) const {
    const std::string match = contentMatch(text);
    if (match.empty() || limit <= 0) {
      return {};
    }
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
                       "SELECT f.id, f.name, f.mtime, f.size FROM content c JOIN files f ON f.id = c.rowid "
                       "WHERE content MATCH ?1 ORDER BY rank LIMIT ?2",
                       -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit + static_cast<int>(excludePaths.size()));
    std::vector<FileRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW && static_cast<int>(rows.size()) < limit) {
      FileRow row{
          .path = pathOf(sqlite3_column_int64(stmt, 0)),
          .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
          .size = sqlite3_column_int64(stmt, 3),
          .isDir = false,
      };
      if (std::ranges::find(excludePaths, row.path) == excludePaths.end()) {
        rows.push_back(std::move(row));
      }
    }
    sqlite3_finalize(stmt);
    return rows;
  }

  std::int64_t IndexDb::count() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM files", -1, &stmt, nullptr);
    const std::int64_t n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
  }

} // namespace andromeda::index

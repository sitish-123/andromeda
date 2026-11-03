#include "index_db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

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
      CREATE VIRTUAL TABLE IF NOT EXISTS names USING fts5(name, content='files', content_rowid='id',
                                                          tokenize='trigram case_sensitive 0', detail=none,
                                                          columnsize=0);
    )sql";

    constexpr const char* kSelect = "SELECT f.id, f.name, f.mtime, f.size, f.is_dir FROM files f ";

    std::string trimmed(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
      }
      return std::string(text);
    }

    // LIKE wildcards in the query itself; such a query cannot use the trigram index.
    bool hasWildcard(std::string_view text) { return text.find_first_of("%_") != std::string_view::npos; }

    std::string escapeLike(std::string_view text) {
      std::string out;
      for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
          out += '\\';
        }
        out += c;
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
        || sqlite3_exec(m_db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
      std::fprintf(stderr, "andromeda-index: %s: %s\n", file.c_str(), m_db != nullptr ? sqlite3_errmsg(m_db) : "open failed");
      sqlite3_close(m_db);
      m_db = nullptr;
      return;
    }
    sqlite3_prepare_v3(
        m_db, "INSERT INTO files(id, parent, name, mtime, size, is_dir) VALUES (?1, ?2, ?3, ?4, ?5, ?6)", -1,
        SQLITE_PREPARE_PERSISTENT, &m_insert, nullptr
    );
    sqlite3_prepare_v3(m_db, "INSERT INTO names(rowid, name) VALUES (?1, ?2)", -1, SQLITE_PREPARE_PERSISTENT, &m_insertName, nullptr);
    sqlite3_prepare_v3(m_db, "SELECT parent, name FROM files WHERE id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &m_parentOf, nullptr);
    sqlite3_prepare_v3(m_db, "SELECT id FROM files WHERE parent = ?1 AND name = ?2", -1, SQLITE_PREPARE_PERSISTENT, &m_child, nullptr);
  }

  IndexDb::~IndexDb() {
    sqlite3_finalize(m_insert);
    sqlite3_finalize(m_insertName);
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
    sqlite3_bind_int64(m_insertName, 1, id);
    sqlite3_bind_text(m_insertName, 2, name.data(), static_cast<int>(name.size()), SQLITE_STATIC);
    sqlite3_step(m_insertName);
    sqlite3_reset(m_insertName);
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

  void IndexDb::removeTree(std::string_view path) {
    const auto id = idOf(path);
    if (!id) {
      return;
    }
    // External-content FTS5 needs the old value to delete a row from the index.
    sqlite3_exec(m_db, "CREATE TEMP TABLE IF NOT EXISTS doomed(id INTEGER PRIMARY KEY); DELETE FROM doomed;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        m_db,
        "INSERT INTO doomed WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL SELECT f.id FROM files f JOIN sub ON f.parent = sub.id) "
        "SELECT id FROM sub",
        -1, &stmt, nullptr
    );
    sqlite3_bind_int64(stmt, 1, *id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_exec(
        m_db,
        "INSERT INTO names(names, rowid, name) SELECT 'delete', f.id, f.name FROM files f JOIN doomed d ON d.id = f.id;"
        "DELETE FROM files WHERE id IN (SELECT id FROM doomed);",
        nullptr, nullptr, nullptr
    );
  }

  std::vector<FileRow> IndexDb::queryNames(std::string_view text, int limit) const {
    const std::string needle = trimmed(text);
    if (needle.empty() || m_db == nullptr) {
      return {};
    }
    // Ranks: 0 exact name, 1 name prefix, 2 substring; newest first within a rank.
    constexpr const char* kOrder =
        "ORDER BY (f.name = ?2 COLLATE NOCASE) DESC, (f.name LIKE ?3 ESCAPE '\\') DESC, f.mtime DESC LIMIT ?4";
    std::string sql = kSelect;
    std::string match;
    if (needle.size() < 3) {
      return {};
    }
    if (hasWildcard(needle)) {
      sql += "WHERE f.name LIKE ?1 ESCAPE '\\' ";
      match = "%" + escapeLike(needle) + "%";
    } else {
      // No ESCAPE clause: SQLite uses the trigram index only for a plain LIKE.
      sql += "JOIN names n ON n.rowid = f.id WHERE n.name LIKE ?1 ";
      match = "%" + needle + "%";
    }
    sql += kOrder;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    const std::string prefix = escapeLike(needle) + "%";
    sqlite3_bind_text(stmt, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, needle.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, limit);
    std::vector<FileRow> rows;
    std::vector<std::int64_t> ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      ids.push_back(sqlite3_column_int64(stmt, 0));
      rows.push_back({
          .path = {},
          .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
          .size = sqlite3_column_int64(stmt, 3),
          .isDir = sqlite3_column_int(stmt, 4) != 0,
      });
    }
    sqlite3_finalize(stmt);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      rows[i].path = pathOf(ids[i]);
    }
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

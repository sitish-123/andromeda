#include "index_db.h"

#include <sqlite3.h>

#include <cctype>
#include <cstdio>

namespace andromeda::index {

  namespace {
    constexpr const char* kSchema = R"sql(
      PRAGMA journal_mode = WAL;
      PRAGMA synchronous = NORMAL;
      CREATE TABLE IF NOT EXISTS files (
        id INTEGER PRIMARY KEY,
        path TEXT NOT NULL UNIQUE,
        name TEXT NOT NULL COLLATE NOCASE,
        mtime INTEGER NOT NULL,
        size INTEGER NOT NULL,
        is_dir INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS files_name ON files(name COLLATE NOCASE);
      CREATE VIRTUAL TABLE IF NOT EXISTS names USING fts5(name, content='files', content_rowid='id',
                                                          tokenize='trigram case_sensitive 0');
      CREATE TRIGGER IF NOT EXISTS files_ai AFTER INSERT ON files BEGIN
        INSERT INTO names(rowid, name) VALUES (new.id, new.name);
      END;
      CREATE TRIGGER IF NOT EXISTS files_ad AFTER DELETE ON files BEGIN
        INSERT INTO names(names, rowid, name) VALUES ('delete', old.id, old.name);
      END;
      CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE OF name ON files BEGIN
        INSERT INTO names(names, rowid, name) VALUES ('delete', old.id, old.name);
        INSERT INTO names(rowid, name) VALUES (new.id, new.name);
      END;
    )sql";

    // An FTS5 phrase: the text in double quotes, inner quotes doubled.
    std::string ftsPhrase(std::string_view text) {
      std::string out = "\"";
      for (const char c : text) {
        out += c;
        if (c == '"') {
          out += '"';
        }
      }
      return out + "\"";
    }

    // A LIKE prefix pattern with % and _ escaped by backslash.
    std::string likePrefix(std::string_view text) {
      std::string out;
      for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      return out + "%";
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

    std::vector<FileRow> readRows(sqlite3_stmt* stmt) {
      std::vector<FileRow> rows;
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            .path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
            .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
            .mtime = sqlite3_column_int64(stmt, 2),
            .size = sqlite3_column_int64(stmt, 3),
            .isDir = sqlite3_column_int(stmt, 4) != 0,
        });
      }
      sqlite3_finalize(stmt);
      return rows;
    }
  } // namespace

  IndexDb::IndexDb(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (sqlite3_open(file.c_str(), &m_db) != SQLITE_OK || sqlite3_exec(m_db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
      std::fprintf(stderr, "andromeda-index: %s: %s\n", file.c_str(), m_db != nullptr ? sqlite3_errmsg(m_db) : "open failed");
      sqlite3_close(m_db);
      m_db = nullptr;
      return;
    }
    sqlite3_prepare_v3(
        m_db,
        "INSERT INTO files(path, name, mtime, size, is_dir) VALUES (?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(path) DO UPDATE SET mtime = excluded.mtime, size = excluded.size, is_dir = excluded.is_dir "
        "WHERE mtime != excluded.mtime OR size != excluded.size",
        -1, SQLITE_PREPARE_PERSISTENT, &m_upsert, nullptr
    );
  }

  IndexDb::~IndexDb() {
    sqlite3_finalize(m_upsert);
    sqlite3_close(m_db);
  }

  void IndexDb::begin() { sqlite3_exec(m_db, "BEGIN", nullptr, nullptr, nullptr); }
  void IndexDb::commit() { sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr); }

  void IndexDb::upsert(const FileRow& row) {
    sqlite3_bind_text(m_upsert, 1, row.path.data(), static_cast<int>(row.path.size()), SQLITE_STATIC);
    sqlite3_bind_text(m_upsert, 2, row.name.data(), static_cast<int>(row.name.size()), SQLITE_STATIC);
    sqlite3_bind_int64(m_upsert, 3, row.mtime);
    sqlite3_bind_int64(m_upsert, 4, row.size);
    sqlite3_bind_int(m_upsert, 5, row.isDir ? 1 : 0);
    sqlite3_step(m_upsert);
    sqlite3_reset(m_upsert);
  }

  void IndexDb::removeTree(std::string_view path) {
    sqlite3_stmt* stmt = nullptr;
    // Everything below path sorts in [path + "/", path + "0"): '0' is the byte after '/'.
    sqlite3_prepare_v2(m_db, "DELETE FROM files WHERE path = ?1 OR (path >= ?1 || '/' AND path < ?1 || '0')", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  std::vector<FileRow> IndexDb::queryNames(std::string_view text, int limit) const {
    const std::string needle = trimmed(text);
    if (needle.empty() || m_db == nullptr) {
      return {};
    }
    sqlite3_stmt* stmt = nullptr;
    if (needle.size() < 3) {
      sqlite3_prepare_v2(
          m_db,
          "SELECT path, name, mtime, size, is_dir FROM files WHERE name LIKE ?1 ESCAPE '\\' "
          "ORDER BY name = ?2 COLLATE NOCASE DESC, mtime DESC LIMIT ?3",
          -1, &stmt, nullptr
      );
      const std::string pattern = likePrefix(needle);
      sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_prepare_v2(
          m_db,
          "SELECT f.path, f.name, f.mtime, f.size, f.is_dir FROM names JOIN files f ON f.id = names.rowid "
          "WHERE names MATCH ?1 "
          "ORDER BY (f.name = ?2 COLLATE NOCASE) DESC, (f.name LIKE ?4 ESCAPE '\\') DESC, f.mtime DESC LIMIT ?3",
          -1, &stmt, nullptr
      );
      const std::string phrase = ftsPhrase(needle);
      const std::string pattern = likePrefix(needle);
      sqlite3_bind_text(stmt, 1, phrase.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, pattern.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 2, needle.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    return readRows(stmt);
  }

  std::int64_t IndexDb::count() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM files", -1, &stmt, nullptr);
    const std::int64_t n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
  }

} // namespace andromeda::index

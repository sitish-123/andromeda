#include "starred.h"

#include "core/log.h"

#include <algorithm>
#include <fstream>
#include <glib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("starred");
}

fs::path Starred::storePath() { return fs::path(g_get_user_data_dir()) / "andromeda" / "files" / "starred.json"; }

Starred::Starred() {
  std::ifstream in(storePath());
  const nlohmann::json list = nlohmann::json::parse(in, nullptr, false);
  if (!list.is_array()) {
    return;
  }
  for (const auto& item : list) {
    if (item.is_string()) {
      m_paths.emplace_back(item.get<std::string>());
    }
  }
}

bool Starred::contains(const fs::path& path) const { return std::ranges::find(m_paths, path) != m_paths.end(); }

bool Starred::toggle(const std::vector<fs::path>& paths) {
  const bool star = std::ranges::any_of(paths, [this](const fs::path& p) { return !contains(p); });
  for (const fs::path& p : paths) {
    std::erase(m_paths, p);
    if (star) {
      m_paths.insert(m_paths.begin(), p);
    }
  }
  save();
  kLog.info("{} {} items", star ? "starred" : "unstarred", paths.size());
  return star;
}

void Starred::moved(const fs::path& from, const fs::path& to) {
  bool changed = false;
  for (fs::path& p : m_paths) {
    // The item itself or anything inside a moved folder.
    const auto rel = p.lexically_relative(from);
    if (p == from || (!rel.empty() && *rel.begin() != "..")) {
      p = p == from ? to : to / rel;
      changed = true;
    }
  }
  if (changed) {
    save();
  }
}

void Starred::save() const {
  nlohmann::json list = nlohmann::json::array();
  for (const fs::path& p : m_paths) {
    list.push_back(p.string());
  }
  const fs::path file = storePath();
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  std::ofstream(tmp) << list.dump(1);
  fs::rename(tmp, file, ec);
  if (ec) {
    kLog.warn("save {}: {}", file.string(), ec.message());
  }
}

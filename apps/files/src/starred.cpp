#include "starred.h"

#include "state_file.h"
#include "core/log.h"

#include <algorithm>
#include <fstream>
#include <glib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("starred");
}

fs::path Starred::storePath() const { return fs::path(g_get_user_data_dir()) / "andromeda" / "files" / m_file; }

Starred::Starred(std::string file) : m_file(std::move(file)) {
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

void Starred::moved(const std::vector<std::pair<fs::path, fs::path>>& moves) {
  bool changed = false;
  for (fs::path& p : m_paths) {
    // The deepest move that holds this path wins: the item itself, or the folder it is in.
    const std::pair<fs::path, fs::path>* best = nullptr;
    for (const auto& move : moves) {
      const auto rel = p.lexically_relative(move.first);
      const bool inside = p == move.first || (!rel.empty() && *rel.begin() != "..");
      if (inside && (best == nullptr || move.first.native().size() > best->first.native().size())) {
        best = &move;
      }
    }
    if (best != nullptr) {
      p = p == best->first ? best->second : best->second / p.lexically_relative(best->first);
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
  writeStateFile(storePath(), list.dump(1));
}

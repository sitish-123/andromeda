#include "tags.h"

#include "state_file.h"
#include "core/deferred_call.h"
#include "core/files/file_info.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mntent.h>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>
#include <sys/xattr.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kXattr = "user.xdg.tags";
constexpr std::uint32_t kUnlistedRgb = 0x8e8e93;
constexpr std::int64_t kCacheNs = std::int64_t{10} * 60 * 1'000'000'000;

std::int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string readRaw(const fs::path& path) {
  std::string buf(4096, '\0');
  for (;;) {
    const ssize_t n = ::lgetxattr(path.c_str(), kXattr, buf.data(), buf.size());
    if (n >= 0) {
      buf.resize(static_cast<std::size_t>(n));
      return buf;
    }
    if (errno != ERANGE || buf.size() >= 64 * 1024) {
      return {};
    }
    buf.resize(buf.size() * 4);
  }
}

// Hidden folders and package trees hold no user tags and are most of a home walk.
bool skipFolder(const std::string& name) {
  return name.starts_with('.') || name == "node_modules" || name == "__pycache__" || name == "target";
}

std::string hexOf(std::uint32_t rgb) {
  char out[8];
  std::snprintf(out, sizeof out, "#%06x", rgb & 0xffffffU);
  return out;
}

} // namespace

std::vector<std::string> parseTagList(std::string_view raw) {
  std::vector<std::string> out;
  while (!raw.empty()) {
    const auto comma = raw.find(',');
    std::string_view tag = raw.substr(0, comma);
    raw = comma == std::string_view::npos ? std::string_view{} : raw.substr(comma + 1);
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) {
      tag.remove_prefix(1);
    }
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) {
      tag.remove_suffix(1);
    }
    if (!tag.empty() && std::ranges::find(out, tag) == out.end()) {
      out.emplace_back(tag);
    }
  }
  return out;
}

std::vector<std::string> readTags(const fs::path& path) { return parseTagList(readRaw(path)); }

fs::path tagConfigPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  const fs::path base = xdg != nullptr && *xdg != '\0' ? fs::path(xdg) : fs::path(home != nullptr ? home : "") / ".config";
  return base / "andromeda" / "files" / "tags.json";
}

std::vector<TagDef> loadTagConfig() {
  std::ifstream in(tagConfigPath());
  if (in) {
    const auto doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_array()) {
      std::vector<TagDef> out;
      for (const auto& t : doc) {
        const std::string name = t.is_object() ? t.value("name", "") : "";
        const std::string color = t.is_object() ? t.value("color", "") : "";
        if (name.empty() || name.find(',') != std::string::npos) {
          continue;
        }
        std::uint32_t rgb = kUnlistedRgb;
        if (color.size() == 7 && color[0] == '#') {
          char* end = nullptr;
          const unsigned long v = std::strtoul(color.c_str() + 1, &end, 16);
          if (end == color.c_str() + 7) {
            rgb = static_cast<std::uint32_t>(v);
          }
        }
        out.push_back({.name = name, .rgb = rgb, .sidebar = t.value("sidebar", true)});
      }
      return out;
    }
  }
  return {{.name = "Urgent", .rgb = 0xd93025},   {.name = "Personal", .rgb = 0xe46b2c},
          {.name = "Ideas", .rgb = 0xe6c229},    {.name = "Done", .rgb = 0x1a7f37},
          {.name = "Work", .rgb = 0x5147e5},     {.name = "Family", .rgb = 0x8a4fd8}};
}

bool saveTagConfig(const std::vector<TagDef>& tags) {
  nlohmann::json doc = nlohmann::json::array();
  for (const auto& t : tags) {
    doc.push_back({{"name", t.name}, {"color", hexOf(t.rgb)}, {"sidebar", t.sidebar}});
  }
  return writeStateFile(tagConfigPath(), doc.dump(2) + '\n');
}

TagIndex::TagIndex(std::function<void()> changed, fs::path root)
    : m_changed(std::move(changed)), m_root(std::move(root)) {
  if (m_root.empty()) {
    const char* home = std::getenv("HOME");
    m_root = home != nullptr ? fs::path(home) : fs::path("/");
  }
}

TagIndex::~TagIndex() { m_stop->store(true); }

void TagIndex::refresh(bool fresh) {
  if (m_walking) {
    m_walkAgain = m_walkAgain || fresh; // a fresh request during a walk runs once that walk lands
    return;
  }
  if (!fresh && m_ready && nowNs() - m_walkedAt < kCacheNs) {
    return;
  }
  walk();
}

void TagIndex::walkThen(std::function<void()> then) {
  m_afterWalk.emplace_back(m_walks + 1, std::move(then));
  refresh(true);
}

std::vector<fs::path> TagIndex::paths(const std::string& tag) const {
  const auto it = m_byTag.find(tag);
  return it == m_byTag.end() ? std::vector<fs::path>{} : it->second;
}

std::size_t TagIndex::count(const std::string& tag) const {
  const auto it = m_byTag.find(tag);
  return it == m_byTag.end() ? 0 : it->second.size();
}

std::vector<std::string> TagIndex::unlisted(const std::vector<TagDef>& config) const {
  std::vector<std::string> out;
  for (const auto& [name, list] : m_byTag) {
    if (std::ranges::none_of(config, [&](const TagDef& t) { return t.name == name; })) {
      out.push_back(name);
    }
  }
  return out;
}

void TagIndex::walk() {
  m_walking = true;
  const std::uint64_t walkNumber = ++m_walks;
  std::thread([weak = weak_from_this(), root = m_root, stop = m_stop, walkNumber] {
    std::map<std::string, std::vector<fs::path>> found;
    std::error_code ec;
    // One file system only: a network or removable mount under home could hang the walk or take hours. Mount points
    // come from the mount table, so the walk never touches them (a stat on a hung mount blocks too).
    std::set<std::string> mounts;
    if (FILE* table = ::setmntent("/proc/self/mounts", "r")) {
      while (const mntent* m = ::getmntent(table)) {
        const std::string dir = m->mnt_dir;
        if (dir != root.string() && dir.starts_with(root.string() + "/")) {
          mounts.insert(dir);
        }
      }
      ::endmntent(table);
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
      if (stop->load(std::memory_order_relaxed)) {
        return;
      }
      std::error_code typeEc;
      if (it->is_directory(typeEc) && !it->is_symlink(typeEc)) {
        if (mounts.contains(it->path().string()) || skipFolder(it->path().filename().string())) {
          it.disable_recursion_pending();
          continue;
        }
      }
      for (auto& tag : readTags(it->path())) {
        found[std::move(tag)].push_back(it->path());
      }
    }
    DeferredCall::callLater([weak, found = std::move(found), walkNumber]() mutable {
      if (auto self = weak.lock()) {
        self->m_byTag = std::move(found);
        self->m_byPathStale = true;
        self->m_walkedAt = nowNs();
        self->m_walking = false;
        self->m_ready = true;
        // Paths re-read during the walk may be newer than what the walk saw.
        const auto during = std::exchange(self->m_rereadDuringWalk, {});
        if (!during.empty()) {
          self->reread(during);
        } else if (self->m_changed) {
          self->m_changed();
        }
        std::vector<std::function<void()>> due;
        std::erase_if(self->m_afterWalk, [&](auto& entry) {
          if (entry.first > walkNumber) {
            return false;
          }
          due.push_back(std::move(entry.second));
          return true;
        });
        for (auto& then : due) {
          then();
        }
        if (std::exchange(self->m_walkAgain, false)) {
          self->walk();
        }
      }
    });
  }).detach();
}

void TagIndex::reread(const std::vector<fs::path>& paths) {
  if (m_walking) {
    m_rereadDuringWalk.insert(m_rereadDuringWalk.end(), paths.begin(), paths.end());
  }
  for (auto& [tag, list] : m_byTag) {
    std::erase_if(list, [&](const fs::path& p) { return std::ranges::find(paths, p) != paths.end(); });
  }
  for (const auto& p : paths) {
    for (auto& tag : readTags(p)) {
      m_byTag[std::move(tag)].push_back(p);
    }
  }
  std::erase_if(m_byTag, [](const auto& kv) { return kv.second.empty(); });
  m_byPathStale = true;
  if (m_changed) {
    m_changed();
  }
}

const std::vector<std::string>& TagIndex::tagsOf(const fs::path& path) const {
  if (m_byPathStale) {
    m_byPath.clear();
    for (const auto& [tag, list] : m_byTag) {
      for (const auto& p : list) {
        m_byPath[p.string()].push_back(tag);
      }
    }
    m_byPathStale = false;
  }
  static const std::vector<std::string> kNone;
  const auto it = m_byPath.find(path.string());
  return it == m_byPath.end() ? kNone : it->second;
}

std::vector<SidebarTag> TagIndex::sidebar() const {
  std::vector<SidebarTag> out;
  const auto config = loadTagConfig();
  auto countOf = [&](const std::string& name) {
    const auto it = m_byTag.find(name);
    return it == m_byTag.end() ? std::size_t{0} : it->second.size();
  };
  for (const auto& t : config) {
    if (t.sidebar) {
      out.push_back({.name = t.name, .rgb = t.rgb, .count = countOf(t.name)});
    }
  }
  for (const auto& [name, list] : m_byTag) {
    if (std::ranges::none_of(config, [&](const TagDef& t) { return t.name == name; })) {
      out.push_back({.name = name, .rgb = kUnlistedRgb, .count = list.size()});
    }
  }
  return out;
}

FileBrowser::Listing TagIndex::listing(const std::string& tag) const {
  FileBrowser::Listing listing{.id = "tag:" + tag, .title = tag, .rows = {}, .activatable = true};
  if (const auto it = m_byTag.find(tag); it != m_byTag.end()) {
    for (const auto& path : it->second) {
      if (auto info = statFile(path)) {
        info->name = path.string(); // absolute: the row opens as a path
        listing.rows.push_back(std::make_shared<const FileInfo>(std::move(*info)));
      }
    }
  }
  std::ranges::sort(listing.rows, [](const auto& a, const auto& b) { return a->mtimeNs > b->mtimeNs; });
  return listing;
}

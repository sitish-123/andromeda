#include "folder_compare.h"

#include <sys/stat.h>

namespace fs = std::filesystem;

namespace folder_compare {

  namespace {
    struct Entry {
      bool dir = false;
      std::int64_t size = 0;
      std::int64_t mtimeNs = 0;
    };

    // FAT and SMB keep 2 s steps; a copy there must still count as the same.
    constexpr std::int64_t kSlackNs = std::int64_t{2'000'000'000};

    struct Walk {
      fs::path left;
      fs::path right;
      bool hidden = false;
      std::size_t budget = kEntryLimit;
      Result* result = nullptr;
    };

    std::map<std::string, Entry> scan(const fs::path& folder, Walk& walk) {
      std::map<std::string, Entry> out;
      std::error_code ec;
      for (const auto& item : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
        if (walk.budget == 0) {
          walk.result->truncated = true;
          break;
        }
        --walk.budget;
        std::string name = item.path().filename().string();
        if (!walk.hidden && name.starts_with('.')) {
          continue;
        }
        struct stat st{};
        if (::lstat(item.path().c_str(), &st) != 0) {
          continue;
        }
        out[std::move(name)] = {.dir = S_ISDIR(st.st_mode), .size = st.st_size,
                                .mtimeNs = std::int64_t{st.st_mtim.tv_sec} * 1'000'000'000 + st.st_mtim.tv_nsec};
      }
      return out;
    }

    struct Sides {
      bool leftNewer = false;
      bool rightNewer = false;
    };

    // One folder level below both roots; adds what each side must send and says which side has newer content.
    Sides diff(const fs::path& rel, Walk& walk) {
      Sides sides;
      const auto a = scan(walk.left / rel, walk);
      const auto b = scan(walk.right / rel, walk);
      for (const auto& [name, l] : a) {
        const auto it = b.find(name);
        if (it == b.end()) {
          walk.result->sendLeft.push_back(rel / name);
          sides.leftNewer = true;
          continue;
        }
        const Entry& r = it->second;
        if (l.dir && r.dir) {
          const Sides below = diff(rel / name, walk);
          sides.leftNewer = sides.leftNewer || below.leftNewer;
          sides.rightNewer = sides.rightNewer || below.rightNewer;
          continue;
        }
        if (l.dir != r.dir) { // a file against a folder: no sync can settle it, both are changed
          sides.leftNewer = sides.rightNewer = true;
          continue;
        }
        const std::int64_t delta = l.mtimeNs - r.mtimeNs;
        if (l.size == r.size && delta <= kSlackNs && delta >= -kSlackNs) {
          continue;
        }
        if (delta >= 0) {
          walk.result->sendLeft.push_back(rel / name);
          sides.leftNewer = true;
        } else {
          walk.result->sendRight.push_back(rel / name);
          sides.rightNewer = true;
        }
      }
      for (const auto& [name, r] : b) {
        if (!a.contains(name)) {
          walk.result->sendRight.push_back(rel / name);
          sides.rightNewer = true;
        }
      }
      return sides;
    }
  } // namespace

  Result compare(const fs::path& left, const fs::path& right, bool hidden) {
    Result r;
    Walk walk{.left = left, .right = right, .hidden = hidden, .budget = kEntryLimit, .result = &r};
    const auto a = scan(left, walk);
    const auto b = scan(right, walk);
    for (const auto& [name, l] : a) {
      const auto it = b.find(name);
      if (it == b.end()) {
        r.left[name] = Mark::OnlyLeft;
        r.sendLeft.emplace_back(name);
        ++r.onlyLeft;
        continue;
      }
      const Entry& o = it->second;
      Sides sides;
      if (l.dir && o.dir) {
        sides = diff(name, walk);
      } else if (l.dir != o.dir) {
        sides = {.leftNewer = true, .rightNewer = true};
      } else {
        const std::int64_t delta = l.mtimeNs - o.mtimeNs;
        if (l.size != o.size || delta > kSlackNs || delta < -kSlackNs) {
          // Same time but another size: the left copy counts as the newer one, as the sync sends it.
          (delta >= 0 ? sides.leftNewer : sides.rightNewer) = true;
          (delta >= 0 ? r.sendLeft : r.sendRight).emplace_back(name);
        }
      }
      if (!sides.leftNewer && !sides.rightNewer) {
        r.left[name] = r.right[name] = Mark::Same;
        ++r.same;
        continue;
      }
      r.left[name] = sides.leftNewer ? Mark::Newer : Mark::Older;
      r.right[name] = sides.rightNewer ? Mark::Newer : Mark::Older;
      ++r.differ;
    }
    for (const auto& [name, e] : b) {
      if (!a.contains(name)) {
        r.right[name] = Mark::OnlyRight;
        r.sendRight.emplace_back(name);
        ++r.onlyRight;
      }
    }
    return r;
  }

} // namespace folder_compare

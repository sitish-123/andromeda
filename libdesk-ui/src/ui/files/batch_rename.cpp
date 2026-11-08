#include "ui/files/batch_rename.h"
#include "util/safe_regex.h"

#include <algorithm>
#include <format>
#include <glib.h>
#include <map>

namespace {
  std::pair<std::string, std::string> splitExtension(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
      return {name, ""};
    }
    return {name.substr(0, dot), name.substr(dot)};
  }

  std::string glibString(gchar* s) {
    std::string out = s != nullptr ? s : "";
    g_free(s);
    return out;
  }

  std::string titleCase(const std::string& s) {
    std::string out;
    bool wordStart = true;
    for (const gchar* p = s.c_str(); *p != '\0'; p = g_utf8_next_char(p)) {
      gunichar c = g_utf8_get_char(p);
      c = wordStart ? g_unichar_totitle(c) : g_unichar_tolower(c);
      char buf[6];
      out.append(buf, static_cast<std::size_t>(g_unichar_to_utf8(c, buf)));
      wordStart = g_unichar_isspace(c) || c == '-' || c == '_' || c == '.';
    }
    return out;
  }

  std::string replaceAll(const std::string& s, const std::string& find, const std::string& with, bool matchCase) {
    if (find.empty()) {
      return s;
    }
    if (matchCase) {
      std::string out;
      std::size_t pos = 0;
      for (std::size_t hit; (hit = s.find(find, pos)) != std::string::npos; pos = hit + find.size()) {
        out += s.substr(pos, hit - pos) + with;
      }
      return out + s.substr(pos);
    }
    // Case-insensitive: compare casefolded text; the folding keeps byte lengths for ASCII only, so match per
    // character on the original string.
    const std::string needle = glibString(g_utf8_casefold(find.c_str(), -1));
    std::string out;
    const gchar* p = s.c_str();
    const gchar* end = p + s.size();
    while (p < end) {
      // Try a match starting here: take as many characters as the needle has.
      const glong needleChars = g_utf8_strlen(find.c_str(), -1);
      const gchar* q = p;
      for (glong i = 0; i < needleChars && q < end; ++i) {
        q = g_utf8_next_char(q);
      }
      const std::string candidate(p, static_cast<std::size_t>(q - p));
      if (glibString(g_utf8_casefold(candidate.c_str(), -1)) == needle) {
        out += with;
        p = q;
      } else {
        const gchar* next = g_utf8_next_char(p);
        out.append(p, static_cast<std::size_t>(next - p));
        p = next;
      }
    }
    return out;
  }

  std::string isoDate(std::chrono::system_clock::time_point t) {
    const auto day = std::chrono::floor<std::chrono::days>(t);
    return std::format("{:%Y-%m-%d}", std::chrono::year_month_day(day));
  }

  std::string formatName(const BatchRenameRule& rule, const std::string& stem, const BatchRenameItem& item, int n) {
    std::string out;
    const std::string& f = rule.format;
    for (std::size_t i = 0; i < f.size();) {
      if (f[i] == '{') {
        const std::size_t close = f.find('}', i);
        if (close != std::string::npos) {
          const std::string token = f.substr(i + 1, close - i - 1);
          if (token == "name") {
            out += stem;
          } else if (token == "n") {
            out += std::format("{:0{}}", n, std::max(1, rule.digits));
          } else if (token == "date") {
            out += isoDate(item.modified);
          } else if (token == "taken") {
            out += isoDate(item.taken.value_or(item.modified));
          } else {
            out += f.substr(i, close - i + 1);
          }
          i = close + 1;
          continue;
        }
      }
      out += f[i++];
    }
    return out;
  }
} // namespace

std::vector<BatchRenameResult> applyBatchRename(const BatchRenameRule& rule, const std::vector<BatchRenameItem>& items,
                                                const std::set<std::string>& existing) {
  std::vector<BatchRenameResult> results(items.size());
  std::optional<SafeRegex> pattern;
  std::string patternError;
  if (rule.mode == BatchRenameRule::Mode::Regex) {
    pattern = SafeRegex::compile(rule.find, false);
    if (!pattern) {
      patternError = "The pattern is not valid.";
    }
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    const std::string& old = items[i].name;
    auto [stem, ext] = rule.keepExtension ? splitExtension(old) : std::pair{old, std::string()};
    std::string next;
    switch (rule.mode) {
    case BatchRenameRule::Mode::Replace:
      next = replaceAll(stem, rule.find, rule.replacement, rule.matchCase);
      break;
    case BatchRenameRule::Mode::Add:
      next = rule.addBefore ? rule.text + stem : stem + rule.text;
      break;
    case BatchRenameRule::Mode::Format:
      next = formatName(rule, stem, items[i], rule.start + static_cast<int>(i));
      break;
    case BatchRenameRule::Mode::Case:
      next = rule.caseMode == BatchRenameRule::CaseMode::Lower   ? glibString(g_utf8_strdown(stem.c_str(), -1))
          : rule.caseMode == BatchRenameRule::CaseMode::Upper    ? glibString(g_utf8_strup(stem.c_str(), -1))
                                                                 : titleCase(stem);
      break;
    case BatchRenameRule::Mode::Regex:
      next = pattern.has_value() ? pattern->replace(stem, rule.replacement) : stem;
      break;
    }
    BatchRenameResult& r = results[i];
    r.name = next + ext;
    if (!patternError.empty()) {
      r.error = patternError;
    } else if (pattern.has_value() && pattern->exhausted()) {
      r.error = "The pattern takes too long. Make it simpler.";
    } else if (next.empty() || r.name == "." || r.name == "..") {
      r.error = "The name would be empty.";
    } else if (r.name.find('/') != std::string::npos || r.name.find('\0') != std::string::npos) {
      r.error = "A name can’t contain “/”.";
    } else if (r.name.size() > 255) {
      r.error = "The name is too long.";
    }
  }
  std::set<std::string> moving;
  for (const BatchRenameItem& item : items) {
    moving.insert(item.name);
  }
  std::map<std::string, int> uses;
  for (const BatchRenameResult& r : results) {
    ++uses[r.name];
  }
  for (std::size_t i = 0; i < results.size(); ++i) {
    BatchRenameResult& r = results[i];
    const bool takenByStayer = r.name != items[i].name && existing.contains(r.name) && !moving.contains(r.name);
    r.conflict = uses[r.name] > 1 || takenByStayer;
  }
  return results;
}

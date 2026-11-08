#include "shell/dock/dock_drop.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_instance.h"
#include "system/desktop_entry.h"

#include <sstream>
#include <vector>

#include <glib.h>

namespace shell::dock {

  namespace {
    std::vector<std::string> uris(const std::string& uriList) {
      std::vector<std::string> out;
      std::istringstream lines(uriList);
      for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (!line.empty() && line.front() != '#') {
          out.push_back(line);
        }
      }
      return out;
    }

    std::string pathOf(const std::string& uri) {
      gchar* path = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
      std::string out = path != nullptr ? path : "";
      g_free(path);
      return out;
    }

    // One value for an Exec line: single quotes for the tokenizer, "\\" for the desktop-string decode and "%%" for
    // the field-code pass that run on it after us (desktop_entry_launch).
    std::string quoted(const std::string& value) {
      std::string out = "'";
      for (const char c : value) {
        if (c == '\'') {
          out += "'\"'\"'";
        } else if (c == '\\') {
          out += "\\\\";
        } else if (c == '%') {
          out += "%%";
        } else {
          out += c;
        }
      }
      return out + "'";
    }

    // The same value inside a double-quoted Exec argument (Exec=sh -c "app %f"): the spec escapes ", `, $ and \
    // with a backslash there; each backslash is doubled again for the desktop-string decode.
    std::string doubleQuoted(const std::string& value) {
      std::string out;
      for (const char c : value) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') {
          out += "\\\\";
          out += c == '\\' ? std::string("\\\\") : std::string(1, c);
        } else if (c == '%') {
          out += "%%";
        } else {
          out += c;
        }
      }
      return out;
    }
  } // namespace

  std::optional<std::size_t> itemAt(const DockInstance& instance, float x, float y) {
    for (std::size_t i = 0; i < instance.items.size(); ++i) {
      const InputArea* area = instance.items[i].area;
      if (area == nullptr) {
        continue;
      }
      float ax = 0.0F;
      float ay = 0.0F;
      Node::absolutePosition(area, ax, ay);
      if (x >= ax && x < ax + area->width() && y >= ay && y < ay + area->height()) {
        return i;
      }
    }
    return std::nullopt;
  }

  bool takesFiles(const DesktopEntry& entry) {
    for (const char* code : {"%f", "%F", "%u", "%U"}) {
      if (entry.exec.find(code) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> execLines(const std::string& exec, const std::string& uriList) {
    const std::vector<std::string> all = uris(uriList);
    // %f/%F take local files only; %u/%U also take other URIs.
    std::vector<std::string> files;
    for (const std::string& uri : all) {
      if (const std::string path = pathOf(uri); !path.empty()) {
        files.push_back(path);
      }
    }
    bool single = false;
    bool wantsUris = false;
    for (std::size_t i = 0; i + 1 < exec.size(); ++i) {
      if (exec[i] == '%') {
        const char code = exec[i + 1];
        single = single || code == 'f' || code == 'u';
        wantsUris = wantsUris || code == 'u' || code == 'U';
        ++i;
      }
    }
    const std::vector<std::string>& items = wantsUris ? all : files;
    if (items.empty()) {
      return {};
    }
    // Codes are replaced where they stand, also inside a word (Exec=app --open=%f); "%%" stays for the later pass.
    const auto expand = [&](const std::vector<std::string>& batch) {
      const auto joined = [&](bool inDouble) {
        std::string text;
        for (const std::string& item : batch) {
          text += (text.empty() ? "" : " ") + (inDouble ? doubleQuoted(item) : quoted(item));
        }
        return text;
      };
      std::string out;
      bool inDouble = false;
      for (std::size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == '\\' && i + 1 < exec.size()) {
          out += exec[i];
          out += exec[++i];
          continue;
        }
        if (exec[i] == '"') {
          inDouble = !inDouble;
        }
        if (exec[i] == '%' && i + 1 < exec.size()) {
          const char code = exec[i + 1];
          if (code == 'f' || code == 'F' || code == 'u' || code == 'U') {
            out += joined(inDouble);
            ++i;
            continue;
          }
          out += exec[i];
          out += code;
          ++i;
          continue;
        }
        out += exec[i];
      }
      return out;
    };
    std::vector<std::string> out;
    if (single) {
      for (const std::string& item : items) {
        out.push_back(expand({item}));
      }
    } else {
      out.push_back(expand(items));
    }
    return out;
  }

} // namespace shell::dock

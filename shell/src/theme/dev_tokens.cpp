#include "theme/dev_tokens.h"

#include "core/log.h"
#include "core/timer_manager.h"
#include "design_tokens.h"
#include "theme/builtin_palettes.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <map>
#include <optional>
#include <string_view>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <variant>

#ifdef TOKENS_RUNTIME
namespace noctalia::theme::dev_tokens {

  namespace {

    constexpr Logger kLog("dev-tokens");

    using Value = std::variant<double, std::int64_t, bool, std::string>;

    const Value* find(const std::map<std::string, Value>& raw, const std::string& path) {
      const auto it = raw.find(path);
      return it == raw.end() ? nullptr : &it->second;
    }

    void flatten(const toml::table& table, const std::string& prefix, std::map<std::string, Value>& out) {
      for (const auto& [key, node] : table) {
        const std::string path = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const auto* sub = node.as_table()) {
          flatten(*sub, path, out);
        } else if (const auto* f = node.as_floating_point()) {
          out[path] = f->get();
        } else if (const auto* i = node.as_integer()) {
          out[path] = i->get();
        } else if (const auto* b = node.as_boolean()) {
          out[path] = b->get();
        } else if (const auto* s = node.as_string()) {
          out[path] = s->get();
        } else {
          out[path] = std::string("?unsupported");
        }
      }
    }

    // "= a.b - c.d + e.f": the same expressions gen-design-tokens.py resolves at build time.
    std::optional<double> evaluate(const std::map<std::string, Value>& raw, std::string_view expr, int depth) {
      if (depth > 16) {
        return std::nullopt;
      }
      expr.remove_prefix(1);
      double total = 0.0;
      double sign = 1.0;
      std::size_t pos = 0;
      bool first = true;
      while (pos <= expr.size()) {
        while (pos < expr.size() && expr[pos] == ' ') {
          ++pos;
        }
        const auto end = expr.find_first_of("+- ", pos);
        const std::string ref(expr.substr(pos, end == std::string_view::npos ? end : end - pos));
        if (ref.empty()) {
          return std::nullopt;
        }
        const Value* v = find(raw, ref);
        if (v == nullptr) {
          return std::nullopt;
        }
        double n = 0.0;
        if (const auto* d = std::get_if<double>(v)) {
          n = *d;
        } else if (const auto* i = std::get_if<std::int64_t>(v)) {
          n = static_cast<double>(*i);
        } else if (const auto* s = std::get_if<std::string>(v); s != nullptr && s->starts_with("=")) {
          const auto inner = evaluate(raw, *s, depth + 1);
          if (!inner) {
            return std::nullopt;
          }
          n = *inner;
        } else {
          return std::nullopt;
        }
        total += sign * n;
        first = false;
        pos = end == std::string_view::npos ? expr.size() + 1 : end;
        while (pos < expr.size() && expr[pos] == ' ') {
          ++pos;
        }
        if (pos >= expr.size()) {
          break;
        }
        if (expr[pos] != '+' && expr[pos] != '-') {
          return std::nullopt;
        }
        sign = expr[pos] == '+' ? 1.0 : -1.0;
        ++pos;
      }
      return first ? std::nullopt : std::optional<double>(total);
    }

    std::optional<std::uint32_t> parseColor(const std::string& s) {
      if (s.size() != 9 || s[0] != '#') {
        return std::nullopt;
      }
      std::uint32_t v = 0;
      for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
          return std::nullopt;
        }
        v = (v << 4) | static_cast<std::uint32_t>(d);
      }
      return v;
    }

  } // namespace

  // Runs on the main loop, which is the only thread that reads Tokens:: except the template worker; that worker
  // reads schemeLightEntries/schemeDarkEntries only after a theme change this reload triggers, not concurrently.
  bool applyFile(const std::filesystem::path& path, std::string& error) {
    toml::table root;
    try {
      root = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
      error = std::string(e.description());
      return false;
    }
    std::map<std::string, Value> raw;
    flatten(root, "", raw);

    std::unordered_map<std::string, const Tokens::Slot*> slots;
    for (const auto& slot : Tokens::slots()) {
      slots[std::string(slot.path)] = &slot;
    }

    struct Pending {
      const Tokens::Slot* slot;
      Value value;
    };
    std::vector<Pending> pending;
    for (const auto& [key, value] : raw) {
      const auto it = slots.find(key);
      if (it == slots.end()) {
        error = "unknown key " + key;
        return false;
      }
      const Tokens::Slot* slot = it->second;
      Value v = value;
      if (const auto* s = std::get_if<std::string>(&v); s != nullptr && s->starts_with("=")) {
        const auto n = evaluate(raw, *s, 0);
        if (!n) {
          error = key + ": bad or circular expression";
          return false;
        }
        v = *n;
      }
      const auto isNumber = std::holds_alternative<double>(v) || std::holds_alternative<std::int64_t>(v);
      const double number = std::holds_alternative<double>(v) ? std::get<double>(v) : static_cast<double>(std::get_if<std::int64_t>(&v) != nullptr ? *std::get_if<std::int64_t>(&v) : 0);
      bool ok = false;
      switch (slot->kind) {
      case 'f':
        ok = isNumber;
        break;
      case 'i':
        ok = std::holds_alternative<std::int64_t>(v) || (isNumber && number == static_cast<std::int64_t>(number));
        break;
      case 'b':
        ok = std::holds_alternative<bool>(v);
        break;
      case 'c':
        if (const auto* s = std::get_if<std::string>(&v)) {
          if (const auto c = parseColor(*s)) {
            v = static_cast<std::int64_t>(*c);
            ok = true;
          }
        }
        break;
      case 's':
        ok = std::holds_alternative<std::string>(v);
        break;
      default:
        break;
      }
      if (!ok) {
        error = key + ": wrong value type";
        return false;
      }
      pending.push_back({slot, std::move(v)});
    }

    // Every slot starts from its built-in value, so a key deleted from the file reverts.
    for (const auto& slot : Tokens::slots()) {
      void* ptr = slot.ptr;
      switch (slot.kind) {
      case 'f':
        *static_cast<float*>(ptr) = static_cast<float>(slot.number);
        break;
      case 'i':
        *static_cast<int*>(ptr) = static_cast<int>(slot.number);
        break;
      case 'b':
        *static_cast<bool*>(ptr) = slot.number != 0.0;
        break;
      case 'c':
        *static_cast<std::uint32_t*>(ptr) = static_cast<std::uint32_t>(slot.number);
        break;
      case 's':
        *static_cast<std::string_view*>(ptr) = slot.text;
        break;
      default:
        break;
      }
    }
    static std::unordered_map<std::string_view, std::string> strings; // string_view values point here, one per slot
    for (auto& p : pending) {
      void* ptr = p.slot->ptr;
      switch (p.slot->kind) {
      case 'f': {
        const auto* d = std::get_if<double>(&p.value);
        *static_cast<float*>(ptr) = d != nullptr ? static_cast<float>(*d) : static_cast<float>(std::get<std::int64_t>(p.value));
        break;
      }
      case 'i': {
        const auto* d = std::get_if<double>(&p.value);
        *static_cast<int*>(ptr) = d != nullptr ? static_cast<int>(*d) : static_cast<int>(std::get<std::int64_t>(p.value));
        break;
      }
      case 'b':
        *static_cast<bool*>(ptr) = std::get<bool>(p.value);
        break;
      case 'c':
        *static_cast<std::uint32_t*>(ptr) = static_cast<std::uint32_t>(std::get<std::int64_t>(p.value));
        break;
      case 's':
        *static_cast<std::string_view*>(ptr) = strings[p.slot->path] = std::get<std::string>(p.value);
        break;
      default:
        break;
      }
    }
    Tokens::refreshEntries();
    refreshBuiltinPalettes();
    return true;
  }

  std::filesystem::path envPath() {
    const char* v = std::getenv("ANDROMEDA_DEV_TOKENS");
    return v != nullptr ? std::filesystem::path(v) : std::filesystem::path();
  }

  bool applyFromEnv() {
    const auto path = envPath();
    if (path.empty()) {
      return false;
    }
    std::string error;
    if (applyFile(path, error)) {
      kLog.info("loaded design tokens from {}", path.string());
    } else {
      kLog.warn("{}: {}; using built-in tokens", path.string(), error);
    }
    return true;
  }

  void watchEnv(FileWatcher& watcher, std::function<void()> onApplied) {
    auto path = envPath();
    if (path.empty()) {
      return;
    }
    // An editor's atomic rename happens in the directory of the real file, so watch that one.
    std::error_code ec;
    if (auto real = std::filesystem::canonical(path, ec); !ec) {
      path = std::move(real);
    }
    // The watcher drops events inside its own debounce window; reading the file after a quiet period
    // makes the last save always apply.
    auto settle = std::make_shared<Timer>();
    watcher.watch(
        path,
        [path, settle, onApplied = std::move(onApplied)] {
          settle->start(std::chrono::milliseconds(200), [path, onApplied] {
            std::string error;
            if (applyFile(path, error)) {
              kLog.info("reloaded design tokens from {}", path.string());
              onApplied();
            } else {
              kLog.warn("{}: {}; keeping the last good tokens", path.string(), error);
            }
          });
        },
        FileWatcher::WatchTrigger::WriteCompleted
    );
  }

} // namespace noctalia::theme::dev_tokens

#else

namespace noctalia::theme::dev_tokens {

  bool applyFile(const std::filesystem::path&, std::string& error) {
    error = "built without dev_tokens";
    return false;
  }

  std::filesystem::path envPath() { return {}; }
  bool applyFromEnv() { return false; }
  void watchEnv(FileWatcher&, std::function<void()>) {}

} // namespace noctalia::theme::dev_tokens

#endif

#include "picker_portal.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <gio/gio.h>
#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("picker");
  const sdbus::ServiceName kBusName{"org.freedesktop.impl.portal.desktop.andromeda.Files"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.FileChooser";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";

  using Options = std::map<std::string, sdbus::Variant>;
  using Pattern = sdbus::Struct<std::uint32_t, std::string>;
  using FilterStruct = sdbus::Struct<std::string, std::vector<Pattern>>;

  // Portal byte strings carry a trailing NUL.
  std::filesystem::path fromBytes(const std::vector<std::uint8_t>& bytes) {
    std::string out(bytes.begin(), bytes.end());
    while (!out.empty() && out.back() == '\0') {
      out.pop_back();
    }
    return out;
  }

  template <typename T> std::optional<T> option(const Options& options, const char* key) {
    const auto it = options.find(key);
    if (it == options.end()) {
      return std::nullopt;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error& e) {
      kLog.warn("option {} has the wrong type: {}", key, e.what());
      return std::nullopt;
    }
  }

  PickerPortal::Filter toFilter(const FilterStruct& in) {
    PickerPortal::Filter out{.name = std::get<0>(in), .patterns = {}};
    for (const Pattern& p : std::get<1>(in)) {
      out.patterns.emplace_back(std::get<0>(p), std::get<1>(p));
    }
    return out;
  }

  FilterStruct toStruct(const PickerPortal::Filter& in) {
    std::vector<Pattern> patterns;
    for (const auto& [kind, pattern] : in.patterns) {
      patterns.emplace_back(kind, pattern);
    }
    return {in.name, patterns};
  }
} // namespace

struct PickerPortal::Pending {
  Request request;
  std::optional<sdbus::Result<std::uint32_t, Options>> result;
  std::unique_ptr<sdbus::IObject> requestObject;
};

PickerPortal::PickerPortal(SessionBus& bus, RequestHandler onRequest, CloseHandler onClose)
    : m_bus(bus), m_onRequest(std::move(onRequest)), m_onClose(std::move(onClose)) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  const auto handler = [this](Request::Kind kind) {
    return [this, kind](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath handle, std::string appId,
                        std::string parent, std::string title, Options options) {
      auto pending = std::make_unique<Pending>();
      Request& r = pending->request;
      r.kind = kind;
      r.handle = handle;
      r.appId = std::move(appId);
      r.parentWindow = std::move(parent);
      r.title = std::move(title);
      r.acceptLabel = option<std::string>(options, "accept_label").value_or("");
      r.multiple = kind == Request::Kind::Open && option<bool>(options, "multiple").value_or(false);
      r.directory = option<bool>(options, "directory").value_or(false) || kind == Request::Kind::SaveFiles;
      for (const FilterStruct& f : option<std::vector<FilterStruct>>(options, "filters").value_or({})) {
        r.filters.push_back(toFilter(f));
      }
      if (const auto current = option<FilterStruct>(options, "current_filter")) {
        const Filter wanted = toFilter(*current);
        std::size_t i = 0;
        for (; i < r.filters.size(); ++i) {
          if (r.filters[i].name == wanted.name) {
            break;
          }
        }
        if (i == r.filters.size()) {
          r.filters.push_back(wanted);
        }
        r.currentFilter = i;
      }
      r.currentName = option<std::string>(options, "current_name").value_or("");
      r.currentFolder = fromBytes(option<std::vector<std::uint8_t>>(options, "current_folder").value_or({}));
      r.currentFile = fromBytes(option<std::vector<std::uint8_t>>(options, "current_file").value_or({}));
      for (const auto& bytes : option<std::vector<std::vector<std::uint8_t>>>(options, "files").value_or({})) {
        r.files.push_back(fromBytes(bytes));
      }
      if (r.currentName.empty() && !r.currentFile.empty()) {
        r.currentName = r.currentFile.filename().string();
      }
      if (r.currentFolder.empty() && !r.currentFile.empty()) {
        r.currentFolder = r.currentFile.parent_path();
      }

      const std::uint64_t id = m_nextId++;
      pending->result = std::move(result);
      pending->requestObject = sdbus::createObject(m_bus.connection(), handle);
      pending->requestObject
          ->addVTable(sdbus::registerMethod("Close").implementedAs([this, id] {
            // The caller gave up: end the call as "other", then drop the Request object after this callback, which
            // that object owns.
            const auto it = m_pending.find(id);
            if (it == m_pending.end() || !it->second->result) {
              return;
            }
            it->second->result->returnResults(std::uint32_t{2}, Options{});
            it->second->result.reset();
            DeferredCall::callLater([this, id, alive = std::weak_ptr<bool>(m_alive)] {
              if (!alive.expired() && m_pending.erase(id) != 0 && m_onClose) {
                m_onClose(id);
              }
            });
          }))
          .forInterface(kRequestInterface);
      const Request copy = r;
      m_pending.emplace(id, std::move(pending));
      kLog.info("{} from {} (multiple={}, directory={}, {} filters)",
                kind == Request::Kind::Open ? "OpenFile" : kind == Request::Kind::Save ? "SaveFile" : "SaveFiles",
                copy.appId.empty() ? "?" : copy.appId, copy.multiple, copy.directory, copy.filters.size());
      m_onRequest(id, copy);
    };
  };
  m_object
      ->addVTable(
          sdbus::registerMethod("OpenFile")
              .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
              .withOutputParamNames("response", "results")
              .implementedAs(handler(Request::Kind::Open)),
          sdbus::registerMethod("SaveFile")
              .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
              .withOutputParamNames("response", "results")
              .implementedAs(handler(Request::Kind::Save)),
          sdbus::registerMethod("SaveFiles")
              .withInputParamNames("handle", "app_id", "parent_window", "title", "options")
              .withOutputParamNames("response", "results")
              .implementedAs(handler(Request::Kind::SaveFiles))
      )
      .forInterface(kInterface);
  m_bus.connection().requestName(kBusName);
}

PickerPortal::~PickerPortal() {
  for (auto& [id, pending] : m_pending) {
    if (pending->result) {
      pending->result->returnResults(std::uint32_t{2}, Options{});
    }
  }
}

void PickerPortal::answer(std::uint64_t id, const Answer& answer) {
  const auto it = m_pending.find(id);
  if (it == m_pending.end()) {
    return;
  }
  if (!it->second->result) {
    return;
  }
  auto pending = std::move(it->second);
  m_pending.erase(it);
  Options results;
  if (answer.response == 0) {
    results["uris"] = sdbus::Variant(answer.uris);
    if (answer.filter.has_value() && *answer.filter < pending->request.filters.size()) {
      results["current_filter"] = sdbus::Variant(toStruct(pending->request.filters[*answer.filter]));
    }
  }
  kLog.info("answer {} with {} uris", answer.response, answer.uris.size());
  pending->result->returnResults(answer.response, results);
}

bool PickerPortal::matches(const Filter& filter, const std::string& name, const std::string& contentType) {
  for (const auto& [kind, pattern] : filter.patterns) {
    if (kind == 0) {
      gchar* lowerName = g_utf8_strdown(name.c_str(), -1);
      gchar* lowerPattern = g_utf8_strdown(pattern.c_str(), -1);
      const bool hit = g_pattern_match_simple(lowerPattern, lowerName) != 0;
      g_free(lowerName);
      g_free(lowerPattern);
      if (hit) {
        return true;
      }
    } else if (kind == 1 && !contentType.empty() && g_content_type_is_a(contentType.c_str(), pattern.c_str()) != 0) {
      return true;
    }
  }
  return false;
}

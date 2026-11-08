#include "app_chooser_portal.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("app-chooser");
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kChooserInterface = "org.freedesktop.impl.portal.AppChooser";
  constexpr auto kAccessInterface = "org.freedesktop.impl.portal.Access";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";
  using Options = std::map<std::string, sdbus::Variant>;
  using ChoiceOption = sdbus::Struct<std::string, std::string>;
  using ChoiceStruct = sdbus::Struct<std::string, std::string, std::vector<ChoiceOption>, std::string>;

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
} // namespace

struct AppChooserPortal::Pending {
  std::optional<sdbus::Result<std::uint32_t, Options>> result;
  std::unique_ptr<sdbus::IObject> requestObject;
  std::string handle;
};

AppChooserPortal::AppChooserPortal(SessionBus& bus, Handlers handlers) : m_bus(bus), m_handlers(std::move(handlers)) {
  m_chooser = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_chooser
      ->addVTable(
          sdbus::registerMethod("ChooseApplication")
              .withInputParamNames("handle", "app_id", "parent_window", "choices", "options")
              .withOutputParamNames("response", "results")
              .implementedAs([this](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath handle,
                                    std::string appId, std::string parent, std::vector<std::string> choices,
                                    Options options) {
                ChooseRequest r{.appId = std::move(appId), .parentWindow = std::move(parent),
                                .choices = std::move(choices),
                                .lastChoice = option<std::string>(options, "last_choice").value_or(""),
                                .contentType = option<std::string>(options, "content_type").value_or(""),
                                .filename = option<std::string>(options, "filename").value_or("")};
                auto pending = std::make_unique<Pending>();
                pending->result = std::move(result);
                const std::uint64_t id = track(handle, std::move(pending));
                kLog.info("choose from {} ({} choices, type {})", r.appId.empty() ? "?" : r.appId, r.choices.size(),
                          r.contentType);
                if (m_handlers.choose) {
                  m_handlers.choose(id, r);
                }
              }),
          sdbus::registerMethod("UpdateChoices")
              .withInputParamNames("handle", "choices")
              .implementedAs([this](sdbus::ObjectPath handle, std::vector<std::string> choices) {
                if (const auto it = m_byHandle.find(handle); it != m_byHandle.end() && m_handlers.updateChoices) {
                  m_handlers.updateChoices(it->second, choices);
                }
              })
      )
      .forInterface(kChooserInterface);

  m_access = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_access
      ->addVTable(sdbus::registerMethod("AccessDialog")
                      .withInputParamNames("handle", "app_id", "parent_window", "title", "subtitle", "body", "options")
                      .withOutputParamNames("response", "results")
                      .implementedAs([this](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath handle,
                                            std::string appId, std::string parent, std::string title,
                                            std::string subtitle, std::string body, Options options) {
                        AccessRequest r{.appId = std::move(appId), .parentWindow = std::move(parent),
                                        .title = std::move(title), .subtitle = std::move(subtitle),
                                        .body = std::move(body),
                                        .denyLabel = option<std::string>(options, "deny_label").value_or("Deny"),
                                        .grantLabel = option<std::string>(options, "grant_label").value_or("Allow"),
                                        .icon = option<std::string>(options, "icon").value_or(""), .choices = {}};
                        for (const ChoiceStruct& c : option<std::vector<ChoiceStruct>>(options, "choices").value_or({})) {
                          AccessChoice choice{.id = c.get<0>(), .label = c.get<1>(), .options = {}, .value = c.get<3>()};
                          for (const ChoiceOption& o : c.get<2>()) {
                            choice.options.emplace_back(o.get<0>(), o.get<1>());
                          }
                          if (choice.value.empty()) {
                            choice.value = choice.options.empty() ? "false" : choice.options.front().first;
                          }
                          r.choices.push_back(std::move(choice));
                        }
                        auto pending = std::make_unique<Pending>();
                        pending->result = std::move(result);
                        const std::uint64_t id = track(handle, std::move(pending));
                        kLog.info("access from {}: {}", r.appId.empty() ? "?" : r.appId, r.title);
                        if (m_handlers.access) {
                          m_handlers.access(id, r);
                        }
                      }))
      .forInterface(kAccessInterface);
}

AppChooserPortal::~AppChooserPortal() {
  for (auto& [id, pending] : m_pending) {
    if (pending->result) {
      pending->result->returnResults(std::uint32_t{2}, Options{});
    }
  }
}

std::uint64_t AppChooserPortal::track(const std::string& handle, std::unique_ptr<Pending> pending) {
  const std::uint64_t id = m_nextId++;
  pending->handle = handle;
  pending->requestObject = sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{handle});
  pending->requestObject
      ->addVTable(sdbus::registerMethod("Close").implementedAs([this, id] {
        // The caller gave up: end the call as "other"; the Request object goes after this callback, which it owns.
        const auto it = m_pending.find(id);
        if (it == m_pending.end() || !it->second->result) {
          return;
        }
        it->second->result->returnResults(std::uint32_t{2}, Options{});
        it->second->result.reset();
        DeferredCall::callLater([this, id, alive = std::weak_ptr<bool>(m_alive)] {
          if (alive.expired()) {
            return;
          }
          if (const auto p = m_pending.find(id); p != m_pending.end()) {
            m_byHandle.erase(p->second->handle);
            m_pending.erase(p);
          }
          if (m_handlers.closed) {
            m_handlers.closed(id);
          }
        });
      }))
      .forInterface(kRequestInterface);
  m_byHandle[handle] = id;
  m_pending.emplace(id, std::move(pending));
  return id;
}

void AppChooserPortal::answerChoose(std::uint64_t id, std::uint32_t response, const std::string& choice) {
  const auto it = m_pending.find(id);
  if (it == m_pending.end() || !it->second->result) {
    return;
  }
  auto pending = std::move(it->second);
  m_pending.erase(it);
  m_byHandle.erase(pending->handle);
  Options results;
  if (response == 0) {
    results["choice"] = sdbus::Variant(choice);
  }
  kLog.info("chose {} ({})", choice, response);
  pending->result->returnResults(response, results);
}

void AppChooserPortal::answerAccess(std::uint64_t id, std::uint32_t response, const std::vector<AccessChoice>& choices) {
  const auto it = m_pending.find(id);
  if (it == m_pending.end() || !it->second->result) {
    return;
  }
  auto pending = std::move(it->second);
  m_pending.erase(it);
  m_byHandle.erase(pending->handle);
  Options results;
  if (response == 0 && !choices.empty()) {
    std::vector<ChoiceOption> chosen;
    for (const AccessChoice& c : choices) {
      chosen.emplace_back(c.id, c.value);
    }
    results["choices"] = sdbus::Variant(chosen);
  }
  kLog.info("access answer {}", response);
  pending->result->returnResults(response, results);
}

#include "shell/bar/widgets/dbus_menu_client.h"

#include "core/log.h"

#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("app-menu");
  constexpr auto kInterface = "com.canonical.dbusmenu";
  constexpr auto kTimeout = std::chrono::milliseconds(2000);

  using Layout = sdbus::Struct<std::int32_t, std::map<std::string, sdbus::Variant>, std::vector<sdbus::Variant>>;
  using PropertiesUpdate = std::vector<sdbus::Struct<std::int32_t, std::map<std::string, sdbus::Variant>>>;
  using PropertiesRemoved = std::vector<sdbus::Struct<std::int32_t, std::vector<std::string>>>;

  template <typename T> std::optional<T> get(const std::map<std::string, sdbus::Variant>& props, const char* key) {
    const auto it = props.find(key);
    if (it == props.end() || !it->second.containsValueOfType<T>()) {
      return std::nullopt;
    }
    return it->second.get<T>();
  }

  // "_File" -> "File", "__" -> "_" (mnemonics show only after F10, board A5).
  std::string stripMnemonic(const std::string& label) {
    std::string out;
    out.reserve(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
      if (label[i] == '_' && i + 1 < label.size()) {
        out += label[++i] == '_' ? '_' : label[i];
        continue;
      }
      out += label[i];
    }
    return out;
  }

  // dbusmenu "shortcut" is aas: [["Control", "Shift", "z"]] -> "Ctrl+Shift+Z".
  std::string shortcutText(const std::vector<std::vector<std::string>>& chords) {
    if (chords.empty()) {
      return {};
    }
    std::string out;
    for (const std::string& key : chords.front()) {
      std::string name = key == "Control" ? "Ctrl" : key == "Super" ? "Super" : key;
      if (name.size() == 1) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
      }
      out += (out.empty() ? "" : "+") + name;
    }
    return out;
  }

  DbusMenuNode decode(const Layout& layout) {
    const auto& props = std::get<1>(layout);
    DbusMenuNode node;
    node.id = std::get<0>(layout);
    node.label = stripMnemonic(get<std::string>(props, "label").value_or(""));
    node.enabled = get<bool>(props, "enabled").value_or(true);
    node.visible = get<bool>(props, "visible").value_or(true);
    node.separator = get<std::string>(props, "type").value_or("") == "separator";
    const std::string toggle = get<std::string>(props, "toggle-type").value_or("");
    node.checkmark = toggle == "checkmark";
    node.radio = toggle == "radio";
    node.toggleState = get<std::int32_t>(props, "toggle-state").value_or(-1);
    node.shortcut = shortcutText(get<std::vector<std::vector<std::string>>>(props, "shortcut").value_or({}));
    for (const sdbus::Variant& child : std::get<2>(layout)) {
      if (child.containsValueOfType<Layout>()) {
        node.children.push_back(decode(child.get<Layout>()));
      }
    }
    return node;
  }
} // namespace

DbusMenuClient::DbusMenuClient(
    sdbus::IConnection& bus, std::string service, std::string path, std::function<void()> onChanged
)
    : m_service(std::move(service)), m_path(std::move(path)), m_onChanged(std::move(onChanged)) {
  try {
    m_proxy = sdbus::createProxy(bus, sdbus::ServiceName{m_service}, sdbus::ObjectPath{m_path});
    m_proxy->uponSignal("LayoutUpdated").onInterface(kInterface).call([this](std::uint32_t, std::int32_t) { load(); });
    m_proxy->uponSignal("ItemsPropertiesUpdated")
        .onInterface(kInterface)
        .call([this](const PropertiesUpdate&, const PropertiesRemoved&) { load(); });
  } catch (const sdbus::Error& e) {
    kLog.debug("no menu proxy for {} {}: {}", m_service, m_path, e.what());
    m_proxy.reset();
    return;
  }
  load();
}

DbusMenuClient::~DbusMenuClient() { *m_alive = false; }

void DbusMenuClient::load() {
  if (m_proxy == nullptr) {
    return;
  }
  try {
    m_proxy->callMethodAsync("GetLayout")
        .onInterface(kInterface)
        .withTimeout(kTimeout)
        .withArguments(std::int32_t{0}, std::int32_t{-1}, std::vector<std::string>{})
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](
                             std::optional<sdbus::Error> error, std::uint32_t /*revision*/, Layout layout
                         ) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.debug("GetLayout {} failed: {}", m_service, error->what());
            return;
          }
          m_root = decode(layout);
          m_loaded = true;
          if (m_onChanged) {
            m_onChanged();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("GetLayout {} failed: {}", m_service, e.what());
  }
}

void DbusMenuClient::event(std::int32_t id, const char* name) {
  if (m_proxy == nullptr) {
    return;
  }
  const auto now = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
  );
  try {
    m_proxy->callMethodAsync("Event")
        .onInterface(kInterface)
        .withTimeout(kTimeout)
        .withArguments(id, std::string(name), sdbus::Variant{std::int32_t{0}}, now)
        .uponReplyInvoke([](std::optional<sdbus::Error>) {});
  } catch (const sdbus::Error& e) {
    kLog.debug("Event {} on {} failed: {}", name, m_service, e.what());
  }
}

void DbusMenuClient::opened(std::int32_t id) {
  event(id, "opened");
  if (m_proxy == nullptr) {
    return;
  }
  try {
    m_proxy->callMethodAsync("AboutToShow")
        .onInterface(kInterface)
        .withTimeout(kTimeout)
        .withArguments(id)
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, bool update) {
          if (!alive.expired() && !error.has_value() && update) {
            load();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("AboutToShow on {} failed: {}", m_service, e.what());
  }
}

void DbusMenuClient::closed(std::int32_t id) { event(id, "closed"); }

void DbusMenuClient::activate(std::int32_t id) { event(id, "clicked"); }

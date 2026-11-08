#include "shell/bar/widgets/gtk_menu_client.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sdbus-c++/sdbus-c++.h>
#include <set>

namespace {
  constexpr Logger kLog("app-menu");
  constexpr auto kMenus = "org.gtk.Menus";
  constexpr auto kActions = "org.gtk.Actions";
  constexpr auto kTimeout = std::chrono::milliseconds(2000);
  // Submenu and section levels deeper than this are dropped (the bar shows one level under each top menu).
  constexpr int kMaxDepth = 6;
  constexpr int kMaxNodes = 2000;

  using ItemProps = std::map<std::string, sdbus::Variant>;
  using MenuGroup = sdbus::Struct<std::uint32_t, std::uint32_t, std::vector<ItemProps>>;
  using Link = sdbus::Struct<std::uint32_t, std::uint32_t>;
  using ActionInfo = sdbus::Struct<bool, sdbus::Signature, std::vector<sdbus::Variant>>;

  // "_File" -> "File" (mnemonics show only after F10).
  std::string stripMnemonic(const std::string& label) {
    std::string out;
    for (std::size_t i = 0; i < label.size(); ++i) {
      if (label[i] == '_' && i + 1 < label.size()) {
        out += label[++i];
        continue;
      }
      out += label[i];
    }
    return out;
  }

  // GTK accelerator "<Primary><Shift>z" -> "Ctrl+Shift+Z".
  std::string accelText(const std::string& accel) {
    std::string out;
    std::size_t i = 0;
    while (i < accel.size() && accel[i] == '<') {
      const std::size_t end = accel.find('>', i);
      if (end == std::string::npos) {
        break;
      }
      std::string mod = accel.substr(i + 1, end - i - 1);
      if (mod == "Primary" || mod == "Control" || mod == "Ctrl") {
        mod = "Ctrl";
      }
      out += mod + "+";
      i = end + 1;
    }
    std::string key = accel.substr(i);
    if (key.size() == 1) {
      key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    }
    return out + key;
  }
} // namespace

GtkMenuClient::GtkMenuClient(sdbus::IConnection& bus, std::string service, std::string menuPath, std::string appPath,
                             std::string windowPath, std::function<void()> onChanged)
    : m_service(std::move(service)), m_menuPath(std::move(menuPath)), m_appPath(std::move(appPath)),
      m_windowPath(std::move(windowPath)), m_onChanged(std::move(onChanged)) {
  try {
    m_menus = sdbus::createProxy(bus, sdbus::ServiceName{m_service}, sdbus::ObjectPath{m_menuPath});
    m_menus->uponSignal("Changed")
        .onInterface(kMenus)
        .call([this](const std::vector<sdbus::Struct<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                     std::vector<ItemProps>>>&) { load(); });
    if (!m_appPath.empty()) {
      m_appActions = sdbus::createProxy(bus, sdbus::ServiceName{m_service}, sdbus::ObjectPath{m_appPath});
    }
    if (!m_windowPath.empty()) {
      m_windowActions = sdbus::createProxy(bus, sdbus::ServiceName{m_service}, sdbus::ObjectPath{m_windowPath});
    }
    for (auto* proxy : {m_appActions.get(), m_windowActions.get()}) {
      if (proxy == nullptr) {
        continue;
      }
      const std::string prefix = proxy == m_appActions.get() ? "app." : "win.";
      proxy->uponSignal("Changed").onInterface(kActions).call(
          [this, prefix](const std::vector<std::string>& removed, const std::map<std::string, bool>& enabled,
                         const std::map<std::string, sdbus::Variant>& /*state*/,
                         const std::map<std::string, ActionInfo>& added) {
            for (const std::string& name : removed) {
              m_enabled.erase(prefix + name);
            }
            for (const auto& [name, on] : enabled) {
              m_enabled[prefix + name] = on;
            }
            for (const auto& [name, info] : added) {
              m_enabled[prefix + name] = std::get<0>(info);
            }
            if (m_loaded) {
              build();
            }
          }
      );
    }
  } catch (const sdbus::Error& e) {
    kLog.debug("no GTK menu proxy for {} {}: {}", m_service, m_menuPath, e.what());
    m_menus.reset();
    return;
  }
  loadActions();
  load();
}

GtkMenuClient::~GtkMenuClient() {
  *m_alive = false;
  if (m_menus != nullptr && !m_subscribed.empty()) {
    try {
      m_menus->callMethodAsync("End").onInterface(kMenus).withArguments(m_subscribed).uponReplyInvoke([](std::optional<sdbus::Error>) {});
    } catch (const sdbus::Error&) {
    }
  }
}

// Subscribes to group 0, then to every group its sections and submenus name, until the tree is complete.
void GtkMenuClient::load() {
  if (m_menus == nullptr) {
    return;
  }
  m_menusById.clear();
  // The old groups end only after the new Start answers: GTK frees a group, and may reuse its id, at zero subscribers.
  auto previous = std::make_shared<std::vector<std::uint32_t>>(std::move(m_subscribed));
  m_subscribed.clear();
  const std::uint64_t generation = ++m_loadGeneration;
  auto pending = std::make_shared<std::set<std::uint32_t>>();
  const auto start = std::make_shared<std::function<void(std::vector<std::uint32_t>)>>();
  // The function holds itself weakly; each pending reply holds it strongly until it runs.
  *start = [this, alive = std::weak_ptr<bool>(m_alive), pending, previous, generation,
            weakStart = std::weak_ptr<std::function<void(std::vector<std::uint32_t>)>>(start)](
               std::vector<std::uint32_t> groups
           ) {
    const auto self = weakStart.lock();
    if (alive.expired() || m_menus == nullptr || self == nullptr || generation != m_loadGeneration) {
      return;
    }
    m_subscribed.insert(m_subscribed.end(), groups.begin(), groups.end());
    try {
      m_menus->callMethodAsync("Start")
          .onInterface(kMenus)
          .withTimeout(kTimeout)
          .withArguments(groups)
          .uponReplyInvoke([this, alive, pending, previous, generation, self](
                               std::optional<sdbus::Error> error, std::vector<MenuGroup> menus
                           ) {
            if (alive.expired() || generation != m_loadGeneration) {
              return;
            }
            if (!previous->empty()) {
              try {
                m_menus->callMethodAsync("End").onInterface(kMenus).withArguments(*previous).uponReplyInvoke(
                    [](std::optional<sdbus::Error>) {}
                );
              } catch (const sdbus::Error&) {
              }
              previous->clear();
            }
            if (error.has_value()) {
              kLog.debug("org.gtk.Menus.Start on {} failed: {}", m_service, error->what());
              return;
            }
            std::vector<std::uint32_t> next;
            for (const MenuGroup& menu : menus) {
              std::vector<Item>& items = m_menusById[{std::get<0>(menu), std::get<1>(menu)}];
              items.clear();
              for (const ItemProps& props : std::get<2>(menu)) {
                Item item;
                for (const auto& [key, value] : props) {
                  if (value.containsValueOfType<std::string>()) {
                    item.strings[key] = value.get<std::string>();
                  } else if (value.containsValueOfType<Link>()) {
                    const Link link = value.get<Link>();
                    item.links.emplace_back(key, std::pair{std::get<0>(link), std::get<1>(link)});
                    if (std::ranges::find(m_subscribed, std::get<0>(link)) == m_subscribed.end()
                        && pending->insert(std::get<0>(link)).second) {
                      next.push_back(std::get<0>(link));
                    }
                  } else if (key == "target" && value.containsValueOfType<sdbus::Variant>()) {
                    const auto inner = value.get<sdbus::Variant>();
                    if (inner.containsValueOfType<std::string>()) {
                      item.strings["target"] = inner.get<std::string>();
                    }
                  }
                }
                items.push_back(std::move(item));
              }
            }
            if (!next.empty()) {
              (*self)(std::move(next));
              return;
            }
            build();
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("org.gtk.Menus.Start on {} failed: {}", m_service, e.what());
    }
  };
  (*start)({0});
}

void GtkMenuClient::loadActions() {
  for (auto* proxy : {m_appActions.get(), m_windowActions.get()}) {
    if (proxy == nullptr) {
      continue;
    }
    const std::string prefix = proxy == m_appActions.get() ? "app." : "win.";
    try {
      proxy->callMethodAsync("DescribeAll")
          .onInterface(kActions)
          .withTimeout(kTimeout)
          .uponReplyInvoke([this, prefix, alive = std::weak_ptr<bool>(m_alive)](
                               std::optional<sdbus::Error> error, std::map<std::string, ActionInfo> actions) {
            if (alive.expired() || error.has_value()) {
              return;
            }
            for (const auto& [name, info] : actions) {
              m_enabled[prefix + name] = std::get<0>(info);
            }
            if (m_loaded) {
              build();
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("org.gtk.Actions.DescribeAll on {} failed: {}", m_service, e.what());
    }
  }
}

void GtkMenuClient::build() {
  m_root = {};
  m_visiting.clear();
  m_nodeBudget = kMaxNodes;
  appendMenu(m_root, 0, 0, 0);
  m_loaded = true;
  if (m_onChanged) {
    m_onChanged();
  }
}

void GtkMenuClient::appendMenu(DbusMenuNode& parent, std::uint32_t group, std::uint32_t menu, int depth) {
  const auto found = m_menusById.find({group, menu});
  if (found == m_menusById.end() || depth > kMaxDepth || m_nodeBudget <= 0 || !m_visiting.insert({group, menu}).second) {
    return;
  }
  for (const Item& item : found->second) {
    const auto link = [&](const char* name) -> const std::pair<std::uint32_t, std::uint32_t>* {
      const auto it = std::ranges::find_if(item.links, [&](const auto& entry) { return entry.first == name; });
      return it != item.links.end() ? &it->second : nullptr;
    };
    if (const auto* section = link(":section")) {
      // A section reads as its rows between separators.
      if (!parent.children.empty() && !parent.children.back().separator) {
        parent.children.push_back({.id = 0, .separator = true});
      }
      appendMenu(parent, section->first, section->second, depth + 1);
      continue;
    }
    DbusMenuNode node;
    const auto label = item.strings.find("label");
    node.label = label != item.strings.end() ? stripMnemonic(label->second) : std::string{};
    if (const auto accel = item.strings.find("accel"); accel != item.strings.end()) {
      node.shortcut = accelText(accel->second);
    }
    if (const auto* submenu = link(":submenu")) {
      appendMenu(node, submenu->first, submenu->second, depth + 1);
    }
    if (const auto action = item.strings.find("action"); action != item.strings.end()) {
      const auto enabled = m_enabled.find(action->second);
      node.enabled = enabled == m_enabled.end() || enabled->second;
      const auto target = item.strings.find("target");
      std::string encoded = action->second + (target != item.strings.end() ? "\n" + target->second : "");
      auto [id, added] = m_idByAction.try_emplace(encoded, static_cast<std::int32_t>(m_actionById.size() + 1));
      if (added) {
        m_actionById.push_back(std::move(encoded));
      }
      node.id = id->second;
    }
    if (!node.label.empty() && m_nodeBudget > 0) {
      --m_nodeBudget;
      parent.children.push_back(std::move(node));
    }
  }
  while (!parent.children.empty() && parent.children.back().separator) {
    parent.children.pop_back();
  }
  m_visiting.erase({group, menu});
}

void GtkMenuClient::activate(std::int32_t id) {
  if (id <= 0 || static_cast<std::size_t>(id) > m_actionById.size()) {
    return;
  }
  const std::string& encoded = m_actionById[static_cast<std::size_t>(id) - 1];
  const std::size_t newline = encoded.find('\n');
  const std::string action = encoded.substr(0, newline);
  const std::size_t dot = action.find('.');
  sdbus::IProxy* proxy = action.starts_with("app.") ? m_appActions.get()
      : action.starts_with("win.")                  ? m_windowActions.get()
                                                    : nullptr;
  if (proxy == nullptr || dot == std::string::npos) {
    return;
  }
  std::vector<sdbus::Variant> parameter;
  if (newline != std::string::npos) {
    parameter.emplace_back(encoded.substr(newline + 1));
  }
  try {
    proxy->callMethodAsync("Activate")
        .onInterface(kActions)
        .withTimeout(kTimeout)
        .withArguments(action.substr(dot + 1), parameter, std::map<std::string, sdbus::Variant>{})
        .uponReplyInvoke([](std::optional<sdbus::Error>) {});
  } catch (const sdbus::Error& e) {
    kLog.debug("org.gtk.Actions.Activate {} on {} failed: {}", action, m_service, e.what());
  }
}

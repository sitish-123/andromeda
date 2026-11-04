#include "a11y/atspi_bridge.h"

#include "core/log.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>
#include <utility>
#include <unordered_set>

namespace {

  constexpr Logger kLog("a11y");

  constexpr auto kAccessible = "org.a11y.atspi.Accessible";
  constexpr auto kApplication = "org.a11y.atspi.Application";
  constexpr auto kComponent = "org.a11y.atspi.Component";
  constexpr auto kAction = "org.a11y.atspi.Action";
  constexpr auto kValue = "org.a11y.atspi.Value";
  constexpr auto kCache = "org.a11y.atspi.Cache";
  constexpr auto kText = "org.a11y.atspi.Text";
  constexpr auto kEventObject = "org.a11y.atspi.Event.Object";
  constexpr auto kEventWindow = "org.a11y.atspi.Event.Window";
  constexpr auto kRootPath = "/org/a11y/atspi/accessible/root";
  constexpr auto kCachePath = "/org/a11y/atspi/cache";
  constexpr auto kNullPath = "/org/a11y/atspi/null";
  constexpr auto kItemPathPrefix = "/org/a11y/atspi/accessible/";

  // atspi-constants.h values (checked against at-spi2-core 2.62).
  constexpr std::uint32_t kRoleApplication = 75;
  constexpr std::uint32_t kRoleFrame = 23;

  std::uint32_t atspiRole(AccessibleRole role) {
    switch (role) {
    case AccessibleRole::None:
    case AccessibleRole::Panel:
      return 39;
    case AccessibleRole::Window:
      return kRoleFrame;
    case AccessibleRole::Button:
      return 43;
    case AccessibleRole::ToggleButton:
      return 62;
    case AccessibleRole::CheckBox:
      return 7;
    case AccessibleRole::RadioButton:
      return 44;
    case AccessibleRole::Slider:
      return 51;
    case AccessibleRole::SpinButton:
      return 52;
    case AccessibleRole::List:
      return 31;
    case AccessibleRole::ListItem:
      return 32;
    case AccessibleRole::TextField:
      return 79;
    case AccessibleRole::Label:
      return 29;
    case AccessibleRole::Image:
      return 27;
    case AccessibleRole::Menu:
      return 33;
    case AccessibleRole::MenuItem:
      return 35;
    case AccessibleRole::Dialog:
      return 16;
    case AccessibleRole::TabList:
      return 38;
    case AccessibleRole::Tab:
      return 37;
    case AccessibleRole::ScrollPane:
      return 49;
    case AccessibleRole::ProgressBar:
      return 42;
    case AccessibleRole::Separator:
      return 50;
    }
    return 39;
  }

  std::string roleName(AccessibleRole role) {
    switch (role) {
    case AccessibleRole::Window:
      return "frame";
    case AccessibleRole::Button:
      return "push button";
    case AccessibleRole::ToggleButton:
      return "toggle button";
    case AccessibleRole::CheckBox:
      return "check box";
    case AccessibleRole::RadioButton:
      return "radio button";
    case AccessibleRole::Slider:
      return "slider";
    case AccessibleRole::SpinButton:
      return "spin button";
    case AccessibleRole::List:
      return "list";
    case AccessibleRole::ListItem:
      return "list item";
    case AccessibleRole::TextField:
      return "entry";
    case AccessibleRole::Label:
      return "label";
    case AccessibleRole::Image:
      return "image";
    case AccessibleRole::Menu:
      return "menu";
    case AccessibleRole::MenuItem:
      return "menu item";
    case AccessibleRole::Dialog:
      return "dialog";
    case AccessibleRole::TabList:
      return "page tab list";
    case AccessibleRole::Tab:
      return "page tab";
    case AccessibleRole::ScrollPane:
      return "scroll pane";
    case AccessibleRole::ProgressBar:
      return "progress bar";
    case AccessibleRole::Separator:
      return "separator";
    default:
      return "panel";
    }
  }

  // AtspiStateType bit numbers.
  enum : std::uint8_t {
    kActive = 1,
    kChecked = 4,
    kEditable = 7,
    kEnabled = 8,
    kExpandable = 9,
    kExpanded = 10,
    kFocusable = 11,
    kFocused = 12,
    kSelectable = 22,
    kSelected = 23,
    kSensitive = 24,
    kShowing = 25,
    kSingleLine = 26,
    kVisible = 30,
    kCheckable = 41,
  };

  constexpr std::uint64_t bit(std::uint8_t state) { return std::uint64_t{1} << state; }

  std::uint64_t statesFor(const Accessible& accessible, const AccessibleEntry& entry) {
    std::uint64_t states = bit(kVisible);
    if (!accessible.disabled) {
      states |= bit(kEnabled) | bit(kSensitive);
    }
    if (entry.showing) {
      states |= bit(kShowing);
    }
    if (entry.focusable) {
      states |= bit(kFocusable);
    }
    if (entry.focused) {
      states |= bit(kFocused);
    }
    if (accessible.checked.has_value()) {
      states |= bit(kCheckable);
      if (*accessible.checked) {
        states |= bit(kChecked);
      }
    }
    if (accessible.role == AccessibleRole::ListItem || accessible.role == AccessibleRole::Tab) {
      states |= bit(kSelectable);
    }
    if (accessible.selected) {
      states |= bit(kSelected);
    }
    if (accessible.expandable) {
      states |= bit(kExpandable);
    }
    if (accessible.expanded) {
      states |= bit(kExpanded);
    }
    if (accessible.role == AccessibleRole::TextField) {
      states |= bit(kEditable) | bit(kSingleLine);
    }
    return states;
  }

  std::vector<std::uint32_t> stateWords(std::uint64_t states) {
    return {static_cast<std::uint32_t>(states & 0xffffffffU), static_cast<std::uint32_t>(states >> 32U)};
  }

  std::int32_t px(float value) { return static_cast<std::int32_t>(std::lround(value)); }

  using Ref = sdbus::Struct<std::string, sdbus::ObjectPath>;

  // UTF-8 helpers: AT-SPI text offsets count characters.
  std::int32_t charCount(const std::string& text) {
    return static_cast<std::int32_t>(std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
  }

  std::size_t byteOffset(const std::string& text, std::int32_t chars) {
    std::size_t i = 0;
    for (std::int32_t seen = 0; i < text.size(); ++i) {
      if ((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U && seen++ == chars) {
        break;
      }
    }
    return i;
  }

  // (text, start, end) around `offset` in characters. kind: 0 character, 1 word, 3 whole text.
  std::tuple<std::string, std::int32_t, std::int32_t> textAt(const std::string& text, std::int32_t offset, std::uint32_t kind) {
    const std::int32_t count = charCount(text);
    if (kind >= 3 || count == 0) {
      return {text, 0, count};
    }
    offset = std::clamp(offset, 0, count - 1);
    std::int32_t start = offset;
    std::int32_t end = offset + 1;
    if (kind == 1) {
      const auto spaceAt = [&](std::int32_t at) { return text[byteOffset(text, at)] == ' '; };
      while (start > 0 && !spaceAt(start - 1)) {
        --start;
      }
      while (end < count && !spaceAt(end)) {
        ++end;
      }
    }
    const std::size_t from = byteOffset(text, start);
    return {text.substr(from, byteOffset(text, end) - from), start, end};
  }

  bool findNode(const Node& under, const Node* target) {
    if (&under == target) {
      return true;
    }
    return std::ranges::any_of(under.children(), [target](const auto& child) { return findNode(*child, target); });
  }

} // namespace

AtspiBridge& AtspiBridge::instance() {
  static AtspiBridge bridge;
  return bridge;
}

AtspiBridge::AtspiBridge() = default;

AtspiBridge::~AtspiBridge() { stop(); }

void AtspiBridge::start(sdbus::IConnection& sessionBus, std::string appName) {
  const char* strict = std::getenv("DESK_A11Y_STRICT");
  m_strict = strict != nullptr && std::string_view(strict) == "1";
  m_appName = std::move(appName);
  m_busProxy = sdbus::createProxy(sessionBus, sdbus::ServiceName{"org.a11y.Bus"}, sdbus::ObjectPath{"/org/a11y/bus"});
  m_busProxy->uponSignal("PropertiesChanged")
      .onInterface("org.freedesktop.DBus.Properties")
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName != "org.a11y.Status") {
          return;
        }
        if (const auto it = changed.find("IsEnabled"); it != changed.end()) {
          m_isEnabled = it->second.get<bool>();
        }
        if (const auto it = changed.find("ScreenReaderEnabled"); it != changed.end()) {
          m_screenReaderEnabled = it->second.get<bool>();
        }
        setEnabled(m_isEnabled || m_screenReaderEnabled);
      });
  const std::weak_ptr<int> alive = m_alive;
  m_busProxy->callMethodAsync("GetAll")
      .onInterface("org.freedesktop.DBus.Properties")
      .withArguments(std::string("org.a11y.Status"))
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> props) {
        if (alive.expired() || error.has_value()) {
          return;
        }
        if (const auto it = props.find("IsEnabled"); it != props.end()) {
          m_isEnabled = it->second.get<bool>();
        }
        if (const auto it = props.find("ScreenReaderEnabled"); it != props.end()) {
          m_screenReaderEnabled = it->second.get<bool>();
        }
        setEnabled(m_isEnabled || m_screenReaderEnabled);
      });
}

void AtspiBridge::stop() {
  disconnect();
  m_busProxy.reset();
}

void AtspiBridge::addRoot(const Node* root, std::function<std::string()> title) {
  if (std::ranges::find(m_roots, root, &Root::node) == m_roots.end()) {
    m_roots.push_back({.node = root, .title = std::move(title)});
  }
}

void AtspiBridge::removeRoot(const Node* root) {
  std::erase_if(m_roots, [root](const Root& entry) { return entry.node == root; });
  // Items of this root reference freed nodes from now on: drop them at once.
  std::vector<std::uint32_t> gone;
  for (const auto& [id, item] : m_items) {
    if (item.root == root) {
      gone.push_back(id);
    }
  }
  for (const std::uint32_t id : gone) {
    const Item& removed = m_items.at(id);
    if (removed.parent == 0) {
      emitObjectEvent(0, "ChildrenChanged", "remove", indexInParent(id), 0, sdbus::Variant{refFor(id)});
    }
    if (m_activeWindow == id) {
      m_activeWindow = 0;
    }
    m_ids.erase(removed.key);
    m_items.erase(id);
  }
}

void AtspiBridge::setEnabled(bool enabled) {
  m_wanted = enabled;
  if (!enabled) {
    disconnect();
    return;
  }
  if (active() || m_connecting) {
    return;
  }
  m_connecting = true;
  const std::weak_ptr<int> alive = m_alive;
  m_busProxy->callMethodAsync("GetAddress")
      .onInterface("org.a11y.Bus")
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, std::string address) {
        if (alive.expired()) {
          return;
        }
        m_connecting = false;
        if (error.has_value()) {
          kLog.warn("accessibility bus unavailable: {}", error->getMessage());
          return;
        }
        if (m_wanted && !active()) {
          connect(address);
        }
      });
}

void AtspiBridge::failed(const char* what, const std::exception& error) {
  kLog.warn("{}: {}", what, error.what());
  disconnect();
}

void AtspiBridge::connect(const std::string& address) {
  disconnect();
  try {
    m_connection = sdbus::createSessionBusConnectionWithAddress(address);
    exportApplication();
    exportCache();
    m_registry = sdbus::createProxy(
        *m_connection, sdbus::ServiceName{"org.a11y.atspi.Registry"}, sdbus::ObjectPath{kRootPath}
    );
    const std::weak_ptr<int> alive = m_alive;
    m_registry->callMethodAsync("Embed")
        .onInterface("org.a11y.atspi.Socket")
        .withArguments(refFor(0))
        .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, Ref desktop) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("AT-SPI embed failed: {}", error->getMessage());
            return;
          }
          m_desktopBus = std::get<0>(desktop);
          m_desktopPath = std::get<1>(desktop);
          kLog.info("accessibility tree published");
        });
    update();
  } catch (const std::exception& e) {
    failed("cannot publish the accessibility tree", e);
  }
}

void AtspiBridge::disconnect() {
  m_items.clear();
  m_ids.clear();
  m_reportedUnnamed.clear();
  m_activeWindow = 0;
  m_registry.reset();
  m_cache.reset();
  m_application.reset();
  m_connection.reset();
}

std::optional<sdbus::IConnection::PollData> AtspiBridge::pollData() const {
  if (m_connection == nullptr) {
    return std::nullopt;
  }
  try {
    return m_connection->getEventLoopPollData();
  } catch (const sdbus::Error&) {
    // dispatch() sees the same error and disconnects.
    return std::nullopt;
  }
}

void AtspiBridge::dispatch() {
  try {
    while (m_connection != nullptr && m_connection->processPendingEvent()) {
    }
    for (auto& run : std::exchange(m_deferred, {})) {
      run();
    }
  } catch (const std::exception& e) {
    failed("accessibility bus failed", e);
  }
}

std::uint32_t AtspiBridge::idFor(std::uint64_t key) {
  if (const auto it = m_ids.find(key); it != m_ids.end()) {
    return it->second;
  }
  const std::uint32_t id = m_nextId++;
  m_ids.emplace(key, id);
  return id;
}

std::vector<std::uint32_t> AtspiBridge::windowIds() const {
  std::vector<std::uint32_t> ids;
  for (const auto& [id, entry] : m_items) {
    if (entry.parent == 0) {
      ids.push_back(id);
    }
  }
  std::ranges::sort(ids);
  return ids;
}

std::int32_t AtspiBridge::indexInParent(std::uint32_t id) const {
  const Item* current = item(id);
  if (current == nullptr) {
    return -1;
  }
  const Item* parent = item(current->parent);
  const std::vector<std::uint32_t> siblings = parent != nullptr ? parent->children : windowIds();
  const auto it = std::ranges::find(siblings, id);
  return it != siblings.end() ? static_cast<std::int32_t>(it - siblings.begin()) : -1;
}

std::string AtspiBridge::pathFor(std::uint32_t id) const {
  return id == 0 ? std::string(kRootPath) : std::string(kItemPathPrefix) + std::to_string(id);
}

sdbus::Struct<std::string, sdbus::ObjectPath> AtspiBridge::refFor(std::uint32_t id) const {
  const std::string bus = m_connection != nullptr ? std::string(m_connection->getUniqueName()) : std::string();
  if (id != 0 && !m_items.contains(id)) {
    return Ref{bus, sdbus::ObjectPath{kNullPath}};
  }
  return Ref{bus, sdbus::ObjectPath{pathFor(id)}};
}

Node* AtspiBridge::liveNode(std::uint32_t id) const {
  const Item* current = item(id);
  if (current == nullptr || std::ranges::find(m_roots, current->root, &Root::node) == m_roots.end()
      || !findNode(*current->root, current->node)) {
    return nullptr;
  }
  if (const Accessible* accessible = current->node->accessible();
      accessible != nullptr && accessible->serial != current->key) {
    return nullptr;
  }
  return const_cast<Node*>(current->node);
}

const AtspiBridge::Item* AtspiBridge::item(std::uint32_t id) const {
  const auto it = m_items.find(id);
  return it != m_items.end() ? &it->second : nullptr;
}

void AtspiBridge::emitObjectEvent(
    std::uint32_t id, const std::string& member, const std::string& detail, std::int32_t detail1, std::int32_t detail2,
    const sdbus::Variant& value, const char* interfaceName
) {
  sdbus::IObject* object = id == 0 ? m_application.get() : nullptr;
  if (id != 0) {
    if (const auto it = m_items.find(id); it != m_items.end()) {
      object = it->second.object.get();
    }
  }
  if (object == nullptr) {
    return;
  }
  try {
    object->emitSignal(member)
        .onInterface(interfaceName != nullptr ? interfaceName : kEventObject)
        .withArguments(detail, detail1, detail2, value, std::map<std::string, sdbus::Variant>{});
  } catch (const sdbus::Error& e) {
    kLog.debug("AT-SPI event {} failed: {}", member, e.getMessage());
  }
}

void AtspiBridge::exportApplication() {
  m_application = sdbus::createObject(*m_connection, sdbus::ObjectPath{kRootPath});
  const auto windows = [this] {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, entry] : m_items) {
      if (entry.parent == 0) {
        ids.push_back(id);
      }
    }
    std::ranges::sort(ids);
    return ids;
  };
  m_application
      ->addVTable(
          sdbus::registerProperty("Name").withGetter([this] { return m_appName; }),
          sdbus::registerProperty("Description").withGetter([] { return std::string(); }),
          sdbus::registerProperty("Parent").withGetter([this] { return Ref{m_desktopBus, sdbus::ObjectPath{m_desktopPath}}; }),
          sdbus::registerProperty("ChildCount").withGetter([windows] {
            return static_cast<std::int32_t>(windows().size());
          }),
          sdbus::registerProperty("Locale").withGetter([] { return std::string("en_US"); }),
          sdbus::registerProperty("AccessibleId").withGetter([] { return std::string(); }),
          sdbus::registerProperty("HelpText").withGetter([] { return std::string(); }),
          sdbus::registerMethod("GetChildAtIndex").implementedAs([this, windows](std::int32_t index) {
            const auto ids = windows();
            return index >= 0 && static_cast<std::size_t>(index) < ids.size() ? refFor(ids[static_cast<std::size_t>(index)])
                                                                                : refFor(UINT32_MAX);
          }),
          sdbus::registerMethod("GetChildren").implementedAs([this, windows] {
            std::vector<Ref> refs;
            for (const std::uint32_t id : windows()) {
              refs.push_back(refFor(id));
            }
            return refs;
          }),
          sdbus::registerMethod("GetIndexInParent").implementedAs([] { return std::int32_t{-1}; }),
          sdbus::registerMethod("GetRelationSet").implementedAs([] {
            return std::vector<sdbus::Struct<std::uint32_t, std::vector<Ref>>>{};
          }),
          sdbus::registerMethod("GetRole").implementedAs([] { return kRoleApplication; }),
          sdbus::registerMethod("GetRoleName").implementedAs([] { return std::string("application"); }),
          sdbus::registerMethod("GetLocalizedRoleName").implementedAs([] { return std::string("application"); }),
          sdbus::registerMethod("GetState").implementedAs([] { return stateWords(0); }),
          sdbus::registerMethod("GetAttributes").implementedAs([] {
            return std::map<std::string, std::string>{{"toolkit", "libdesk-ui"}};
          }),
          sdbus::registerMethod("GetApplication").implementedAs([this] { return refFor(0); }),
          sdbus::registerMethod("GetInterfaces").implementedAs([] {
            return std::vector<std::string>{kAccessible, kApplication};
          })
      )
      .forInterface(kAccessible);
  m_application
      ->addVTable(
          sdbus::registerProperty("ToolkitName").withGetter([] { return std::string("libdesk-ui"); }),
          sdbus::registerProperty("Version").withGetter([] { return std::string("1"); }),
          sdbus::registerProperty("AtspiVersion").withGetter([] { return std::string("2.1"); }),
          sdbus::registerProperty("Id")
              .withGetter([this] { return m_applicationId; })
              .withSetter([this](std::int32_t id) { m_applicationId = id; }),
          sdbus::registerMethod("GetLocale").implementedAs([](std::uint32_t /*type*/) { return std::string("en_US"); })
      )
      .forInterface(kApplication);
}

void AtspiBridge::exportCache() {
  using CacheItem = sdbus::Struct<
      Ref, Ref, Ref, std::int32_t, std::int32_t, std::vector<std::string>, std::string, std::uint32_t, std::string,
      std::vector<std::uint32_t>>;
  m_cache = sdbus::createObject(*m_connection, sdbus::ObjectPath{kCachePath});
  m_cache
      ->addVTable(sdbus::registerMethod("GetItems").implementedAs([this] {
        std::vector<CacheItem> items;
        for (const auto& [id, entry] : m_items) {
          std::vector<std::string> interfaces{kAccessible, kComponent};
          if (!entry.actionName.empty()) {
            interfaces.emplace_back(kAction);
          }
          if (entry.value.has_value()) {
            interfaces.emplace_back(kValue);
          }
          if (entry.text.has_value()) {
            interfaces.emplace_back(kText);
          }
          items.emplace_back(
              refFor(id), refFor(0), refFor(entry.parent), indexInParent(id),
              static_cast<std::int32_t>(entry.children.size()), std::move(interfaces), entry.name, atspiRole(entry.role),
              entry.description, stateWords(entry.states)
          );
        }
        return items;
      }))
      .forInterface(kCache);
}

void AtspiBridge::exportItem(Item& entry) {
  const std::uint32_t id = entry.id;
  entry.object = sdbus::createObject(*m_connection, sdbus::ObjectPath{pathFor(id)});
  const auto self = [this, id] { return item(id); };
  entry.object
      ->addVTable(
          sdbus::registerProperty("Name").withGetter([self] { return self() != nullptr ? self()->name : std::string(); }),
          sdbus::registerProperty("Description").withGetter([self] {
            return self() != nullptr ? self()->description : std::string();
          }),
          sdbus::registerProperty("Parent").withGetter([this, self] {
            return self() != nullptr ? refFor(self()->parent) : refFor(0);
          }),
          sdbus::registerProperty("ChildCount").withGetter([self] {
            return self() != nullptr ? static_cast<std::int32_t>(self()->children.size()) : 0;
          }),
          sdbus::registerProperty("Locale").withGetter([] { return std::string("en_US"); }),
          sdbus::registerProperty("AccessibleId").withGetter([id] { return std::to_string(id); }),
          sdbus::registerProperty("HelpText").withGetter([] { return std::string(); }),
          sdbus::registerMethod("GetChildAtIndex").implementedAs([this, self](std::int32_t index) {
            const Item* current = self();
            if (current == nullptr || index < 0 || static_cast<std::size_t>(index) >= current->children.size()) {
              return refFor(UINT32_MAX);
            }
            return refFor(current->children[static_cast<std::size_t>(index)]);
          }),
          sdbus::registerMethod("GetChildren").implementedAs([this, self] {
            std::vector<Ref> refs;
            if (const Item* current = self(); current != nullptr) {
              for (const std::uint32_t child : current->children) {
                refs.push_back(refFor(child));
              }
            }
            return refs;
          }),
          sdbus::registerMethod("GetIndexInParent").implementedAs([this, id] { return indexInParent(id); }),
          sdbus::registerMethod("GetRelationSet").implementedAs([] {
            return std::vector<sdbus::Struct<std::uint32_t, std::vector<Ref>>>{};
          }),
          sdbus::registerMethod("GetRole").implementedAs([self] {
            return self() != nullptr ? atspiRole(self()->role) : atspiRole(AccessibleRole::Panel);
          }),
          sdbus::registerMethod("GetRoleName").implementedAs([self] {
            return roleName(self() != nullptr ? self()->role : AccessibleRole::Panel);
          }),
          sdbus::registerMethod("GetLocalizedRoleName").implementedAs([self] {
            return roleName(self() != nullptr ? self()->role : AccessibleRole::Panel);
          }),
          sdbus::registerMethod("GetState").implementedAs([self] {
            return stateWords(self() != nullptr ? self()->states : 0);
          }),
          sdbus::registerMethod("GetAttributes").implementedAs([] {
            return std::map<std::string, std::string>{{"toolkit", "libdesk-ui"}};
          }),
          sdbus::registerMethod("GetApplication").implementedAs([this] { return refFor(0); }),
          sdbus::registerMethod("GetInterfaces").implementedAs([self] {
            std::vector<std::string> interfaces{kAccessible, kComponent};
            if (const Item* current = self(); current != nullptr) {
              if (!current->actionName.empty()) {
                interfaces.emplace_back(kAction);
              }
              if (current->value.has_value()) {
                interfaces.emplace_back(kValue);
              }
              if (current->text.has_value()) {
                interfaces.emplace_back(kText);
              }
            }
            return interfaces;
          })
      )
      .forInterface(kAccessible);

  // Wayland gives clients no global position: screen and window coordinates are both surface-local.
  const auto extents = [self] {
    const Item* current = self();
    return current != nullptr
        ? sdbus::Struct<std::int32_t, std::int32_t, std::int32_t, std::int32_t>{current->x, current->y,
                                                                                current->width, current->height}
        : sdbus::Struct<std::int32_t, std::int32_t, std::int32_t, std::int32_t>{0, 0, 0, 0};
  };
  entry.object
      ->addVTable(
          sdbus::registerMethod("Contains").implementedAs([self](std::int32_t x, std::int32_t y, std::uint32_t) {
            const Item* current = self();
            return current != nullptr && x >= current->x && y >= current->y && x < current->x + current->width
                && y < current->y + current->height;
          }),
          sdbus::registerMethod("GetAccessibleAtPoint")
              .implementedAs([this, self](std::int32_t x, std::int32_t y, std::uint32_t) {
                // Deepest descendant under the point, depth-first in paint order (last child on top).
                std::uint32_t found = UINT32_MAX;
                std::vector<std::uint32_t> stack;
                if (const Item* current = self(); current != nullptr) {
                  stack = current->children;
                }
                while (!stack.empty()) {
                  const std::uint32_t candidate = stack.back();
                  stack.pop_back();
                  const Item* child = item(candidate);
                  if (child == nullptr || x < child->x || y < child->y || x >= child->x + child->width
                      || y >= child->y + child->height) {
                    continue;
                  }
                  found = candidate;
                  stack = child->children;
                }
                return refFor(found);
              }),
          sdbus::registerMethod("GetExtents").implementedAs([extents](std::uint32_t) { return extents(); }),
          sdbus::registerMethod("GetPosition").implementedAs([self](std::uint32_t) {
            const Item* current = self();
            return current != nullptr ? std::make_tuple(current->x, current->y) : std::make_tuple(0, 0);
          }),
          sdbus::registerMethod("GetSize").implementedAs([self] {
            const Item* current = self();
            return current != nullptr ? std::make_tuple(current->width, current->height) : std::make_tuple(0, 0);
          }),
          sdbus::registerMethod("GrabFocus").implementedAs([this, id] {
            Node* node = liveNode(id);
            InputArea* area = node != nullptr ? accessibleInputSource(*node) : nullptr;
            if (area == nullptr || InputDispatcher::owning(area) == nullptr) {
              return false;
            }
            m_deferred.emplace_back([this, id] {
              Node* current = liveNode(id);
              InputArea* target = current != nullptr ? accessibleInputSource(*current) : nullptr;
              if (InputDispatcher* dispatcher = target != nullptr ? InputDispatcher::owning(target) : nullptr) {
                dispatcher->setFocus(target);
                markDirty();
              }
            });
            return true;
          })
      )
      .forInterface(kComponent);

  if (!entry.actionName.empty()) {
    entry.object
        ->addVTable(
            sdbus::registerProperty("NActions").withGetter([] { return std::int32_t{1}; }),
            sdbus::registerMethod("GetName").implementedAs([self](std::int32_t) {
              return self() != nullptr ? self()->actionName : std::string();
            }),
            sdbus::registerMethod("GetDescription").implementedAs([](std::int32_t) { return std::string(); }),
            sdbus::registerMethod("GetLocalizedName").implementedAs([self](std::int32_t) {
              return self() != nullptr ? self()->actionName : std::string();
            }),
            sdbus::registerMethod("GetKeyBinding").implementedAs([](std::int32_t) { return std::string(); }),
            sdbus::registerMethod("GetActions").implementedAs([self] {
              const std::string name = self() != nullptr ? self()->actionName : std::string();
              return std::vector<sdbus::Struct<std::string, std::string, std::string>>{{name, name, ""}};
            }),
            sdbus::registerMethod("DoAction").implementedAs([this, id](std::int32_t index) {
              Node* node = index == 0 ? liveNode(id) : nullptr;
              if (node == nullptr || accessibleInputSource(*node) == nullptr) {
                return false;
              }
              m_deferred.emplace_back([this, id] {
                Node* current = liveNode(id);
                if (InputArea* area = current != nullptr ? accessibleInputSource(*current) : nullptr) {
                  area->activate();
                  markDirty();
                }
              });
              return true;
            })
        )
        .forInterface(kAction);
  }
  if (entry.value.has_value()) {
    const auto value = [self] { return self() != nullptr ? self()->value.value_or(AccessibleValue{}) : AccessibleValue{}; };
    entry.object
        ->addVTable(
            sdbus::registerProperty("MinimumValue").withGetter([value] { return value().minimum; }),
            sdbus::registerProperty("MaximumValue").withGetter([value] { return value().maximum; }),
            sdbus::registerProperty("MinimumIncrement").withGetter([value] { return value().step; }),
            sdbus::registerProperty("CurrentValue")
                .withGetter([value] { return value().current; })
                .withSetter([this, id](double requested) {
                  m_deferred.emplace_back([this, id, requested] {
                    Node* node = liveNode(id);
                    const Accessible* accessible = node != nullptr ? node->accessible() : nullptr;
                    if (accessible != nullptr && accessible->setValue && accessible->value.has_value()) {
                      const AccessibleValue& range = *accessible->value;
                      accessible->setValue(std::clamp(requested, range.minimum, range.maximum));
                      markDirty();
                    }
                  });
                }),
            sdbus::registerProperty("Text").withGetter([] { return std::string(); })
        )
        .forInterface(kValue);
  }
  if (entry.text.has_value()) {
    const auto text = [self] { return self() != nullptr ? self()->text.value_or(std::string()) : std::string(); };
    entry.object
        ->addVTable(
            sdbus::registerProperty("CharacterCount").withGetter([text] { return charCount(text()); }),
            sdbus::registerProperty("CaretOffset").withGetter([self] {
              return self() != nullptr ? self()->caretOffset : std::int32_t{0};
            }),
            sdbus::registerMethod("GetText").implementedAs([text](std::int32_t start, std::int32_t end) {
              const std::string all = text();
              const std::size_t from = byteOffset(all, std::max(0, start));
              const std::size_t to = end < 0 ? all.size() : byteOffset(all, end);
              return from < to ? all.substr(from, to - from) : std::string();
            }),
            sdbus::registerMethod("SetCaretOffset").implementedAs([](std::int32_t) { return false; }),
            sdbus::registerMethod("GetTextAtOffset").implementedAs([text](std::int32_t offset, std::uint32_t boundary) {
              // AtspiTextBoundaryType: 0 char, 1-2 word; sentence and line boundaries return the whole field.
              return textAt(text(), offset, boundary == 0 ? 0U : boundary <= 2 ? 1U : 3U);
            }),
            sdbus::registerMethod("GetStringAtOffset").implementedAs([text](std::int32_t offset, std::uint32_t granularity) {
              // AtspiTextGranularity: 0 char, 1 word, 2+ whole field.
              return textAt(text(), offset, std::min(granularity, 3U));
            }),
            sdbus::registerMethod("GetCharacterAtOffset").implementedAs([text](std::int32_t offset) {
              const auto [ch, start, end] = textAt(text(), offset, 0);
              std::int32_t code = 0;
              if (!ch.empty()) {
                const auto lead = static_cast<unsigned char>(ch[0]);
                const int extra = lead >= 0xF0U ? 3 : lead >= 0xE0U ? 2 : lead >= 0xC0U ? 1 : 0;
                code = extra == 0 ? lead : static_cast<std::int32_t>(lead & (0x3FU >> extra));
                for (int k = 1; k <= extra && k < static_cast<int>(ch.size()); ++k) {
                  code = (code << 6) | (static_cast<unsigned char>(ch[static_cast<std::size_t>(k)]) & 0x3FU);
                }
              }
              return code;
            })
        )
        .forInterface(kText);
  }
}

void AtspiBridge::update() {
  m_dirty = false;
  if (m_connection == nullptr) {
    return;
  }
  try {
    updateTree();
  } catch (const std::exception& e) {
    failed("accessibility update failed", e);
  }
}

void AtspiBridge::updateTree() {
  std::unordered_map<std::uint32_t, std::int32_t> oldIndex;
  for (const auto& [id, entry] : m_items) {
    oldIndex[id] = indexInParent(id);
  }
  std::unordered_set<std::uint32_t> seen;
  std::uint32_t activeWindow = 0;
  for (const Root& root : m_roots) {
    const std::string title = root.title ? root.title() : std::string();
    const std::vector<AccessibleEntry> tree = buildAccessibleTree(*root.node);
    std::vector<std::uint64_t> keys(tree.size());
    std::vector<std::uint32_t> ids(tree.size());
    for (std::size_t i = 0; i < tree.size(); ++i) {
      const Accessible* accessible = tree[i].node->accessible();
      // A root without a record is keyed by address: it stays registered (and alive) until removeRoot().
      keys[i] = accessible != nullptr ? accessible->serial
                                      : (std::uint64_t{1} << 63U) | reinterpret_cast<std::uintptr_t>(tree[i].node);
      ids[i] = idFor(keys[i]);
    }
    const bool rootFocused = std::ranges::any_of(tree, &AccessibleEntry::focused);
    if (rootFocused && activeWindow == 0) {
      activeWindow = ids[0];
    }
    for (std::size_t i = 0; i < tree.size(); ++i) {
      const AccessibleEntry& entry = tree[i];
      const Accessible fallback{.role = AccessibleRole::Window, .name = title};
      const Accessible* accessible = entry.node->accessible();
      if (accessible == nullptr || accessible->role == AccessibleRole::None) {
        accessible = &fallback;
      }
      Item next;
      next.id = ids[i];
      next.key = keys[i];
      next.parent = entry.parent < 0 ? 0 : ids[static_cast<std::size_t>(entry.parent)];
      for (const int child : entry.children) {
        next.children.push_back(ids[static_cast<std::size_t>(child)]);
      }
      next.node = entry.node;
      next.root = root.node;
      next.role = accessible->role;
      next.name = i == 0 && accessible->name.empty() ? title : accessible->name;
      next.description = accessible->description;
      next.value = accessible->value;
      next.actionName = accessible->actionName;
      next.text = accessible->text;
      next.caretOffset = accessible->caretOffset;
      next.states = statesFor(*accessible, entry);
      if (i == 0 && rootFocused) {
        next.states |= bit(kActive);
      }
      next.x = px(entry.x);
      next.y = px(entry.y);
      next.width = px(entry.width);
      next.height = px(entry.height);
      seen.insert(next.id);
      if (m_strict && (next.states & bit(kShowing)) != 0 && next.name.empty() && accessibleRoleNeedsName(next.role) && !m_reportedUnnamed[next.id]) {
        m_reportedUnnamed[next.id] = true;
        kLog.warn("a11y strict: unnamed {} in window \"{}\"", roleName(next.role), title);
      }

      auto it = m_items.find(next.id);
      const bool created = it == m_items.end() || it->second.role != next.role
          || it->second.actionName.empty() != next.actionName.empty()
          || it->second.value.has_value() != next.value.has_value()
          || it->second.text.has_value() != next.text.has_value();
      if (created) {
        if (it != m_items.end()) {
          emitObjectEvent(it->second.parent, "ChildrenChanged", "remove", oldIndex[next.id], 0,
                          sdbus::Variant{refFor(next.id)});
          m_items.erase(it);
        }
        it = m_items.emplace(next.id, std::move(next)).first;
        Item& added = it->second;
        exportItem(added);
        emitObjectEvent(added.parent, "ChildrenChanged", "add", indexInParent(added.id), 0,
                        sdbus::Variant{refFor(added.id)});
        // Announce states the object already has, so a field focused in its first frame is read.
        for (const auto& [state, detail] : {std::pair{kFocused, "focused"}, std::pair{kSelected, "selected"},
                                            std::pair{kChecked, "checked"}}) {
          if ((added.states & bit(state)) != 0) {
            emitObjectEvent(added.id, "StateChanged", detail, 1, 0, sdbus::Variant{std::int32_t{0}});
          }
        }
        continue;
      }
      Item& previous = it->second;
      const std::uint64_t changed = previous.states ^ next.states;
      const bool nameChanged = previous.name != next.name;
      const bool valueChanged = next.value.has_value() && previous.value.has_value()
          && previous.value->current != next.value->current;
      const std::string oldText = previous.text.value_or(std::string());
      const bool textChanged = previous.text != next.text;
      const bool caretMoved = next.text.has_value() && previous.caretOffset != next.caretOffset;
      next.object = std::move(previous.object);
      previous = std::move(next);
      if (nameChanged) {
        emitObjectEvent(previous.id, "PropertyChange", "accessible-name", 0, 0, sdbus::Variant{previous.name});
      }
      if (textChanged) {
        // Report only the changed range, so a screen reader echoes the typed character, not the whole field.
        const std::string now = previous.text.value_or(std::string());
        const auto continuation = [](const std::string& text, std::size_t at) {
          return at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0U) == 0x80U;
        };
        std::size_t prefix = 0;
        while (prefix < oldText.size() && prefix < now.size() && oldText[prefix] == now[prefix]) {
          ++prefix;
        }
        while (prefix > 0 && (continuation(oldText, prefix) || continuation(now, prefix))) {
          --prefix;
        }
        std::size_t suffix = 0;
        while (suffix < oldText.size() - prefix && suffix < now.size() - prefix
               && oldText[oldText.size() - 1 - suffix] == now[now.size() - 1 - suffix]) {
          ++suffix;
        }
        while (suffix > 0 && (continuation(oldText, oldText.size() - suffix) || continuation(now, now.size() - suffix))) {
          --suffix;
        }
        const std::string removed = oldText.substr(prefix, oldText.size() - prefix - suffix);
        const std::string inserted = now.substr(prefix, now.size() - prefix - suffix);
        const std::int32_t at = charCount(now.substr(0, prefix));
        if (!removed.empty()) {
          emitObjectEvent(previous.id, "TextChanged", "delete", at, charCount(removed), sdbus::Variant{removed});
        }
        if (!inserted.empty()) {
          emitObjectEvent(previous.id, "TextChanged", "insert", at, charCount(inserted), sdbus::Variant{inserted});
        }
      }
      if (caretMoved) {
        emitObjectEvent(previous.id, "TextCaretMoved", "", previous.caretOffset, 0, sdbus::Variant{std::int32_t{0}});
      }
      if (valueChanged) {
        emitObjectEvent(previous.id, "PropertyChange", "accessible-value", 0, 0, sdbus::Variant{previous.value->current});
      }
      for (const auto& [state, detail] : {std::pair{kFocused, "focused"}, std::pair{kChecked, "checked"},
                                          std::pair{kSelected, "selected"}, std::pair{kShowing, "showing"},
                                          std::pair{kExpanded, "expanded"}, std::pair{kEnabled, "enabled"},
                                          std::pair{kActive, "active"}}) {
        if ((changed & bit(state)) != 0) {
          const std::int32_t on = (previous.states & bit(state)) != 0 ? 1 : 0;
          emitObjectEvent(previous.id, "StateChanged", detail, on, 0, sdbus::Variant{std::int32_t{0}});
        }
      }
    }
  }
  std::vector<std::uint32_t> gone;
  for (const auto& [id, entry] : m_items) {
    if (!seen.contains(id)) {
      gone.push_back(id);
    }
  }
  for (const std::uint32_t id : gone) {
    const Item& removed = m_items.at(id);
    emitObjectEvent(removed.parent, "ChildrenChanged", "remove", oldIndex[id], 0, sdbus::Variant{refFor(id)});
    m_ids.erase(removed.key);
    m_items.erase(id);
  }
  if (activeWindow != m_activeWindow) {
    if (m_items.contains(m_activeWindow)) {
      emitObjectEvent(m_activeWindow, "Deactivate", "", 0, 0, sdbus::Variant{std::string()}, kEventWindow);
    }
    if (activeWindow != 0) {
      emitObjectEvent(activeWindow, "Activate", "", 0, 0, sdbus::Variant{std::string()}, kEventWindow);
    }
    m_activeWindow = activeWindow;
  }
}

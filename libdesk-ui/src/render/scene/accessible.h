#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class InputArea;
class Node;

// What a node is to assistive technology. The AT-SPI bridge maps these to AT-SPI roles.
enum class AccessibleRole : std::uint8_t {
  None,
  Window,
  Panel,
  Button,
  ToggleButton,
  CheckBox,
  RadioButton,
  Slider,
  SpinButton,
  List,
  ListItem,
  TextField,
  Label,
  Image,
  Menu,
  MenuItem,
  Dialog,
  TabList,
  Tab,
  ScrollPane,
  ProgressBar,
  Separator,
};

// Roles a user acts on: they must have a name (DESK_A11Y_STRICT=1 checks it).
[[nodiscard]] bool accessibleRoleNeedsName(AccessibleRole role) noexcept;

struct AccessibleValue {
  double current = 0.0;
  double minimum = 0.0;
  double maximum = 1.0;
  double step = 0.0;
};

struct Accessible {
  AccessibleRole role = AccessibleRole::None;
  std::string name;
  std::string description;
  std::optional<AccessibleValue> value;
  std::optional<bool> checked;
  bool selected = false;
  bool expanded = false;
  bool expandable = false;
  bool disabled = false;
  // Runs the default action (press, toggle, activate). Empty = no action.
  std::string actionName;
  // Text content of an editable or read-only text object; caretOffset counts characters.
  std::optional<std::string> text;
  std::int32_t caretOffset = 0;
  // Sets `value` on a user request from assistive technology (as a committed drag or key step).
  std::function<void(double)> setValue;
  // Set by Node: unique per node for the process lifetime, so a freed node's address reuse is not the same object.
  std::uint64_t serial = 0;
};

// One accessible object of a surface, in tree order. Nodes without a role are left out; their accessible
// descendants join the nearest accessible ancestor.
struct AccessibleEntry {
  const Node* node = nullptr;
  int parent = -1;
  std::vector<int> children;
  // Surface-local logical coordinates.
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  bool focusable = false;
  bool focused = false;
  bool showing = false;
};

// Entry 0 is `root` itself (role taken from it, or Window when it has none).
[[nodiscard]] std::vector<AccessibleEntry> buildAccessibleTree(const Node& root);

// The input area that holds focus or clicks for `node`: the first one in its subtree outside other exposed nodes.
[[nodiscard]] InputArea* accessibleInputSource(Node& node);

// Entries with a role in accessibleRoleNeedsName() and an empty name.
[[nodiscard]] std::vector<const Node*> unnamedAccessibles(const std::vector<AccessibleEntry>& tree);

#include "render/scene/accessible.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"

namespace {

  // Controls whose content is their name: their descendants are not exposed (a button's label, a row's icon).
  bool isLeafRole(AccessibleRole role) noexcept {
    switch (role) {
    case AccessibleRole::Button:
    case AccessibleRole::ToggleButton:
    case AccessibleRole::CheckBox:
    case AccessibleRole::RadioButton:
    case AccessibleRole::Slider:
    case AccessibleRole::SpinButton:
    case AccessibleRole::ListItem:
    case AccessibleRole::TextField:
    case AccessibleRole::Label:
    case AccessibleRole::Image:
    case AccessibleRole::MenuItem:
    case AccessibleRole::Tab:
    case AccessibleRole::ProgressBar:
    case AccessibleRole::Separator:
      return true;
    default:
      return false;
    }
  }

} // namespace

bool accessibleRoleNeedsName(AccessibleRole role) noexcept {
  switch (role) {
  case AccessibleRole::Button:
  case AccessibleRole::ToggleButton:
  case AccessibleRole::CheckBox:
  case AccessibleRole::RadioButton:
  case AccessibleRole::Slider:
  case AccessibleRole::SpinButton:
  case AccessibleRole::ListItem:
  case AccessibleRole::TextField:
  case AccessibleRole::MenuItem:
  case AccessibleRole::Dialog:
  case AccessibleRole::Tab:
  case AccessibleRole::Window:
    return true;
  default:
    return false;
  }
}

namespace {

  bool exposed(const Node& node) {
    const Accessible* accessible = node.accessible();
    return accessible != nullptr && accessible->role != AccessibleRole::None;
  }

  InputArea* asInput(Node& node) {
    auto* area = dynamic_cast<InputArea*>(&node);
    return area != nullptr && (area->focusable() || area->clickable()) ? area : nullptr;
  }

  InputArea* inputBelow(Node& node) {
    if (InputArea* area = asInput(node); area != nullptr) {
      return area;
    }
    for (const auto& child : node.children()) {
      if (exposed(*child)) {
        continue;
      }
      if (InputArea* area = inputBelow(*child); area != nullptr) {
        return area;
      }
    }
    return nullptr;
  }

  // Input areas are often children of the node with the role, or wrap it (a list row inside its click area).
  InputArea* inputSource(Node& node) {
    if (InputArea* area = inputBelow(node); area != nullptr) {
      return area;
    }
    for (Node* parent = node.parent(); parent != nullptr && !exposed(*parent); parent = parent->parent()) {
      if (InputArea* area = asInput(*parent); area != nullptr) {
        return area;
      }
    }
    return nullptr;
  }

  const InputArea* focusSource(const Node& node) { return inputSource(const_cast<Node&>(node)); }

  void collect(const Node& node, int parent, bool parentShowing, std::vector<AccessibleEntry>& out) {
    const bool showing = parentShowing && node.visible() && node.opacity() > 0.0F;
    int owner = parent;
    // An empty label says nothing: leave it out.
    if (const Accessible* accessible = node.accessible(); accessible != nullptr && accessible->role != AccessibleRole::None
        && !(accessible->role == AccessibleRole::Label && accessible->name.empty())) {
      AccessibleEntry entry;
      entry.node = &node;
      entry.parent = parent;
      Node::absolutePosition(&node, entry.x, entry.y);
      entry.width = node.width();
      entry.height = node.height();
      entry.showing = showing;
      if (const InputArea* area = focusSource(node); area != nullptr) {
        entry.focusable = area->focusable();
        entry.focused = area->focused();
      }
      owner = static_cast<int>(out.size());
      out.push_back(std::move(entry));
      out[static_cast<std::size_t>(parent)].children.push_back(owner);
    }
    if (owner != parent && isLeafRole(node.accessible()->role)) {
      return;
    }
    for (const auto& child : node.children()) {
      collect(*child, owner, showing, out);
    }
  }

} // namespace

std::vector<AccessibleEntry> buildAccessibleTree(const Node& root) {
  std::vector<AccessibleEntry> tree;
  AccessibleEntry rootEntry;
  rootEntry.node = &root;
  Node::absolutePosition(&root, rootEntry.x, rootEntry.y);
  rootEntry.width = root.width();
  rootEntry.height = root.height();
  rootEntry.showing = root.visible();
  tree.push_back(std::move(rootEntry));
  for (const auto& child : root.children()) {
    collect(*child, 0, tree.front().showing, tree);
  }
  return tree;
}

InputArea* accessibleInputSource(Node& node) { return inputSource(node); }

std::vector<const Node*> unnamedAccessibles(const std::vector<AccessibleEntry>& tree) {
  std::vector<const Node*> unnamed;
  for (const AccessibleEntry& entry : tree) {
    const Accessible* accessible = entry.node->accessible();
    if (accessible != nullptr && accessibleRoleNeedsName(accessible->role) && accessible->name.empty()) {
      unnamed.push_back(entry.node);
    }
  }
  return unnamed;
}

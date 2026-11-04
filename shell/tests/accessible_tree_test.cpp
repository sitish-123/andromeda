#include "render/scene/accessible.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "tests/test_check.h"

#include <memory>

// The bridge walks this tree: unexposed containers vanish, their exposed children join the nearest exposed
// ancestor, focus comes from the input area under an exposed node, hidden branches are not showing.
int main() {
  Node root;
  root.setAccessible({.role = AccessibleRole::Window, .name = "Launcher"});

  auto* box = root.addChild(std::make_unique<Node>());
  box->setPosition(10.0F, 20.0F);

  auto* list = box->addChild(std::make_unique<Node>());
  list->setAccessible({.role = AccessibleRole::List});

  auto* row = list->addChild(std::make_unique<Node>());
  row->setAccessible({.role = AccessibleRole::ListItem, .name = "Firefox"});
  auto* area = static_cast<InputArea*>(row->addChild(std::make_unique<InputArea>()));
  area->setFocusable(true);

  auto* unnamed = list->addChild(std::make_unique<Node>());
  unnamed->setAccessible({.role = AccessibleRole::Button});

  auto* hidden = root.addChild(std::make_unique<Node>());
  hidden->setVisible(false);
  auto* hiddenLabel = hidden->addChild(std::make_unique<Node>());
  hiddenLabel->setAccessible({.role = AccessibleRole::Label, .name = "Hint"});

  const std::vector<AccessibleEntry> tree = buildAccessibleTree(root);
  TEST_CHECK(tree.size() == 5);
  TEST_CHECK(tree[0].node == &root);
  TEST_CHECK(tree[1].node == list && tree[1].parent == 0);
  TEST_CHECK(tree[2].node == row && tree[2].parent == 1);
  TEST_CHECK(tree[3].node == unnamed && tree[3].parent == 1);
  TEST_CHECK(tree[4].node == hiddenLabel && tree[4].parent == 0);
  TEST_CHECK((tree[0].children == std::vector<int>{1, 4}));
  TEST_CHECK((tree[1].children == std::vector<int>{2, 3}));
  TEST_CHECK(tree[1].x == 10.0F && tree[1].y == 20.0F);
  TEST_CHECK(tree[2].focusable && !tree[2].focused);
  TEST_CHECK(!tree[3].focusable);
  TEST_CHECK(tree[2].showing && !tree[4].showing);

  const std::vector<const Node*> missing = unnamedAccessibles(tree);
  TEST_CHECK(missing.size() == 1 && missing.front() == unnamed);
  return 0;
}

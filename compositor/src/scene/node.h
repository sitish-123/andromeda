#pragma once

#include <cstdint>

namespace umbriel {

  enum class SceneNodeKind : uint8_t {
    View,
    LayerSurface,
    LockSurface,
  };

  // Stored in wlr_scene_node::data so hit-testing can tell views from layers
  // (including xdg popups parented to a layer surface).
  struct SceneNode {
    // Distinguishes our tagged nodes from any other user data that ends up in
    // wlr_scene_node::data. "UMBS".
    static constexpr uint32_t kMagic = 0x554D4253;

    explicit SceneNode(SceneNodeKind kind) : kind(kind) {}

    uint32_t magic = kMagic;
    SceneNodeKind kind;
  };

  // Produces the pointer to store in wlr_scene_node::data. Always store through this rather than assigning `this`
  // directly. A derived class that also inherits a polymorphic base does not begin with its SceneNode subobject: the
  // vptr takes offset 0 and SceneNode moves down, so a raw `this` would not round-trip through sceneNodeFrom. Passing
  // `this` here makes the compiler apply the offset as an ordinary argument conversion.
  [[nodiscard]] inline void* sceneNodeData(SceneNode* node) { return node; }

  // Recovers a SceneNode from a wlr_scene_node::data pointer, or null when the pointer is something else. Every node
  // the compositor tags sets the sentinel, so this catches the case that used to be silent: a wlroots or SceneFX helper
  // stashing its own user data in a node we then walk over, previously reinterpreted as a SceneNode and dereferenced.
  // It is a guard, not a proof. Reading `magic` through a foreign pointer is only safe because anything in that field
  // points at a live object; the sentinel then makes the mismatch detectable rather than catastrophic.
  [[nodiscard]] inline SceneNode* sceneNodeFrom(void* data) {
    auto* node = static_cast<SceneNode*>(data);
    if (node == nullptr || node->magic != SceneNode::kMagic) {
      return nullptr;
    }
    return node;
  }

} // namespace umbriel

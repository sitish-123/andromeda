#include "scripting/ui_tree_reconciler_internal.h"

namespace ui {
  using namespace ui_tree_detail;

  UiTreeReconciler::UiTreeReconciler()
      : m_defaultFontWeight(FontWeight::Normal), m_dragDropController(std::make_unique<DragDropController>()) {
    m_dragDropController->setDropCallback([this](std::string callback, std::string payload, std::string target) {
      if (m_sink) {
        m_sink(ControlCallback{std::move(callback), std::move(payload), std::move(target), false});
      }
    });
  }

  UiTreeReconciler::~UiTreeReconciler() { reset(); }

  void UiTreeReconciler::setScale(float scale) {
    m_scale = scale;
    m_dragDropController->setScale(scale);
  }

  void UiTreeReconciler::setFontScale(float scale) { m_fontScale = scale; }

  void UiTreeReconciler::setDragDropEnabled(bool enabled) {
    if (m_dragDropEnabled == enabled) {
      return;
    }
    if (!enabled) {
      m_dragDropController->cancel();
    }
    m_dragDropEnabled = enabled;
  }

  void UiTreeReconciler::setDragDropOverlayRoot(Node* root) { m_dragDropController->setOverlayRoot(root); }

  void UiTreeReconciler::cancelDragDrop() { m_dragDropController->cancel(); }

  bool UiTreeReconciler::reconcile(Flex& host, const UiTreeNode& tree, Renderer& renderer) {
    std::vector<UiTreeNode> desired;
    desired.push_back(tree); // single root child of the host container
    return syncChildren(host, m_rootSlots, desired, renderer);
  }

  void UiTreeReconciler::reset() {
    m_dragDropController->cancel();
    // The host tree is gone, so any hover or graph pointer session it was
    // reporting has ended. The slots still name it; the Nodes they point at are
    // already freed.
    closeHover();
    closePointer();
    m_rootSlots.clear();
  }

  std::unique_ptr<Node> UiTreeReconciler::createControl(const UiTreeNode& desired) {
    if (isDragDropType(desired.type) && !m_dragDropEnabled) {
      kLog.error("ui tree: '{}' is only supported in plugin panels and bar widgets; node skipped", desired.type);
      return nullptr;
    }
    // ui.* flex containers default to stretching children across the cross axis
    // (like CSS flexbox), so a column fills its width without every plugin having
    // to say so. Override per node with `align`. (Flex's own C++ default is
    // Center, which the native shell relies on — only the ui.* layer changes.)
    if (desired.type == "column") {
      auto flex = std::make_unique<Flex>();
      flex->setDirection(FlexDirection::Vertical);
      flex->setAlign(FlexAlign::Stretch);
      if (wantsInputAreaWrapper(desired)) {
        return wrapClickable(std::move(flex), callbackProp(desired, "onClick") != nullptr);
      }
      return flex;
    }
    if (desired.type == "row") {
      auto flex = std::make_unique<Flex>();
      flex->setDirection(FlexDirection::Horizontal);
      flex->setAlign(FlexAlign::Stretch);
      if (wantsInputAreaWrapper(desired)) {
        return wrapClickable(std::move(flex), callbackProp(desired, "onClick") != nullptr);
      }
      return flex;
    }
    if (desired.type == "drag_source") {
      auto source = std::make_unique<DragSource>(m_dragDropController.get());
      source->setDirection(FlexDirection::Horizontal);
      source->setAlign(FlexAlign::Stretch);
      return source;
    }
    if (desired.type == "drop_zone") {
      auto zone = std::make_unique<DropZone>(m_dragDropController.get());
      zone->setDirection(FlexDirection::Vertical);
      zone->setAlign(FlexAlign::Stretch);
      return zone;
    }
    if (desired.type == "box") {
      if (wantsInputAreaWrapper(desired)) {
        return wrapClickable(std::make_unique<Box>(), callbackProp(desired, "onClick") != nullptr);
      }
      return std::make_unique<Box>();
    }
    if (desired.type == "label") {
      return std::make_unique<Label>();
    }
    if (desired.type == "glyph") {
      return std::make_unique<Glyph>();
    }
    if (desired.type == "image") {
      if (wantsInputAreaWrapper(desired)) {
        return wrapClickable(std::make_unique<Image>(), callbackProp(desired, "onClick") != nullptr);
      }
      return std::make_unique<Image>();
    }
    if (desired.type == "pixel_stream") {
      return std::make_unique<PixelStreamView>();
    }
    if (desired.type == "color_picker") {
      return std::make_unique<ColorPicker>();
    }
    if (desired.type == "separator") {
      return std::make_unique<Separator>();
    }
    if (desired.type == "spacer") {
      return std::make_unique<Spacer>();
    }
    if (desired.type == "progress") {
      return std::make_unique<ProgressBar>();
    }
    if (desired.type == "button") {
      return std::make_unique<Button>();
    }
    if (desired.type == "graph") {
      if (wantsInputAreaWrapper(desired)) {
        return wrapClickable(std::make_unique<Graph>(), /*acceptClicks=*/false);
      }
      return std::make_unique<Graph>();
    }
    if (desired.type == "input") {
      return std::make_unique<Input>();
    }
    if (desired.type == "markdown") {
      return std::make_unique<MarkdownView>();
    }
    if (desired.type == "select") {
      return std::make_unique<Select>();
    }
    if (desired.type == "slider") {
      return std::make_unique<Slider>();
    }
    if (desired.type == "toggle") {
      return std::make_unique<Toggle>();
    }
    if (desired.type == "scroll") {
      auto scroll = std::make_unique<ScrollView>();
      scroll->setContentScale(m_scale);
      scroll->content()->setAlign(FlexAlign::Stretch); // match the ui.* column/row default
      return scroll;
    }
    kLog.warn("ui tree: unknown control type '{}', node skipped", desired.type);
    return nullptr;
  }

  bool UiTreeReconciler::syncChildren(
      Node& parent, std::vector<Slot>& slots, const std::vector<UiTreeNode>& desired, Renderer& renderer
  ) {
    // Fast path: the (type, key) sequence is unchanged — update props in place.
    bool sequenceMatches = slots.size() == desired.size();
    if (sequenceMatches) {
      for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].type != desired[i].type
            || slots[i].key != desired[i].key
            || clickableWrapMismatch(desired[i], slots[i].node)
            || (isDragDropType(desired[i].type) && !m_dragDropEnabled)) {
          sequenceMatches = false;
          break;
        }
      }
    }

    bool structureChanged = false;
    if (!sequenceMatches) {
      structureChanged = true;
      // Detach the current children, then re-add in desired order, reusing a
      // detached control whose (type, key) matches; everything else is created
      // fresh and unmatched leftovers are dropped.
      struct Detached {
        Slot slot;
        std::unique_ptr<Node> node;
        bool used = false;
      };
      std::vector<Detached> detached;
      detached.reserve(slots.size());
      for (auto& slot : slots) {
        if (slot.node != nullptr) {
          m_dragDropController->cancelIfParticipantIn(slot.node);
          auto owned = parent.removeChild(slot.node);
          detached.push_back(Detached{.slot = std::move(slot), .node = std::move(owned)});
        }
      }
      slots.clear();
      slots.reserve(desired.size());

      for (const auto& want : desired) {
        Detached* match = nullptr;
        for (auto& candidate : detached) {
          if (candidate.used
              || (isDragDropType(want.type) && !m_dragDropEnabled)
              || candidate.slot.type != want.type
              || candidate.slot.key != want.key
              || clickableWrapMismatch(want, candidate.node.get())) {
            continue;
          }
          match = &candidate;
          break;
        }
        if (match != nullptr) {
          match->used = true;
          Slot slot = std::move(match->slot);
          slot.node = parent.addChild(std::move(match->node));
          slots.push_back(std::move(slot));
          continue;
        }

        auto control = createControl(want);
        if (control == nullptr) {
          continue;
        }
        Slot slot;
        slot.type = want.type;
        slot.key = want.key;
        slot.node = parent.addChild(std::move(control));
        slots.push_back(std::move(slot));
      }

      for (const auto& leftover : detached) {
        if (leftover.used) {
          continue;
        }
        if (subtreeOwnsHover(leftover.slot)) {
          closeHover();
        }
        if (subtreeOwnsPointer(leftover.slot)) {
          closePointer();
        }
      }
    }

    // Apply props and recurse. With a sequence mismatch slots were rebuilt above
    // and may be shorter than `desired` (unknown types skipped).
    std::size_t slotIndex = 0;
    for (const auto& want : desired) {
      if (slotIndex >= slots.size()) {
        break;
      }
      Slot& slot = slots[slotIndex];
      if (slot.type != want.type || slot.key != want.key) {
        continue; // skipped unknown type
      }
      ++slotIndex;
      applyProps(slot, want, renderer);
      Node* container = childContainer(want.type, slot.node);
      if (slot.node != nullptr && !want.children.empty()) {
        if (container == nullptr) {
          kLog.warn("ui tree: '{}' cannot have children, {} dropped", want.type, want.children.size());
        } else {
          structureChanged |= syncChildren(*container, slot.children, want.children, renderer);
        }
      } else if (container != nullptr && want.children.empty() && !slot.children.empty()) {
        structureChanged |= syncChildren(*container, slot.children, {}, renderer);
      }
      if (want.type == "scroll" && slot.node != nullptr) {
        syncScrollReveal(slot, want);
      }
    }
    return structureChanged;
  }

  const UiTreeReconciler::Slot*
  UiTreeReconciler::findKeyedSlot(const std::vector<Slot>& slots, const std::string& key) {
    for (const auto& slot : slots) {
      if (slot.key == key) {
        return &slot;
      }
      if (const Slot* match = findKeyedSlot(slot.children, key)) {
        return match;
      }
    }
    return nullptr;
  }

  void UiTreeReconciler::syncScrollReveal(Slot& slot, const UiTreeNode& desired) {
    const std::string* revealKey = strProp(desired, "revealKey");
    if (revealKey == nullptr || revealKey->empty()) {
      slot.revealKey.clear();
      return;
    }
    // Looked up before the unchanged check, so a key whose child is missing is
    // forgotten and the reveal lands on the render that adds or restores it.
    const Slot* target = findKeyedSlot(slot.children, *revealKey);
    if (target == nullptr) {
      slot.revealKey.clear();
      return;
    }
    if (*revealKey == slot.revealKey) {
      return;
    }
    slot.revealKey = *revealKey;
    static_cast<ScrollView*>(slot.node)->requestScrollIntoView(target->node);
  }

  void UiTreeReconciler::syncWrapperCallbacks(Slot& slot, const UiTreeNode& desired, Node* node) {
    InputArea* inputArea = inputAreaFromSlot(node);
    if (const std::string* onClick = callbackProp(desired, "onClick"); onClick != nullptr) {
      if (*onClick != slot.callbackName) {
        slot.callbackName = *onClick;
        if (inputArea != nullptr) {
          wireWrapperActivation(inputArea, [this, name = slot.callbackName] {
            if (m_sink) {
              m_sink(ControlCallback{name});
            }
          });
        }
      }
    } else if (!slot.callbackName.empty()) {
      slot.callbackName.clear();
      if (inputArea != nullptr) {
        clearWrapperActivation(inputArea);
      }
    }
    if (const std::string* onHover = callbackProp(desired, "onHover"); onHover != nullptr) {
      if (*onHover != slot.hoverCallbackName) {
        releaseHover(node);
        slot.hoverCallbackName = *onHover;
        if (inputArea != nullptr) {
          inputArea->setOnEnter([this, name = slot.hoverCallbackName, key = desired.key,
                                 node](const InputArea::PointerData&) { openHover(name, key, node); });
          inputArea->setOnLeave([this, node] { releaseHover(node); });
        }
      }
    } else if (!slot.hoverCallbackName.empty()) {
      releaseHover(node);
      slot.hoverCallbackName.clear();
      if (inputArea != nullptr) {
        clearWrapperHover(inputArea);
      }
    }
    // Tooltip on wrappable containers, unconditional so a dropped tooltip
    // clears on the retained InputArea. Empty routes to clearTooltip().
    if (inputArea != nullptr) {
      const std::string* tooltip = strProp(desired, "tooltip");
      if (tooltip != nullptr && !tooltip->empty()) {
        inputArea->setTooltip(*tooltip);
      } else {
        inputArea->clearTooltip();
      }
    }
  }

} // namespace ui

#include "scripting/ui_tree_reconciler_internal.h"

namespace ui {
  using namespace ui_tree_detail;

  // Hover is state the plugin mirrors, so every "true" owes a "false". Exactly
  // one InputArea is hovered at a time, so tracking the one node that opened the
  // hover is enough to close it when that node is dropped, rewired, or reset
  // away. None of those reach the dispatcher's leave path, which only tracks
  // areas still in the scene.
  void UiTreeReconciler::openHover(const std::string& name, const std::string& key, const Node* owner) {
    if (m_hoveredOwner == owner) {
      return;
    }
    closeHover();
    m_hoveredCallback = name;
    m_hoveredKey = key;
    m_hoveredOwner = owner;
    if (m_sink) {
      m_sink(ControlCallback{name, "true", key});
    }
  }

  void UiTreeReconciler::closeHover() {
    if (m_hoveredOwner == nullptr) {
      return;
    }
    const std::string name = std::exchange(m_hoveredCallback, std::string{});
    const std::string key = std::exchange(m_hoveredKey, std::string{});
    m_hoveredOwner = nullptr;
    if (m_sink) {
      m_sink(ControlCallback{name, "false", key});
    }
  }

  // Closes the hover only if `owner` is the node currently reporting it, so
  // sibling nodes sharing one callback name do not close each other's hover.
  void UiTreeReconciler::releaseHover(const Node* owner) {
    if (owner != nullptr && owner == m_hoveredOwner) {
      closeHover();
    }
  }

  bool UiTreeReconciler::subtreeOwnsHover(const Slot& slot) const {
    if (m_hoveredOwner == nullptr) {
      return false;
    }
    if (slot.node == m_hoveredOwner) {
      return true;
    }
    return std::ranges::any_of(slot.children, [&](const Slot& child) { return subtreeOwnsHover(child); });
  }

  // Both edges of a graph's pointer session are wired together: the enter/motion
  // reporter and the leave share one coalescing stream key, so the script queue
  // holds a single pending event per graph and the newest one always wins. A
  // fresh key per rewire keeps a superseded stream from swallowing the leave the
  // old wiring owes.
  void UiTreeReconciler::syncGraphPointerCallbacks(Slot& slot, const UiTreeNode& desired, Node* node) {
    const std::string* move = callbackProp(desired, "onPointerMove");
    const std::string* leave = callbackProp(desired, "onPointerLeave");
    const std::string moveName = move != nullptr ? *move : std::string{};
    const std::string leaveName = leave != nullptr ? *leave : std::string{};
    if (moveName == slot.pointerMoveCallbackName && leaveName == slot.pointerLeaveCallbackName) {
      return;
    }
    releasePointer(node);
    slot.pointerMoveCallbackName = moveName;
    slot.pointerLeaveCallbackName = leaveName;

    InputArea* inputArea = inputAreaFromSlot(node);
    if (inputArea == nullptr) {
      return; // no wrapper: clickableWrapMismatch rebuilds instead of rewiring
    }
    if (moveName.empty() && leaveName.empty()) {
      slot.pointerStreamKey.clear();
      inputArea->setOnEnter(nullptr);
      inputArea->setOnMotion(nullptr);
      inputArea->setOnLeave(nullptr);
      return;
    }
    slot.pointerStreamKey = std::format("graph-pointer:{}", ++m_pointerStreamSeq);

    // Position is normalized to [0,1] over the graph's content area, which fills
    // the whole control (Graph::setSize puts the render node at 0,0, no inset).
    auto report = [this, name = moveName, key = slot.pointerStreamKey,
                   inputArea](const InputArea::PointerData& pointer) {
      if (!m_sink || name.empty() || inputArea->width() <= 0.0F || inputArea->height() <= 0.0F) {
        return;
      }
      const float normX = std::clamp(pointer.localX / inputArea->width(), 0.0F, 1.0F);
      const float normY = std::clamp(pointer.localY / inputArea->height(), 0.0F, 1.0F);
      m_sink(ControlCallback{name, std::format("{:.4F}", normX), std::format("{:.4F}", normY), true, key});
    };
    inputArea->setOnEnter([this, node, leaveName, key = slot.pointerStreamKey,
                           report](const InputArea::PointerData& pointer) {
      openPointer(node, leaveName, key);
      report(pointer);
    });
    inputArea->setOnMotion(report);
    inputArea->setOnLeave([this, node] { releasePointer(node); });
  }

  void UiTreeReconciler::openPointer(const Node* owner, std::string leaveCallback, std::string streamKey) {
    if (m_pointerOwner == owner) {
      return;
    }
    closePointer();
    m_pointerLeaveCallback = std::move(leaveCallback);
    m_pointerStreamKey = std::move(streamKey);
    m_pointerOwner = owner;
  }

  void UiTreeReconciler::closePointer() {
    if (m_pointerOwner == nullptr) {
      return;
    }
    const std::string name = std::exchange(m_pointerLeaveCallback, std::string{});
    const std::string key = std::exchange(m_pointerStreamKey, std::string{});
    m_pointerOwner = nullptr;
    if (m_sink && !name.empty()) {
      m_sink(ControlCallback{name, {}, {}, true, key});
    }
  }

  void UiTreeReconciler::releasePointer(const Node* owner) {
    if (owner != nullptr && owner == m_pointerOwner) {
      closePointer();
    }
  }

  void UiTreeReconciler::endPointer(const Node* owner) {
    if (owner != nullptr && owner == m_pointerOwner) {
      m_pointerOwner = nullptr;
      m_pointerLeaveCallback.clear();
      m_pointerStreamKey.clear();
    }
  }

  void UiTreeReconciler::applyColorPickerProps(Slot& slot, const UiTreeNode& desired, ColorPicker& picker) {
    constexpr std::size_t kMaxSwatches = 16;
    picker.setScale(m_scale);
    if (const double* width = numProp(desired, "width")) {
      picker.setPickerWidth(static_cast<float>(*width) * m_scale);
    } else {
      picker.setFillWidth();
    }

    const bool* alpha = boolProp(desired, "alpha");
    const bool withAlpha = alpha != nullptr && *alpha;
    picker.setAlphaEnabled(withAlpha);

    ColorPicker::Inputs inputs = ColorPicker::Inputs::Full;
    if (const std::string* mode = strProp(desired, "inputs")) {
      if (*mode == "hex") {
        inputs = ColorPicker::Inputs::Hex;
      } else if (*mode == "none") {
        inputs = ColorPicker::Inputs::None;
      } else if (*mode != "full") {
        kLog.warn("ui tree: colorPicker has unknown inputs '{}'", *mode);
      }
    }
    picker.setInputs(inputs);

    std::vector<Color> swatches;
    if (const std::vector<std::string>* entries = strArrayProp(desired, "swatches")) {
      for (const std::string& entry : *entries) {
        if (swatches.size() == kMaxSwatches) {
          kLog.warn("ui tree: colorPicker keeps the first {} swatches", kMaxSwatches);
          break;
        }
        Color color{};
        if (tryParseHexColor(entry, color)) {
          swatches.push_back(color);
        } else {
          kLog.warn("ui tree: colorPicker skips invalid swatch '{}'", entry);
        }
      }
    }
    picker.setSwatches(std::move(swatches));

    // Reads the picker's current alpha mode, so callbacks wired earlier stay correct when it changes.
    const auto format = [owner = &picker](const Color& color) {
      return owner->alphaEnabled() ? formatRgbaHex(color) : formatRgbHex(color);
    };
    // Like a slider's value: applied only when the prop itself changes, so re-applying an
    // unchanged tree never undoes the user's edits, and never mid-drag.
    if (const std::string* value = strProp(desired, "value"); value != nullptr && !picker.dragging()) {
      Color color{};
      if (*value == slot.colorValue) {
        // Unchanged since the last render.
      } else if (!tryParseHexColor(*value, color)) {
        kLog.warn("ui tree: colorPicker has invalid value '{}'", *value);
        slot.colorValue = *value;
      } else {
        slot.colorValue = *value;
        if (!withAlpha) {
          color.a = 1.0F;
        }
        // Echoing onChange back is a no-op, so it never rewrites the field being typed in.
        if (format(color) != format(picker.color())) {
          picker.setColor(color);
        }
      }
    }
    if (const bool* enabled = boolProp(desired, "enabled")) {
      picker.setEnabled(*enabled);
    }

    const std::string* onChange = callbackProp(desired, "onChange");
    const std::string changeName = onChange != nullptr ? *onChange : std::string{};
    if (changeName != slot.callbackName) {
      slot.callbackName = changeName;
      picker.setOnColorChanged(
          changeName.empty() ? std::function<void(const Color&)>{} : [this, changeName, format](const Color& color) {
            if (m_sink) {
              m_sink(ControlCallback{changeName, format(color), "", true});
            }
          }
      );
    }
    const std::string* onChangeEnd = callbackProp(desired, "onChangeEnd");
    const std::string endName = onChangeEnd != nullptr ? *onChangeEnd : std::string{};
    if (endName != slot.dragEndCallbackName) {
      slot.dragEndCallbackName = endName;
      picker.setOnColorCommitted(
          endName.empty() ? std::function<void(const Color&)>{} : [this, endName, format](const Color& color) {
            if (m_sink) {
              m_sink(ControlCallback{endName, format(color)});
            }
          }
      );
    }
  }

  void UiTreeReconciler::syncPixelStreamCallbacks(Slot& slot, const UiTreeNode& desired, PixelStreamView& view) {
    std::array<std::string, kStreamCallbackCount> names;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (const std::string* name = callbackProp(desired, kPixelStreamCallbackProps[i])) {
        names[i] = *name;
      }
    }
    if (names == slot.pixelStreamCallbackNames) {
      return;
    }
    // A drag that began under the old callbacks ends under them.
    view.cancelDrag();
    slot.pixelStreamCallbackNames = names;
    slot.pointerStreamKey = std::format("pixel-stream:{}", ++m_pointerStreamSeq);

    const auto wants = [&names](PixelStreamCallback callback) { return !names[callback].empty(); };
    std::uint32_t buttons = 0;
    if (wants(kStreamDown) || wants(kStreamUp) || wants(kStreamCancel)) {
      buttons |= InputArea::buttonMask(BTN_LEFT);
    }
    if (wants(kStreamRightClick)) {
      buttons |= InputArea::buttonMask(BTN_RIGHT);
    }
    if (wants(kStreamMiddleClick)) {
      buttons |= InputArea::buttonMask(BTN_MIDDLE);
    }
    if (buttons != 0 || wants(kStreamMove) || wants(kStreamScroll)) {
      view.setPointerHandler(
          [this, owner = static_cast<const Node*>(&view), stream = slot.pointerStreamKey,
           names](const PixelStreamView::PointerEvent& event) {
            if (!m_sink) {
              return;
            }
            const auto coordinate = [](float value) { return std::format("{:.4F}", value); };
            // One coalescing stream per segment: a newer move may replace a queued one, but
            // never one queued before the press, release, or scroll that separates them.
            const std::string segment = std::format("{}:{}", stream, event.segment);
            const auto call = [this, &names, &event, &coordinate](PixelStreamCallback callback) {
              if (!names[callback].empty()) {
                m_sink(ControlCallback{names[callback], coordinate(event.x), coordinate(event.y)});
              }
            };
            switch (event.kind) {
            case PixelStreamView::PointerKind::Move:
              if (!names[kStreamMove].empty()) {
                m_sink(ControlCallback{names[kStreamMove], coordinate(event.x), coordinate(event.y), true, segment});
              }
              break;
            case PixelStreamView::PointerKind::Down:
              // The drag is a pointer session: if the view is dropped, rewired, or reset away
              // mid-drag, closing the session still delivers onPointerCancel.
              if (!names[kStreamCancel].empty()) {
                openPointer(owner, names[kStreamCancel], segment);
              }
              call(kStreamDown);
              break;
            case PixelStreamView::PointerKind::Up:
              endPointer(owner);
              call(kStreamUp);
              break;
            case PixelStreamView::PointerKind::Cancel:
              releasePointer(owner);
              break;
            case PixelStreamView::PointerKind::RightClick:
              call(kStreamRightClick);
              break;
            case PixelStreamView::PointerKind::MiddleClick:
              call(kStreamMiddleClick);
              break;
            case PixelStreamView::PointerKind::Scroll:
              if (!names[kStreamScroll].empty()) {
                m_sink(ControlCallback{names[kStreamScroll], std::format("{}", event.dx), std::format("{}", event.dy)});
              }
              break;
            }
          },
          buttons, wants(kStreamScroll)
      );
    } else {
      view.setPointerHandler({}, 0, false);
    }

    if (wants(kStreamTextInput) || wants(kStreamKeyEvent)) {
      view.setKeyHandler([this, text = names[kStreamTextInput],
                          keyEvent = names[kStreamKeyEvent]](const InputArea::KeyData& key) {
        // Composition previews are not input yet; Super chords belong to the compositor.
        if (!m_sink || key.preedit || (key.modifiers & KeyMod::Super) != 0) {
          return;
        }
        if (isPlainPrintableKey(key.utf32, key.modifiers, key.preedit)) {
          if (key.pressed && !text.empty()) {
            if (std::string utf8 = StringUtils::utf32ToUtf8(key.utf32); !utf8.empty()) {
              m_sink(ControlCallback{text, std::move(utf8)});
            }
          }
          return;
        }
        if (keyEvent.empty()) {
          return;
        }
        if (std::string chord = keyChordToString(KeyChord{.sym = key.sym, .modifiers = key.modifiers});
            !chord.empty()) {
          m_sink(ControlCallback{keyEvent, std::move(chord), key.pressed ? "true" : "false"});
        }
      });
    } else {
      view.setKeyHandler({});
    }
  }

  bool UiTreeReconciler::subtreeOwnsPointer(const Slot& slot) const {
    if (m_pointerOwner == nullptr) {
      return false;
    }
    if (slot.node == m_pointerOwner) {
      return true;
    }
    return std::ranges::any_of(slot.children, [&](const Slot& child) { return subtreeOwnsPointer(child); });
  }

} // namespace ui

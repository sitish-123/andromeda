#pragma once

#include "scripting/ui_tree_reconciler.h"

#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/color_picker.h"
#include "ui/controls/drag_source.h"
#include "ui/controls/drop_zone.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/graph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/markdown_view.h"
#include "scripting/pixel_stream_view.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/controls/spacer.h"
#include "ui/controls/toggle.h"
#include "ui/drag_drop_controller.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/ui_tree.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <linux/input-event-codes.h>
#include <optional>
#include <unordered_set>
#include <utility>

namespace ui {
  namespace ui_tree_detail {

    constexpr Logger kLog("ui-tree");

    inline const double* numProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<double>(&it->second) : nullptr;
    }

    inline const std::string* strProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<std::string>(&it->second) : nullptr;
    }

    inline const bool* boolProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<bool>(&it->second) : nullptr;
    }

    inline const std::vector<double>* arrayProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<std::vector<double>>(&it->second) : nullptr;
    }

    inline const std::vector<std::string>* strArrayProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<std::vector<std::string>>(&it->second) : nullptr;
    }

    constexpr std::size_t kDragPayloadMaxBytes = 16 * 1024;
    constexpr std::size_t kDragIdentifierMaxBytes = 256;
    constexpr std::size_t kDropAcceptsMaxEntries = 16;

    // ui.pixelStream callback props, indexed by PixelStreamCallback.
    enum PixelStreamCallback : std::uint8_t {
      kStreamMove,
      kStreamDown,
      kStreamUp,
      kStreamCancel,
      kStreamRightClick,
      kStreamMiddleClick,
      kStreamScroll,
      kStreamTextInput,
      kStreamKeyEvent,
      kStreamCallbackCount,
    };
    constexpr std::array<const char*, kStreamCallbackCount> kPixelStreamCallbackProps = {
        "onPointerMove", "onPointerDown", "onPointerUp", "onPointerCancel", "onRightClick",
        "onMiddleClick", "onScroll",      "onTextInput", "onKeyEvent",
    };

    inline bool isDragDropType(std::string_view type) { return type == "drag_source" || type == "drop_zone"; }

    struct DragDropPropError {
      std::string prop;
      std::string reason;
    };

    inline std::optional<DragDropPropError>
    validateRequiredString(const UiTreeNode& node, const char* key, std::size_t maxBytes) {
      const auto it = node.props.find(key);
      if (it == node.props.end()) {
        return DragDropPropError{.prop = key, .reason = "missing"};
      }
      const auto* value = std::get_if<std::string>(&it->second);
      if (value == nullptr) {
        return DragDropPropError{.prop = key, .reason = "must be a string"};
      }
      if (value->empty()) {
        return DragDropPropError{.prop = key, .reason = "must not be empty"};
      }
      if (value->size() > maxBytes) {
        return DragDropPropError{.prop = key, .reason = "exceeds the size limit"};
      }
      return std::nullopt;
    }

    inline std::optional<DragDropPropError> validateDragDropProps(const UiTreeNode& node) {
      if (node.type == "drag_source") {
        if (auto error = validateRequiredString(node, "dragType", kDragIdentifierMaxBytes)) {
          return error;
        }
        if (auto error = validateRequiredString(node, "payload", kDragPayloadMaxBytes)) {
          return error;
        }
      } else if (node.type == "drop_zone") {
        if (auto error = validateRequiredString(node, "value", kDragIdentifierMaxBytes)) {
          return error;
        }
        if (auto error = validateRequiredString(node, "onDrop", kDragIdentifierMaxBytes)) {
          return error;
        }
        const auto acceptsIt = node.props.find("accepts");
        if (acceptsIt == node.props.end()) {
          return DragDropPropError{.prop = "accepts", .reason = "missing"};
        }
        const auto* accepts = std::get_if<std::vector<std::string>>(&acceptsIt->second);
        if (accepts == nullptr) {
          return DragDropPropError{.prop = "accepts", .reason = "must be a string array"};
        }
        if (accepts->size() > kDropAcceptsMaxEntries) {
          return DragDropPropError{.prop = "accepts", .reason = "has too many entries"};
        }
        for (const auto& accepted : *accepts) {
          if (accepted.empty()) {
            return DragDropPropError{.prop = "accepts", .reason = "contains an empty entry"};
          }
          if (accepted.size() > kDragIdentifierMaxBytes) {
            return DragDropPropError{.prop = "accepts", .reason = "contains an over-limit entry"};
          }
        }
      }
      if (node.props.contains("enabled") && boolProp(node, "enabled") == nullptr) {
        return DragDropPropError{.prop = "enabled", .reason = "must be a boolean"};
      }
      return std::nullopt;
    }

    // Optional-prop counterpart to validateDragDropProps: a present-but-mistyped
    // optional prop falls back to its default, but never silently.
    inline void warnMistypedOptionalProp(const UiTreeNode& node, const char* key, const char* expected) {
      if (node.props.contains(key)) {
        kLog.warn(
            "ui tree: '{}' key '{}': prop '{}' expects {}; default used", node.type,
            node.key.empty() ? "<unkeyed>" : node.key, key, expected
        );
      }
    }

    // Role token ("primary", "on_surface", …) with an optional alpha suffix
    // ("primary/0.6" → the role at 60% alpha, resolved live against the palette),
    // or hex ("#rrggbb[aa]"). Alpha is 0.0–1.0; hex carries its own alpha byte.
    inline std::optional<ColorSpec> parseColor(const UiTreeNode& node, const char* key) {
      const std::string* token = strProp(node, key);
      if (token == nullptr) {
        return std::nullopt;
      }
      std::string_view base = *token;
      float alpha = 1.0F;
      if (const auto slash = base.find('/'); slash != std::string_view::npos) {
        const std::string_view alphaText = base.substr(slash + 1);
        base = base.substr(0, slash);
        const auto* end = alphaText.data() + alphaText.size();
        if (const auto res = std::from_chars(alphaText.data(), end, alpha);
            res.ec != std::errc{} || res.ptr != end || alpha < 0.0F || alpha > 1.0F) {
          kLog.warn(
              "ui node '{}': invalid alpha '{}' in color '{}' for prop '{}' (expected 0.0-1.0)", node.type, alphaText,
              *token, key
          );
          return std::nullopt;
        }
      }
      if (auto role = colorRoleFromPluginToken(base); role.has_value()) {
        return colorSpecFromRole(*role, alpha);
      }
      if (base.size() != token->size()) {
        kLog.warn("ui node '{}': alpha suffix requires a color role, got '{}' for prop '{}'", node.type, base, key);
        return std::nullopt;
      }
      Color fixed;
      if (tryParseHexColor(*token, fixed)) {
        return fixedColorSpec(fixed);
      }
      kLog.warn("ui node '{}': unknown color '{}' for prop '{}'", node.type, *token, key);
      return std::nullopt;
    }

    inline std::optional<FontWeight> parseFontWeight(const UiTreeNode& node) {
      const std::string* token = strProp(node, "fontWeight");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "thin")
        return FontWeight::Thin;
      if (*token == "light")
        return FontWeight::Light;
      if (*token == "normal")
        return FontWeight::Normal;
      if (*token == "medium")
        return FontWeight::Medium;
      if (*token == "semibold")
        return FontWeight::SemiBold;
      if (*token == "bold")
        return FontWeight::Bold;
      if (*token == "heavy")
        return FontWeight::Heavy;
      kLog.warn("ui node '{}': unknown fontWeight '{}'", node.type, *token);
      return std::nullopt;
    }

    inline std::optional<FlexAlign> parseAlign(const UiTreeNode& node) {
      const std::string* token = strProp(node, "align");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "start")
        return FlexAlign::Start;
      if (*token == "center")
        return FlexAlign::Center;
      if (*token == "end")
        return FlexAlign::End;
      if (*token == "stretch")
        return FlexAlign::Stretch;
      kLog.warn("ui node '{}': unknown align '{}'", node.type, *token);
      return std::nullopt;
    }

    inline std::optional<FlexJustify> parseJustify(const UiTreeNode& node) {
      const std::string* token = strProp(node, "justify");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "start")
        return FlexJustify::Start;
      if (*token == "center")
        return FlexJustify::Center;
      if (*token == "end")
        return FlexJustify::End;
      if (*token == "space_between")
        return FlexJustify::SpaceBetween;
      kLog.warn("ui node '{}': unknown justify '{}'", node.type, *token);
      return std::nullopt;
    }

    inline std::optional<TextAlign> parseTextAlign(const UiTreeNode& node) {
      const std::string* token = strProp(node, "textAlign");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "start")
        return TextAlign::Start;
      if (*token == "center")
        return TextAlign::Center;
      if (*token == "end")
        return TextAlign::End;
      kLog.warn("ui node '{}': unknown textAlign '{}'", node.type, *token);
      return std::nullopt;
    }

    inline std::optional<ButtonVariant> parseButtonVariant(const UiTreeNode& node) {
      const std::string* token = strProp(node, "variant");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "default")
        return ButtonVariant::Default;
      if (*token == "primary")
        return ButtonVariant::Primary;
      if (*token == "secondary")
        return ButtonVariant::Secondary;
      if (*token == "destructive")
        return ButtonVariant::Destructive;
      if (*token == "outline")
        return ButtonVariant::Outline;
      if (*token == "ghost")
        return ButtonVariant::Ghost;
      kLog.warn("ui node '{}': unknown button variant '{}'", node.type, *token);
      return std::nullopt;
    }

    // Named control-height tier, in unscaled logical pixels. Callers scale it.
    // Distinct from ui.glyph's numeric `size`; use `height` for exact pixels.
    inline std::optional<float> parseControlSize(const UiTreeNode& node) {
      const std::string* token = strProp(node, "controlSize");
      if (token == nullptr) {
        if (node.props.contains("controlSize")) {
          kLog.warn(
              "ui node '{}': controlSize expects 'sm'/'md'/'lg', not a number - use 'height' for exact pixels",
              node.type
          );
        }
        return std::nullopt;
      }
      if (*token == "sm")
        return Style::controlHeight;
      if (*token == "md")
        return Style::controlHeight;
      if (*token == "lg")
        return Style::controlHeightLg;
      kLog.warn("ui node '{}': unknown controlSize '{}'", node.type, *token);
      return std::nullopt;
    }

    inline InputArea* inputAreaFromSlot(Node* node) { return dynamic_cast<InputArea*>(node); }

    // The Node that a control's children reconcile into. Flex containers host
    // their children directly; a wrapped Flex hosts them in its inner Flex; a
    // ScrollView hosts them in its inner content Flex. Any other control
    // returns nullptr — it cannot have children.
    inline Node* childContainer(const std::string& type, Node* node) {
      if (node == nullptr) {
        return nullptr;
      }
      if (type == "column" || type == "row") {
        if (auto* inputArea = inputAreaFromSlot(node)) {
          const auto& kids = inputArea->children();
          return kids.empty() ? nullptr : kids.front().get();
        }
        return node;
      }
      if (type == "drag_source" || type == "drop_zone") {
        return node;
      }
      if (type == "scroll") {
        return static_cast<ScrollView*>(node)->content();
      }
      return nullptr;
    }

    inline std::vector<float> toFloatSeries(const std::vector<double>& values) {
      std::vector<float> out;
      out.reserve(values.size());
      for (double v : values) {
        out.push_back(std::clamp(static_cast<float>(v), 0.0F, 1.0F));
      }
      return out;
    }

    template <typename Control> Control* controlFromSlot(Node* node) {
      if (node == nullptr) {
        return nullptr;
      }
      if (auto* control = dynamic_cast<Control*>(node)) {
        return control;
      }
      if (auto* inputArea = dynamic_cast<InputArea*>(node)) {
        const auto& kids = inputArea->children();
        if (!kids.empty()) {
          return dynamic_cast<Control*>(kids.front().get());
        }
      }
      return nullptr;
    }

    // A callback prop with an empty name counts as unset. Callback wiring is
    // change-detected against the slot's empty-string default, so an empty
    // name would never wire a handler — but it WOULD create a wrapper that
    // swallows clicks/hover meant for ancestors and sits in tab order dead.
    inline const std::string* callbackProp(const UiTreeNode& node, const char* key) {
      const std::string* name = strProp(node, key);
      return name != nullptr && !name->empty() ? name : nullptr;
    }

    // Only these types can carry an InputArea wrapper (see createControl).
    inline bool wrappableType(const std::string& type) {
      return type == "box" || type == "image" || type == "row" || type == "column" || type == "graph";
    }

    // The props that make a wrappable type need its InputArea: pointer tracking
    // for a graph, click/hover for the rest.
    inline bool wantsInputAreaWrapper(const UiTreeNode& want) {
      if (want.type == "graph") {
        return callbackProp(want, "onPointerMove") != nullptr || callbackProp(want, "onPointerLeave") != nullptr;
      }
      return callbackProp(want, "onClick") != nullptr
          || callbackProp(want, "onHover") != nullptr
          || strProp(want, "tooltip") != nullptr;
    }

    // If a reconcile flips that need, the existing node cannot be reused (its
    // structure no longer matches), so it is rebuilt like a type change.
    inline bool clickableWrapMismatch(const UiTreeNode& want, Node* node) {
      if (!wrappableType(want.type)) {
        return false;
      }
      return wantsInputAreaWrapper(want) != (inputAreaFromSlot(node) != nullptr);
    }

    // InputArea never self-sizes; box/image mirror explicit sizes, but a
    // content-sized flex needs its child measurement forwarded.
    class ClickWrap final : public InputArea {
    protected:
      LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
        if (!children().empty()) {
          const LayoutSize measured = children().front()->measure(renderer, constraints);
          setSize(measured.width, measured.height);
          return measured;
        }
        return InputArea::doMeasure(renderer, constraints);
      }

      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        setPosition(rect.x, rect.y);
        setSize(rect.width, rect.height);
        if (!children().empty()) {
          children().front()->arrange(
              renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = rect.width, .height = rect.height}
          );
        }
      }
    };

    inline std::unique_ptr<Node> wrapClickable(std::unique_ptr<Node> control, bool acceptClicks) {
      auto inputArea = std::make_unique<ClickWrap>();
      // A hover-only wrapper must not swallow clicks meant for ancestors.
      inputArea->setAcceptedButtons(acceptClicks ? InputArea::buttonMask(BTN_LEFT) : 0);
      if (acceptClicks) {
        inputArea->setFocusable(true);
      }
      inputArea->addChild(std::move(control));
      return inputArea;
    }

    inline void wireWrapperActivation(InputArea* inputArea, const std::function<void()>& sink) {
      // A wrapper created hover-only starts with an empty button mask.
      inputArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      inputArea->setOnClick([sink](const InputArea::PointerData&) { sink(); });
      inputArea->setFocusable(true);
      inputArea->setOnKeyDown([sink](const InputArea::KeyData& key) {
        if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
          sink();
        }
      });
    }

    // Wrapper input handlers deviate from the retain-absent-props default:
    // when onClick/onHover disappears while the other callback keeps the
    // wrapper alive, the stale handler must go with it — a retained one would
    // leave an invisible node that swallows clicks and sits in tab order as a
    // keyboard-activatable ghost.
    inline void clearWrapperActivation(InputArea* inputArea) {
      inputArea->setAcceptedButtons(0);
      inputArea->setFocusable(false);
      inputArea->setOnClick(nullptr);
      inputArea->setOnKeyDown(nullptr);
      // setOnClick(fn) auto-selects the pointer cursor; drop it with the click.
      inputArea->setCursorShape(0);
    }

    inline void clearWrapperHover(InputArea* inputArea) {
      inputArea->setOnEnter(nullptr);
      inputArea->setOnLeave(nullptr);
    }

    // Known prop keys per control type — an unknown prop is a loud skip, not a
    // silent no-op, so typos in plugin code surface immediately.
    inline const std::unordered_set<std::string>& knownProps(const std::string& type) {
      static const std::unordered_set<std::string> kCommon = {"width", "height", "flexGrow", "opacity", "visible"};
      static const std::unordered_set<std::string> kFlex = {"width",     "height",  "flexGrow",    "opacity",
                                                            "visible",   "gap",     "padding",     "paddingH",
                                                            "paddingV",  "align",   "justify",     "fill",
                                                            "radius",    "border",  "borderWidth", "minWidth",
                                                            "minHeight", "onClick", "onHover",     "tooltip"};
      static const std::unordered_set<std::string> kBox = {"width",   "height",  "flexGrow", "opacity",     "visible",
                                                           "fill",    "radius",  "border",   "borderWidth", "softness",
                                                           "onClick", "onHover", "tooltip"};
      static const std::unordered_set<std::string> kLabel = {"width",      "height",   "flexGrow", "opacity",
                                                             "visible",    "text",     "fontSize", "color",
                                                             "fontWeight", "maxWidth", "maxLines", "textAlign",
                                                             "fontFamily", "baseline"};
      static const std::unordered_set<std::string> kGlyph = {"width",   "height", "flexGrow", "opacity",
                                                             "visible", "name",   "size",     "color"};
      static const std::unordered_set<std::string> kImage = {"width",   "height",  "flexGrow", "opacity", "visible",
                                                             "path",    "radius",  "fit",      "border",  "borderWidth",
                                                             "onClick", "onHover", "tooltip"};
      static const std::unordered_set<std::string> kPixelStream = {
          "width",         "height",        "flexGrow",      "opacity",     "visible",         "path",
          "radius",        "onPointerMove", "onPointerDown", "onPointerUp", "onPointerCancel", "onRightClick",
          "onMiddleClick", "onScroll",      "onTextInput",   "onKeyEvent",
      };
      static const std::unordered_set<std::string> kColorPicker = {
          "width",  "flexGrow", "opacity", "visible",  "value",       "alpha",
          "inputs", "swatches", "enabled", "onChange", "onChangeEnd",
      };
      static const std::unordered_set<std::string> kSeparator = {"width",   "height",  "flexGrow",
                                                                 "opacity", "visible", "thickness",
                                                                 "color",   "spacing", "orientation"};
      static const std::unordered_set<std::string> kProgress = {"width",   "height",   "flexGrow",   "opacity",
                                                                "visible", "progress", "fill",       "track",
                                                                "radius",  "marker",   "markerColor"};
      static const std::unordered_set<std::string> kButton = {"width",       "height",  "flexGrow",     "opacity",
                                                              "visible",     "text",    "glyph",        "fontSize",
                                                              "glyphSize",   "variant", "contentAlign", "enabled",
                                                              "selected",    "onClick", "onRightClick", "tooltip",
                                                              "controlSize", "onHover"};
      static const std::unordered_set<std::string> kGraph = {
          "width", "height", "flexGrow",  "opacity",     "visible",       "values",        "values2",
          "color", "color2", "lineWidth", "fillOpacity", "onPointerMove", "onPointerLeave"
      };
      static const std::unordered_set<std::string> kInput = {"width",       "height",   "flexGrow",    "opacity",
                                                             "visible",     "value",    "placeholder", "fontSize",
                                                             "enabled",     "password", "multiline",   "focus",
                                                             "onChange",    "onSubmit", "controlSize", "submitOnEnter",
                                                             "frameVisible"};
      static const std::unordered_set<std::string> kMarkdown = {"width",   "height",  "flexGrow",
                                                                "opacity", "visible", "text"};
      static const std::unordered_set<std::string> kSelect = {"width",       "height",   "flexGrow",      "opacity",
                                                              "visible",     "options",  "selectedIndex", "enabled",
                                                              "placeholder", "onChange", "controlSize"};
      static const std::unordered_set<std::string> kSlider = {"width",      "height",  "flexGrow", "opacity",
                                                              "visible",    "min",     "max",      "step",
                                                              "value",      "enabled", "onChange", "onDragEnd",
                                                              "controlSize"};
      static const std::unordered_set<std::string> kToggle = {"width",   "height",  "flexGrow", "opacity",
                                                              "visible", "checked", "enabled",  "onChange"};
      static const std::unordered_set<std::string> kScroll = {
          "width",   "height",        "flexGrow", "opacity",           "visible",  "fill",     "radius",
          "border",  "borderWidth",   "gap",      "padding",           "paddingH", "paddingV", "align",
          "justify", "stickToBottom", "onScroll", "scrollToBottomRev", "revealKey"
      };
      static const std::unordered_set<std::string> kDragSource = {
          "width",           "height",         "flexGrow",   "opacity",  "visible", "gap",     "padding",
          "paddingH",        "paddingV",       "align",      "justify",  "fill",    "radius",  "border",
          "borderWidth",     "minWidth",       "minHeight",  "dragType", "payload", "enabled", "tooltip",
          "previewAncestor", "liftFromLayout", "grabCursor", "onClick"
      };
      static const std::unordered_set<std::string> kDropZone = {
          "width",     "height",  "flexGrow", "opacity", "visible",   "gap",     "padding",      "paddingH",
          "paddingV",  "align",   "justify",  "fill",    "radius",    "border",  "borderWidth",  "minWidth",
          "minHeight", "accepts", "value",    "onDrop",  "direction", "enabled", "expandOnDrag", "hitSlop"
      };

      if (type == "column" || type == "row") {
        return kFlex;
      }
      if (type == "box") {
        return kBox;
      }
      if (type == "label") {
        return kLabel;
      }
      if (type == "glyph") {
        return kGlyph;
      }
      if (type == "image") {
        return kImage;
      }
      if (type == "pixel_stream") {
        return kPixelStream;
      }
      if (type == "color_picker") {
        return kColorPicker;
      }
      if (type == "separator") {
        return kSeparator;
      }
      if (type == "progress") {
        return kProgress;
      }
      if (type == "button") {
        return kButton;
      }
      if (type == "graph") {
        return kGraph;
      }
      if (type == "input") {
        return kInput;
      }
      if (type == "markdown") {
        return kMarkdown;
      }
      if (type == "select") {
        return kSelect;
      }
      if (type == "slider") {
        return kSlider;
      }
      if (type == "toggle") {
        return kToggle;
      }
      if (type == "scroll") {
        return kScroll;
      }
      if (type == "drag_source") {
        return kDragSource;
      }
      if (type == "drop_zone") {
        return kDropZone;
      }
      return kCommon;
    }

  } // namespace ui_tree_detail

  struct UiTreeReconciler::Slot {
    std::string type;
    std::string key;
    Node* node = nullptr;
    std::string callbackName;             // last-wired button onClick / control onChange target
    std::string rightCallbackName;        // last-wired button onRightClick target
    std::string hoverCallbackName;        // last-wired onHover target (button/box/image/row/column)
    std::string submitCallbackName;       // last-wired input onSubmit target
    std::string dragEndCallbackName;      // last-wired slider onDragEnd target
    std::string pointerMoveCallbackName;  // last-wired graph onPointerMove target
    std::string pointerLeaveCallbackName; // last-wired graph onPointerLeave target
    std::string pointerStreamKey; // coalescing stream shared by this graph's or pixel stream's pointer callbacks
    // Last-wired ui.pixelStream callback targets, indexed by PixelStreamCallback.
    std::array<std::string, ui_tree_detail::kStreamCallbackCount> pixelStreamCallbackNames;
    std::string imagePath;              // last-applied resolved image source
    std::string colorValue;             // last-applied colorPicker value prop
    std::string lastText;               // markdown source cache - setMarkdown re-parses, only call on change
    std::string revealKey;              // last-applied scroll revealKey, empty while no child carries it
    float lastMarkdownScale = 0.0F;     // content scale baked into the parsed markdown
    float lastMarkdownFontScale = 0.0F; // text-only multiplier baked into the parsed markdown
    float imageTargetSize = 0.0F;
    // Controlled-with-change-detection: a value-driven control (toggle/slider/
    // select) only re-applies its declared value when it differs from the last
    // applied one, so an async re-render never fights an optimistic local change.
    std::optional<double> lastScalar;
    bool seeded = false; // an uncontrolled input has had its initial value applied
    std::vector<Slot> children;
  };

} // namespace ui

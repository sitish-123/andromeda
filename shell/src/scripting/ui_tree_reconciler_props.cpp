#include "scripting/ui_tree_reconciler_internal.h"

namespace ui {
  using namespace ui_tree_detail;

  void UiTreeReconciler::applyProps(Slot& slot, const UiTreeNode& desired, Renderer& renderer) {
    Node* node = slot.node;
    if (node == nullptr) {
      return;
    }

    const auto& known = knownProps(desired.type);
    for (const auto& [key, value] : desired.props) {
      (void)value;
      if (!known.contains(key)) {
        kLog.warn("ui tree: '{}' has no prop '{}', ignored", desired.type, key);
      }
    }

    const auto scaled = [this](double v) { return static_cast<float>(v) * m_scale; };
    const auto scaledFont = [this](double v) { return static_cast<float>(v) * m_scale * m_fontScale; };

    // Common node props.
    if (const bool* visible = boolProp(desired, "visible")) {
      if (!*visible) {
        m_dragDropController->cancelIfParticipantIn(node);
      }
      node->setVisible(*visible);
    }
    if (const double* opacity = numProp(desired, "opacity")) {
      const float clamped = std::clamp(static_cast<float>(*opacity), 0.0F, 1.0F);
      if (desired.type == "drag_source") {
        static_cast<DragSource*>(node)->setSourceOpacity(clamped);
      } else {
        node->setOpacity(clamped);
      }
    }
    if (const double* grow = numProp(desired, "flexGrow")) {
      node->setFlexGrow(static_cast<float>(*grow));
    }

    const double* width = numProp(desired, "width");
    const double* height = numProp(desired, "height");

    if (desired.type == "drag_source") {
      auto* source = static_cast<DragSource*>(node);
      if (auto error = validateDragDropProps(desired)) {
        kLog.warn(
            "ui tree: '{}' key '{}': prop '{}' {}; control disabled", desired.type,
            desired.key.empty() ? "<unkeyed>" : desired.key, error->prop, error->reason
        );
        source->setDragType({});
        source->setPayload({});
        source->setEnabled(false);
      } else {
        source->setDragType(*strProp(desired, "dragType"));
        source->setPayload(*strProp(desired, "payload"));
        source->setEnabled(boolProp(desired, "enabled") == nullptr || *boolProp(desired, "enabled"));
      }
      const std::string* tooltip = strProp(desired, "tooltip");
      if (tooltip == nullptr) {
        warnMistypedOptionalProp(desired, "tooltip", "a string");
      }
      source->setTooltip(tooltip != nullptr ? *tooltip : "");
      const double* previewAncestor = numProp(desired, "previewAncestor");
      if (previewAncestor == nullptr) {
        warnMistypedOptionalProp(desired, "previewAncestor", "an integer from 0 to 8");
        source->setPreviewAncestor(0);
      } else if (*previewAncestor < 0.0 || *previewAncestor > 8.0 || std::floor(*previewAncestor) != *previewAncestor) {
        kLog.warn("ui tree: 'drag_source' key '{}': previewAncestor expects an integer from 0 to 8", desired.key);
        source->setPreviewAncestor(0);
      } else {
        source->setPreviewAncestor(static_cast<std::size_t>(*previewAncestor));
      }
      const bool* liftFromLayout = boolProp(desired, "liftFromLayout");
      if (liftFromLayout == nullptr) {
        warnMistypedOptionalProp(desired, "liftFromLayout", "a boolean");
      }
      source->setLiftFromLayout(liftFromLayout != nullptr && *liftFromLayout);
      const bool* grabCursor = boolProp(desired, "grabCursor");
      if (grabCursor == nullptr) {
        warnMistypedOptionalProp(desired, "grabCursor", "a boolean");
      }
      source->setGrabCursor(grabCursor == nullptr || *grabCursor);
      if (const std::string* onClick = callbackProp(desired, "onClick"); onClick != nullptr) {
        if (*onClick != slot.callbackName) {
          slot.callbackName = *onClick;
          source->setOnClick([this, name = slot.callbackName] {
            if (m_sink) {
              m_sink(ControlCallback{name});
            }
          });
        }
      } else if (!slot.callbackName.empty()) {
        slot.callbackName.clear();
        source->setOnClick(nullptr);
      }
    } else if (desired.type == "drop_zone") {
      auto* zone = static_cast<DropZone*>(node);
      if (auto error = validateDragDropProps(desired)) {
        kLog.warn(
            "ui tree: '{}' key '{}': prop '{}' {}; control disabled", desired.type,
            desired.key.empty() ? "<unkeyed>" : desired.key, error->prop, error->reason
        );
        zone->setAccepts({});
        zone->setValue({});
        zone->setOnDrop({});
        zone->setEnabled(false);
      } else {
        zone->setAccepts(*strArrayProp(desired, "accepts"));
        zone->setValue(*strProp(desired, "value"));
        zone->setOnDrop(*strProp(desired, "onDrop"));
        zone->setEnabled(boolProp(desired, "enabled") == nullptr || *boolProp(desired, "enabled"));
      }
      const bool* expandOnDrag = boolProp(desired, "expandOnDrag");
      if (expandOnDrag == nullptr) {
        warnMistypedOptionalProp(desired, "expandOnDrag", "a boolean");
      }
      zone->setExpandOnDrag(expandOnDrag != nullptr && *expandOnDrag);
      const double* hitSlop = numProp(desired, "hitSlop");
      if (hitSlop == nullptr) {
        warnMistypedOptionalProp(desired, "hitSlop", "a number");
      }
      zone->setHitSlop(hitSlop != nullptr ? scaled(std::max(0.0, *hitSlop)) : 0.0F);
    }

    if (desired.type == "column"
        || desired.type == "row"
        || desired.type == "drag_source"
        || desired.type == "drop_zone") {
      auto* flex = controlFromSlot<Flex>(node);
      if (flex == nullptr) {
        return;
      }
      if (desired.type == "drop_zone") {
        auto* zone = static_cast<DropZone*>(node);
        const std::string* direction = strProp(desired, "direction");
        if (direction == nullptr) {
          warnMistypedOptionalProp(desired, "direction", R"("column" or "row")");
          zone->setDirection(FlexDirection::Vertical);
        } else if (*direction == "column") {
          zone->setDirection(FlexDirection::Vertical);
        } else if (*direction == "row") {
          zone->setDirection(FlexDirection::Horizontal);
        } else {
          kLog.warn("ui tree: 'drop_zone' key '{}': unknown direction '{}'", desired.key, *direction);
          zone->setDirection(FlexDirection::Vertical);
        }
      }
      if (const double* gap = numProp(desired, "gap")) {
        flex->setGap(scaled(*gap));
      }
      const double* padding = numProp(desired, "padding");
      const double* paddingV = numProp(desired, "paddingV");
      const double* paddingH = numProp(desired, "paddingH");
      if (padding != nullptr || paddingV != nullptr || paddingH != nullptr) {
        const float fallback = padding != nullptr ? scaled(*padding) : 0.0F;
        flex->setPadding(
            paddingV != nullptr ? scaled(*paddingV) : fallback, paddingH != nullptr ? scaled(*paddingH) : fallback
        );
      }
      if (auto align = parseAlign(desired)) {
        flex->setAlign(*align);
      }
      if (auto justify = parseJustify(desired)) {
        flex->setJustify(*justify);
      }
      if (auto fill = parseColor(desired, "fill")) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setZoneFill(*fill);
        } else {
          flex->setFill(*fill);
        }
      } else if (desired.type == "drop_zone") {
        static_cast<DropZone*>(node)->clearZoneFill();
      }
      if (const double* radius = numProp(desired, "radius")) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setZoneRadius(Style::scaledRadius(static_cast<float>(*radius), m_scale));
        } else {
          flex->setRadius(scaled(*radius));
        }
      } else if (desired.type == "drop_zone") {
        static_cast<DropZone*>(node)->clearZoneRadius(Style::scaledRadiusSm(m_scale));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setZoneBorder(
              *border, borderWidth != nullptr ? scaled(*borderWidth) : Style::borderWidth
          );
        } else {
          flex->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : Style::borderWidth);
        }
      } else if (desired.type == "drop_zone") {
        static_cast<DropZone*>(node)->clearZoneBorder();
      }
      if (const double* minWidth = numProp(desired, "minWidth")) {
        flex->setMinWidth(scaled(*minWidth));
      }
      if (const double* minHeight = numProp(desired, "minHeight")) {
        flex->setMinHeight(scaled(*minHeight));
      }
      if (width != nullptr) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setCollapsedWidth(scaled(*width));
        } else {
          flex->setMinWidth(scaled(*width));
          flex->setMaxWidth(scaled(*width));
        }
      }
      if (height != nullptr) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setCollapsedHeight(scaled(*height));
        } else {
          flex->setMinHeight(scaled(*height));
          flex->setMaxHeight(scaled(*height));
        }
      }
      syncWrapperCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "box") {
      auto* box = controlFromSlot<Box>(node);
      if (box == nullptr) {
        return;
      }
      if (auto fill = parseColor(desired, "fill")) {
        box->setFill(*fill);
      }
      if (const double* radius = numProp(desired, "radius")) {
        box->setRadius(scaled(*radius));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        box->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : 1.0F);
      }
      if (const double* softness = numProp(desired, "softness")) {
        box->setSoftness(static_cast<float>(*softness));
      }
      const float boxWidth = width != nullptr ? scaled(*width) : node->width();
      const float boxHeight = height != nullptr ? scaled(*height) : node->height();
      box->setSize(boxWidth, boxHeight);
      if (auto* inputArea = inputAreaFromSlot(node)) {
        inputArea->setSize(boxWidth, boxHeight);
      }
      syncWrapperCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "label") {
      auto* label = static_cast<Label*>(node);
      if (const std::string* fontFamily = strProp(desired, "fontFamily")) {
        label->setFontFamily(*fontFamily);
      } else {
        label->setFontFamily(m_defaultFontFamily);
      }
      LabelBaselineMode baselineMode = LabelBaselineMode::Text;
      if (const std::string* baseline = strProp(desired, "baseline")) {
        if (auto mode = labelBaselineModeFromToken(*baseline)) {
          baselineMode = *mode;
        } else {
          kLog.warn("ui node 'label': unknown baseline '{}'", *baseline);
        }
      }
      label->setBaselineMode(baselineMode);
      if (const std::string* text = strProp(desired, "text")) {
        label->setText(*text);
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        label->setFontSize(scaledFont(*fontSize));
      } else {
        label->setFontSize(Style::fontSizeBody * m_scale * m_fontScale);
      }
      if (auto color = parseColor(desired, "color")) {
        label->setColor(*color);
      } else {
        label->setColor(m_defaultLabelColor);
      }
      if (auto weight = parseFontWeight(desired)) {
        label->setFontWeight(*weight);
      } else {
        label->setFontWeight(m_defaultFontWeight);
      }
      if (const double* maxWidth = numProp(desired, "maxWidth")) {
        label->setMaxWidth(scaled(*maxWidth));
      }
      if (const double* maxLines = numProp(desired, "maxLines")) {
        label->setMaxLines(static_cast<int>(*maxLines));
      }
      if (auto align = parseTextAlign(desired)) {
        label->setTextAlign(*align);
      }
      return;
    }

    if (desired.type == "glyph") {
      auto* glyph = static_cast<Glyph*>(node);
      if (const std::string* name = strProp(desired, "name")) {
        glyph->setGlyph(*name);
      }
      if (const double* size = numProp(desired, "size")) {
        glyph->setGlyphSize(scaled(*size));
      } else {
        glyph->setGlyphSize(Style::baseGlyphSize * m_scale);
      }
      if (auto color = parseColor(desired, "color")) {
        glyph->setColor(*color);
      } else {
        glyph->setColor(m_defaultGlyphColor);
      }
      return;
    }

    if (desired.type == "color_picker") {
      applyColorPickerProps(slot, desired, *static_cast<ColorPicker*>(node));
      return;
    }

    if (desired.type == "pixel_stream") {
      auto* view = static_cast<PixelStreamView*>(node);
      view->setSize(
          width != nullptr ? scaled(*width) : view->width(), height != nullptr ? scaled(*height) : view->height()
      );
      const std::string* path = strProp(desired, "path");
      view->setPath(path != nullptr && !path->empty() ? (m_resolver ? m_resolver(*path) : *path) : std::string{});
      const double* radius = numProp(desired, "radius");
      view->setRadius(radius != nullptr ? std::max(0.0F, scaled(*radius)) : 0.0F);
      syncPixelStreamCallbacks(slot, desired, *view);
      return;
    }

    if (desired.type == "image") {
      auto* image = controlFromSlot<Image>(node);
      if (image == nullptr) {
        return;
      }
      if (const double* radius = numProp(desired, "radius")) {
        image->setRadius(scaled(*radius));
      }
      if (const std::string* fit = strProp(desired, "fit")) {
        if (*fit == "contain") {
          image->setFit(ImageFit::Contain);
        } else if (*fit == "cover") {
          image->setFit(ImageFit::Cover);
        } else if (*fit == "stretch") {
          image->setFit(ImageFit::Stretch);
        } else {
          kLog.warn("ui tree: image has unknown fit '{}'", *fit);
        }
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        image->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : Style::borderWidth);
      } else {
        image->setBorder(clearColorSpec(), 0.0F);
      }
      const float imageWidth = width != nullptr ? scaled(*width) : node->width();
      const float imageHeight = height != nullptr ? scaled(*height) : imageWidth;
      image->setSize(imageWidth, imageHeight);
      if (auto* inputArea = inputAreaFromSlot(node)) {
        inputArea->setSize(imageWidth, imageHeight);
      }
      if (const std::string* path = strProp(desired, "path")) {
        const std::string resolved = m_resolver ? m_resolver(*path) : *path;
        const float targetSize = std::max(1.0F, std::max(imageWidth, imageHeight) * 3.0F);
        if (resolved != slot.imagePath || targetSize > slot.imageTargetSize) {
          slot.imagePath = resolved;
          slot.imageTargetSize = targetSize;
          if (!image->setSourceFile(renderer, resolved, static_cast<int>(std::round(targetSize)), true)) {
            kLog.warn("ui tree: image failed to load '{}'", resolved);
          }
        }
      }
      syncWrapperCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "separator") {
      auto* separator = static_cast<Separator*>(node);
      if (const double* thickness = numProp(desired, "thickness")) {
        separator->setThickness(scaled(*thickness));
      }
      if (auto color = parseColor(desired, "color")) {
        separator->setColor(*color);
      }
      if (const double* spacing = numProp(desired, "spacing")) {
        separator->setSpacing(scaled(*spacing));
      }
      if (const std::string* orientation = strProp(desired, "orientation")) {
        if (*orientation == "horizontal") {
          separator->setOrientation(SeparatorOrientation::HorizontalRule);
        } else if (*orientation == "vertical") {
          separator->setOrientation(SeparatorOrientation::VerticalRule);
        } else if (*orientation == "auto") {
          separator->setOrientation(SeparatorOrientation::Auto);
        } else {
          kLog.warn("ui tree: separator has unknown orientation '{}'", *orientation);
        }
      }
      return;
    }

    if (desired.type == "progress") {
      auto* progress = static_cast<ProgressBar*>(node);
      if (const double* value = numProp(desired, "progress")) {
        progress->setProgress(std::clamp(static_cast<float>(*value), 0.0F, 1.0F));
      }
      if (auto fill = parseColor(desired, "fill")) {
        progress->setFill(*fill);
      }
      if (auto track = parseColor(desired, "track")) {
        progress->setTrack(*track);
      }
      if (const double* radius = numProp(desired, "radius")) {
        progress->setRadius(scaled(*radius));
      }
      // An absent marker clears the line, so a tree can drop it without restructuring.
      if (const double* marker = numProp(desired, "marker")) {
        progress->setMarkerWidth(Style::progressMarkerWidth * m_scale);
        progress->setMarker(static_cast<float>(*marker));
      } else {
        progress->clearMarker();
      }
      if (auto markerColor = parseColor(desired, "markerColor")) {
        progress->setMarkerColor(*markerColor);
      }
      progress->setSize(
          width != nullptr ? scaled(*width) : node->width(), height != nullptr ? scaled(*height) : node->height()
      );
      return;
    }

    if (desired.type == "button") {
      auto* button = static_cast<Button*>(node);
      const std::string* text = strProp(desired, "text");
      const std::string* glyph = strProp(desired, "glyph");
      if (glyph != nullptr) {
        button->setGlyph(*glyph);
      }
      if (text != nullptr) {
        button->setText(*text);
      } else if (glyph != nullptr) {
        button->setText("");
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        button->setFontSize(scaledFont(*fontSize));
      } else if (text != nullptr && !text->empty()) {
        // Guarded on non-empty text: setFontSize creates the label, which
        // would flip a glyph-only button to the taller text chrome tier.
        button->setFontSize(Style::fontSizeBody * m_scale * m_fontScale);
      }
      if (const double* glyphSize = numProp(desired, "glyphSize")) {
        button->setGlyphSize(scaled(*glyphSize));
      } else if (glyph != nullptr) {
        button->setGlyphSize(Style::fontSizeBody * m_scale);
      }
      if (auto variant = parseButtonVariant(desired)) {
        button->setVariant(*variant);
      }
      if (const std::string* contentAlign = strProp(desired, "contentAlign")) {
        if (*contentAlign == "start") {
          button->setContentAlign(ButtonContentAlign::Start);
        } else if (*contentAlign == "end") {
          button->setContentAlign(ButtonContentAlign::End);
        } else if (*contentAlign == "center") {
          button->setContentAlign(ButtonContentAlign::Center);
        } else {
          kLog.warn("ui tree: unknown button contentAlign '{}'", *contentAlign);
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        button->setEnabled(*enabled);
      }
      if (const bool* selected = boolProp(desired, "selected")) {
        button->setSelected(*selected);
      }
      // Unconditional: a tooltip dropped between renders must clear on the
      // retained Button. An empty text routes to InputArea::clearTooltip().
      const std::string* tooltip = strProp(desired, "tooltip");
      button->setTooltip(tooltip != nullptr ? *tooltip : "");
      // Like the wrapper controls, hover handlers clear on removal instead of
      // being retained: a stale one keeps firing and keeps the Button's
      // InputArea enabled for a callback the tree no longer declares.
      if (const std::string* onHover = callbackProp(desired, "onHover"); onHover != nullptr) {
        if (*onHover != slot.hoverCallbackName) {
          releaseHover(node);
          slot.hoverCallbackName = *onHover;
          button->setOnEnter([this, name = slot.hoverCallbackName, key = desired.key, node] {
            openHover(name, key, node);
          });
          button->setOnLeave([this, node] { releaseHover(node); });
        }
      } else if (!slot.hoverCallbackName.empty()) {
        releaseHover(node);
        slot.hoverCallbackName.clear();
        button->setOnEnter(nullptr);
        button->setOnLeave(nullptr);
      }
      if (const std::string* onClick = strProp(desired, "onClick");
          onClick != nullptr && *onClick != slot.callbackName) {
        slot.callbackName = *onClick;
        button->setOnClick([this, name = slot.callbackName] {
          if (m_sink) {
            m_sink(ControlCallback{name});
          }
        });
      }
      if (const std::string* onRightClick = strProp(desired, "onRightClick");
          onRightClick != nullptr && *onRightClick != slot.rightCallbackName) {
        slot.rightCallbackName = *onRightClick;
        button->setOnRightClickWithPointer(
            [this, name = slot.rightCallbackName](float x, float y, std::uint32_t serial, std::uint32_t time) {
              if (m_sink) {
                ControlCallback callback{name};
                callback.pointerContext =
                    ControlCallback::PointerContext{.x = x, .y = y, .serial = serial, .time = time};
                m_sink(callback);
              }
            }
        );
      }
      // Compact hosts (bar widgets): drop the settings-tier control chrome
      // (min-height, wide padding) and hug the content — a bar capsule is
      // barely one control height tall. Applied after setText/setGlyph, whose
      // label creation re-applies the text-tier chrome. Explicit width/height
      // below still override.
      if (m_compactControls) {
        button->setMinHeight(0.0F);
        button->setPadding(Style::spaceXs * m_scale);
      }
      if (auto size = parseControlSize(desired)) {
        button->setControlHeight(scaled(*size));
      }
      if (width != nullptr) {
        button->setMinWidth(scaled(*width));
        button->setMaxWidth(scaled(*width));
      }
      if (height != nullptr) {
        button->setMinHeight(scaled(*height));
        button->setMaxHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "graph") {
      auto* graph = controlFromSlot<Graph>(node);
      if (graph == nullptr) {
        return;
      }
      if (const std::vector<double>* values = arrayProp(desired, "values")) {
        graph->setValues(toFloatSeries(*values));
      }
      if (const std::vector<double>* values2 = arrayProp(desired, "values2")) {
        graph->setValues2(toFloatSeries(*values2));
      }
      if (auto color = parseColor(desired, "color")) {
        graph->setColor(*color);
      }
      if (auto color2 = parseColor(desired, "color2")) {
        graph->setColor2(*color2);
      }
      if (const double* lineWidth = numProp(desired, "lineWidth")) {
        graph->setLineWidth(scaled(*lineWidth));
      }
      if (const double* fillOpacity = numProp(desired, "fillOpacity")) {
        graph->setFillOpacity(std::clamp(static_cast<float>(*fillOpacity), 0.0F, 1.0F));
      }
      const float graphWidth = width != nullptr ? scaled(*width) : graph->width();
      const float graphHeight = height != nullptr ? scaled(*height) : graph->height();
      graph->setSize(graphWidth, graphHeight);
      InputArea* inputArea = inputAreaFromSlot(node);
      if (inputArea != nullptr) {
        inputArea->setSize(graphWidth, graphHeight);
      }
      syncGraphPointerCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "toggle") {
      auto* toggle = static_cast<Toggle*>(node);
      if (const bool* checked = boolProp(desired, "checked")) {
        const double v = *checked ? 1.0 : 0.0;
        if (!slot.lastScalar.has_value() || *slot.lastScalar != v) {
          slot.lastScalar = v;
          toggle->setChecked(*checked);
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        toggle->setEnabled(*enabled);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
        toggle->setOnChange([this, name = slot.callbackName](bool value) {
          if (m_sink) {
            m_sink(ControlCallback{name, value ? "true" : "false"});
          }
        });
      }
      return;
    }

    if (desired.type == "slider") {
      auto* slider = static_cast<Slider*>(node);
      const double* minV = numProp(desired, "min");
      const double* maxV = numProp(desired, "max");
      if (minV != nullptr || maxV != nullptr) {
        slider->setRange(minV != nullptr ? *minV : slider->minValue(), maxV != nullptr ? *maxV : slider->maxValue());
      }
      if (const double* step = numProp(desired, "step")) {
        slider->setStep(*step);
      }
      // Don't fight an in-progress drag, and only re-apply a changed value.
      if (const double* value = numProp(desired, "value");
          value && !slider->dragging() && (!slot.lastScalar.has_value() || *slot.lastScalar != *value)) {
        slot.lastScalar = *value;
        slider->setValue(*value);
      }

      if (const bool* enabled = boolProp(desired, "enabled")) {
        slider->setEnabled(*enabled);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
        slider->setOnValueChanged([this, name = slot.callbackName](double value) {
          if (m_sink) {
            m_sink(ControlCallback{name, std::format("{}", value), "", true});
          }
        });
      }
      if (const std::string* onDragEnd = strProp(desired, "onDragEnd");
          onDragEnd != nullptr && *onDragEnd != slot.dragEndCallbackName) {
        slot.dragEndCallbackName = *onDragEnd;
        slider->setOnDragEnd([this, name = slot.dragEndCallbackName] {
          if (m_sink) {
            m_sink(ControlCallback{name});
          }
        });
      }
      if (auto size = parseControlSize(desired)) {
        slider->setControlHeight(scaled(*size));
      }
      return;
    }

    if (desired.type == "select") {
      auto* select = static_cast<Select*>(node);
      const std::string* onChangeProp = strProp(desired, "onChange");
      if (const std::vector<std::string>* options = strArrayProp(desired, "options")) {
        select->setOptions(*options);
        slot.lastScalar.reset(); // re-apply the selection against the new option set
      }
      if (const double* index = numProp(desired, "selectedIndex");
          index && (!slot.lastScalar.has_value() || *slot.lastScalar != *index)) {
        slot.lastScalar = *index;
        select->setSelectedIndexSilently(static_cast<std::size_t>(std::max(0.0, *index)));
      }

      if (const bool* enabled = boolProp(desired, "enabled")) {
        select->setEnabled(*enabled);
      }
      if (const std::string* placeholder = strProp(desired, "placeholder")) {
        select->setPlaceholder(*placeholder);
      }
      select->setFontSize(Style::fontSizeBody * m_scale * m_fontScale);
      if (onChangeProp != nullptr && *onChangeProp != slot.callbackName) {
        slot.callbackName = *onChangeProp;
        select->setOnSelectionChanged([this, name = slot.callbackName](std::size_t idx, std::string_view text) {
          if (m_sink) {
            m_sink(ControlCallback{name, std::format("{}", idx), std::string(text)});
          }
        });
      }
      if (width != nullptr) {
        const float w = scaled(*width);
        select->setMinWidth(w);
        if (const double* grow = numProp(desired, "flexGrow"); grow == nullptr || *grow <= 0.0) {
          select->setMaxWidth(w);
        }
      }
      if (auto size = parseControlSize(desired)) {
        select->setControlHeight(scaled(*size));
      }
      if (height != nullptr) {
        select->setControlHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "input") {
      auto* input = static_cast<Input*>(node);
      // Uncontrolled: the host owns the text buffer/cursor. `value` seeds the
      // field once at creation; later reconciles never overwrite what the user
      // typed (the node key keeps the same Input instance alive across renders).
      if (!slot.seeded) {
        if (const std::string* value = strProp(desired, "value")) {
          input->setValue(*value);
        }
        // `focus` requests keyboard focus once, when the control is created
        // (a fresh key makes a new control) — never on later re-renders.
        if (const bool* focus = boolProp(desired, "focus"); focus != nullptr && *focus && m_focusSink) {
          m_focusSink(input->inputArea());
        }
        slot.seeded = true;
      }
      if (const std::string* placeholder = strProp(desired, "placeholder")) {
        input->setPlaceholder(*placeholder);
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        input->setFontSize(scaledFont(*fontSize));
      } else {
        input->setFontSize(Style::fontSizeBody * m_scale * m_fontScale);
      }
      // Multiline before password: they are mutually exclusive and multiline wins.
      if (const bool* multiline = boolProp(desired, "multiline")) {
        input->setMultiline(*multiline);
      }
      if (const bool* submitOnEnter = boolProp(desired, "submitOnEnter")) {
        input->setSubmitOnEnter(*submitOnEnter);
      }
      if (const bool* password = boolProp(desired, "password")) {
        input->setPasswordMode(*password);
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        input->setEnabled(*enabled);
      }
      if (const bool* frameVisible = boolProp(desired, "frameVisible")) {
        input->setFrameVisible(*frameVisible);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
        input->setOnChange([this, name = slot.callbackName](const std::string& value) {
          if (m_sink) {
            m_sink(ControlCallback{name, value, "", true});
          }
        });
      }
      if (const std::string* onSubmit = strProp(desired, "onSubmit");
          onSubmit != nullptr && *onSubmit != slot.submitCallbackName) {
        slot.submitCallbackName = *onSubmit;
        input->setOnSubmit([this, name = slot.submitCallbackName](const std::string& value) {
          if (m_sink) {
            m_sink(ControlCallback{name, value});
          }
        });
      }
      if (width != nullptr) {
        input->setMinLayoutWidth(scaled(*width));
      }
      if (auto size = parseControlSize(desired)) {
        input->setControlHeight(scaled(*size));
      }
      if (height != nullptr) {
        input->setControlHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "markdown") {
      auto* md = static_cast<MarkdownView*>(node);
      // The scale check sits outside the text-presence gate: a render that
      // omits text (retained-prop convention) must still pick up a rescale.
      const std::string* text = strProp(desired, "text");
      if ((text != nullptr && *text != slot.lastText)
          || m_scale != slot.lastMarkdownScale
          || m_fontScale != slot.lastMarkdownFontScale) {
        if (text != nullptr) {
          slot.lastText = *text;
        }
        slot.lastMarkdownScale = m_scale;
        slot.lastMarkdownFontScale = m_fontScale;
        md->setMarkdown(slot.lastText, m_scale, m_fontScale);
      }
      if (width != nullptr) {
        md->setMinWidth(scaled(*width));
        md->setMaxWidth(scaled(*width));
      }
      if (height != nullptr) {
        md->setMinHeight(scaled(*height));
        md->setMaxHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "scroll") {
      auto* scroll = static_cast<ScrollView*>(node);
      if (auto fill = parseColor(desired, "fill")) {
        scroll->setFill(*fill);
      }
      if (const double* radius = numProp(desired, "radius")) {
        scroll->setRadius(scaled(*radius));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        scroll->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : 1.0F);
      }
      if (const bool* stickToBottom = boolProp(desired, "stickToBottom")) {
        scroll->setStickToBottom(*stickToBottom);
      }
      if (const std::string* onScroll = strProp(desired, "onScroll");
          onScroll != nullptr && *onScroll != slot.callbackName) {
        slot.callbackName = *onScroll;
        scroll->setOnScrollChanged([this, name = slot.callbackName, scroll](float offset) {
          if (m_sink) {
            m_sink(ControlCallback{name, std::format("{}", offset), std::format("{}", scroll->maxScrollOffset())});
          }
        });
      }
      if (const double* scrollToBottomRev = numProp(desired, "scrollToBottomRev");
          scrollToBottomRev && (!slot.lastScalar.has_value() || *slot.lastScalar != *scrollToBottomRev)) {
        slot.lastScalar = *scrollToBottomRev;
        // Deferred: children reconcile after applyProps, so an immediate
        // setScrollOffset would clamp against the previous scroll extent.
        scroll->requestScrollToBottom();
      }

      // gap / padding / align / justify configure the inner content Flex that
      // hosts the children.
      Flex* content = scroll->content();
      if (const double* gap = numProp(desired, "gap")) {
        content->setGap(scaled(*gap));
      }
      if (auto align = parseAlign(desired)) {
        content->setAlign(*align);
      }
      if (auto justify = parseJustify(desired)) {
        content->setJustify(*justify);
      }
      const double* padding = numProp(desired, "padding");
      const double* paddingV = numProp(desired, "paddingV");
      const double* paddingH = numProp(desired, "paddingH");
      if (padding != nullptr || paddingV != nullptr || paddingH != nullptr) {
        const float fallback = padding != nullptr ? scaled(*padding) : 0.0F;
        content->setPadding(
            paddingV != nullptr ? scaled(*paddingV) : fallback, paddingH != nullptr ? scaled(*paddingH) : fallback
        );
      }
      if (width != nullptr) {
        scroll->setMinWidth(scaled(*width));
        scroll->setMaxWidth(scaled(*width));
      }
      if (height != nullptr) {
        scroll->setMinHeight(scaled(*height));
        scroll->setMaxHeight(scaled(*height));
      }
      return;
    }

    // spacer (and any future no-prop control): common props only.
    if (width != nullptr || height != nullptr) {
      node->setSize(
          width != nullptr ? scaled(*width) : node->width(), height != nullptr ? scaled(*height) : node->height()
      );
    }
  }

} // namespace ui

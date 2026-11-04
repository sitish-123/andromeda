#include "shell/settings/settings_modal_host.h"

#include "design_tokens.h"

#include "core/input/keybind_matcher.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/rect_node.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace settings {

  namespace {

    struct ShadowLayer {
      float y;
      float blur;
      float lightAlpha;
      float darkAlpha;
    };
    // Rulebook E4: key + contact shadow; y offset, blur and opacity per scheme.
    constexpr ShadowLayer kE4Key{24.0F, 64.0F, 0.22F, 0.50F};
    constexpr ShadowLayer kE4Contact{2.0F, 8.0F, 0.10F, 0.30F};

    void layoutShadow(RectNode& node, const ShadowLayer& layer, float x, float y, float w, float h, float radius,
                      float scale) {
      const float offsetY = layer.y * scale;
      Color color = colorForRole(ColorRole::Shadow);
      color.a *= isLightPalette() ? layer.lightAlpha : layer.darkAlpha;
      node.setStyle(RoundedRectStyle{
          .fill = color,
          .radius = Radii{radius, radius, radius, radius},
          // The shader fades over 2 x softness centred on the edge, so softness = blur / 2.
          .softness = layer.blur * 0.5F * scale,
          .outerShadow = true,
          .shadowCutoutOffsetY = offsetY,
      });
      node.setPosition(x, y + offsetY);
      node.setFrameSize(w, h);
    }

    class ModalEntryNode final : public Node {
    public:
      explicit ModalEntryNode(SettingsModalRequest request) : m_request(std::move(request)) {
        setParticipatesInLayout(false);

        auto backdrop = ui::inputArea({
            .out = &m_backdrop,
            .focusable = false,
            .tabStop = false,
        });
        auto dim = ui::box({
            .out = &m_dim,
            .configure = [](Box& box) {
              box.setFill(colorSpecFromRole(ColorRole::Scrim));
              box.clearBorder();
              box.setRadius(0.0F);
            },
        });
        backdrop->addChild(std::move(dim));
        addChild(std::move(backdrop));

        m_keyShadow = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
        m_contactShadow = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));

        auto panel = ui::box({
            .out = &m_panel,
            .configure = [this](Box& box) {
              box.setDialogStyle();
              box.setRadius(m_radius());
            },
        });
        panel->setZIndex(1);
        m_paletteConn = paletteChanged().connect([this] { layoutShadows(); });
        auto contentHost = ui::node({.out = &m_contentHost});
        if (m_request.build) {
          m_content = contentHost->addChild(m_request.build());
        }
        panel->addChild(std::move(contentHost));
        addChild(std::move(panel));
      }

      [[nodiscard]] std::function<void()> closeCallback() const { return m_request.requestClose; }
      [[nodiscard]] std::function<void()> closedCallback() const { return m_request.onClosed; }
      [[nodiscard]] std::function<bool(const KeyboardEvent&)> preDispatchCallback() const {
        return m_request.preDispatchKeyboard;
      }
      [[nodiscard]] std::function<InputArea*()> initialFocusCallback() const { return m_request.initialFocusArea; }

      void rebuildContent() {
        if (!m_request.build || m_contentHost == nullptr) {
          return;
        }
        if (m_content != nullptr) {
          m_contentHost->removeChild(m_content);
        }
        m_content = m_contentHost->addChild(m_request.build());
      }

      void updateContent(Renderer& renderer) {
        if (m_request.update) {
          m_request.update(renderer);
        }
      }

    protected:
      void doLayout(Renderer& renderer) override {
        const float fullWidth = std::max(1.0F, width());
        const float fullHeight = std::max(1.0F, height());
        m_backdrop->setPosition(0.0F, 0.0F);
        m_backdrop->setSize(fullWidth, fullHeight);
        m_dim->setPosition(0.0F, 0.0F);
        m_dim->setSize(fullWidth, fullHeight);

        const float padding = std::max(0.0F, m_request.contentPadding);
        const float margin = std::max(0.0F, m_request.windowMargin);
        const SettingsModalLayoutSpace space{
            .windowWidth = fullWidth,
            .maxContentWidth = std::max(1.0F, fullWidth - 2.0F * (margin + padding)),
            .maxContentHeight = std::max(1.0F, fullHeight - 2.0F * (margin + padding)),
        };
        LayoutSize contentSize{.width = space.maxContentWidth, .height = space.maxContentHeight};
        if (m_request.measure) {
          contentSize = m_request.measure(renderer, space);
        }
        contentSize.width = std::clamp(contentSize.width, 1.0F, space.maxContentWidth);
        contentSize.height = std::clamp(contentSize.height, 1.0F, space.maxContentHeight);

        const float panelWidth = contentSize.width + 2.0F * padding;
        const float panelHeight = contentSize.height + 2.0F * padding;
        m_panel->setPosition((fullWidth - panelWidth) * 0.5F, (fullHeight - panelHeight) * 0.5F);
        m_panel->setSize(panelWidth, panelHeight);
        layoutShadows();
        m_contentHost->setPosition(padding, padding);
        m_contentHost->setSize(contentSize.width, contentSize.height);
        if (m_content != nullptr) {
          if (m_request.arrange) {
            m_request.arrange(renderer, contentSize.width, contentSize.height);
          } else {
            m_content->arrange(
                renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = contentSize.width, .height = contentSize.height}
            );
          }
        }
      }

      LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
        return constraints.constrain(LayoutSize{.width = width(), .height = height()});
      }

      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        setPosition(rect.x, rect.y);
        setSize(rect.width, rect.height);
        doLayout(renderer);
      }

    private:
      [[nodiscard]] float m_scale() const noexcept { return m_request.scale; }
      void layoutShadows() {
        const float x = m_panel->x();
        const float y = m_panel->y();
        const float w = m_panel->width();
        const float h = m_panel->height();
        layoutShadow(*m_keyShadow, kE4Key, x, y, w, h, m_radius(), m_scale());
        layoutShadow(*m_contactShadow, kE4Contact, x, y, w, h, m_radius(), m_scale());
      }

      [[nodiscard]] float m_radius() const noexcept {
        return Style::scaledRadius(Tokens::componentDialogRadius, m_scale());
      }

      SettingsModalRequest m_request;
      InputArea* m_backdrop = nullptr;
      Box* m_dim = nullptr;
      Box* m_panel = nullptr;
      RectNode* m_keyShadow = nullptr;
      RectNode* m_contactShadow = nullptr;
      Signal<>::ScopedConnection m_paletteConn;
      Node* m_contentHost = nullptr;
      Node* m_content = nullptr;
    };

    class ModalStackNode final : public Node {
    public:
      ModalStackNode() {
        setParticipatesInLayout(false);
        setZIndex(1000);
      }

    protected:
      void doLayout(Renderer& renderer) override {
        for (const auto& child : children()) {
          child->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = width(), .height = height()});
        }
      }

      LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
        return constraints.constrain(LayoutSize{.width = width(), .height = height()});
      }

      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        setPosition(rect.x, rect.y);
        setSize(rect.width, rect.height);
        doLayout(renderer);
      }
    };

  } // namespace

  struct SettingsModalHost::Impl {
    struct Entry {
      ModalId id = 0;
      ModalEntryNode* node = nullptr;
      InputDispatcher::TabFocusSnapshot previousFocus;
    };

    InputDispatcher* input = nullptr;
    std::function<void()> requestLayout;
    std::function<void()> requestUpdateOnly;
    Node* sceneRoot = nullptr;
    Node* settingsContent = nullptr;
    ModalStackNode* stackRoot = nullptr;
    std::unique_ptr<Node> detachedStack;
    std::vector<Entry> entries;
    ModalId nextId = 1;

    void ensureStack() {
      if (stackRoot != nullptr) {
        return;
      }
      auto stack = std::make_unique<ModalStackNode>();
      stackRoot = stack.get();
      detachedStack = std::move(stack);
    }

    void refreshFocusExclusion() {
      if (stackRoot != nullptr) {
        stackRoot->setVisible(!entries.empty());
        stackRoot->setHitTestVisible(!entries.empty());
      }
      if (settingsContent != nullptr) {
        settingsContent->setExcludeSubtreeFromTabOrder(!entries.empty());
      }
      for (std::size_t index = 0; index < entries.size(); ++index) {
        entries[index].node->setExcludeSubtreeFromTabOrder(index + 1 < entries.size());
      }
    }
  };

  SettingsModalHost::SettingsModalHost() : m_impl(std::make_unique<Impl>()) {}

  SettingsModalHost::~SettingsModalHost() = default;

  void SettingsModalHost::initialize(
      InputDispatcher& inputDispatcher, std::function<void()> requestLayout, std::function<void()> requestUpdateOnly
  ) {
    m_impl->input = &inputDispatcher;
    m_impl->requestLayout = std::move(requestLayout);
    m_impl->requestUpdateOnly = std::move(requestUpdateOnly);
    m_impl->ensureStack();
  }

  void
  SettingsModalHost::attach(Node& sceneRoot, Node* settingsContent, Renderer& renderer, float width, float height) {
    m_impl->ensureStack();
    if (m_impl->stackRoot->parent() != nullptr) {
      detach();
    }
    m_impl->sceneRoot = &sceneRoot;
    m_impl->settingsContent = settingsContent;
    if (m_impl->detachedStack != nullptr) {
      sceneRoot.addChild(std::move(m_impl->detachedStack));
    }
    m_impl->stackRoot->setPopupContext(sceneRoot.popupContext());
    m_impl->stackRoot->setSize(width, height);
    m_impl->refreshFocusExclusion();
    if (!m_impl->entries.empty()) {
      m_impl->stackRoot->layout(renderer);
      if (m_impl->input != nullptr) {
        auto initialFocus = m_impl->entries.back().node->initialFocusCallback();
        m_impl->input->setFocus(initialFocus ? initialFocus() : nullptr);
      }
    }
  }

  void SettingsModalHost::detach() {
    if (m_impl->sceneRoot != nullptr && m_impl->stackRoot != nullptr && m_impl->stackRoot->parent() != nullptr) {
      m_impl->detachedStack = m_impl->sceneRoot->removeChild(m_impl->stackRoot);
    }
    m_impl->sceneRoot = nullptr;
    m_impl->settingsContent = nullptr;
  }

  void SettingsModalHost::resize(float width, float height) {
    if (m_impl->stackRoot != nullptr) {
      m_impl->stackRoot->setSize(width, height);
    }
  }

  std::optional<SettingsModalHost::ModalId> SettingsModalHost::push(SettingsModalRequest request) {
    if (m_impl->stackRoot == nullptr || m_impl->stackRoot->parent() == nullptr || !request.build) {
      return std::nullopt;
    }
    if (m_impl->input != nullptr) {
      m_impl->input->cancelPointerCapture();
    }
    auto entry = std::make_unique<ModalEntryNode>(std::move(request));
    auto* raw = entry.get();
    InputDispatcher::TabFocusSnapshot previousFocus;
    if (m_impl->input != nullptr) {
      previousFocus = m_impl->input->captureTabFocus();
    }
    const ModalId id = m_impl->nextId++;
    m_impl->stackRoot->addChild(std::move(entry));
    m_impl->entries.push_back(Impl::Entry{.id = id, .node = raw, .previousFocus = std::move(previousFocus)});
    m_impl->refreshFocusExclusion();
    if (m_impl->input != nullptr) {
      auto initialFocus = raw->initialFocusCallback();
      m_impl->input->setFocus(initialFocus ? initialFocus() : nullptr);
      m_impl->input->syncPointerHover();
    }
    if (m_impl->requestLayout) {
      m_impl->requestLayout();
    }
    return id;
  }

  void SettingsModalHost::rebuildTop() {
    if (m_impl->entries.empty()) {
      return;
    }
    if (m_impl->input != nullptr) {
      m_impl->input->stashTabFocus();
      m_impl->input->setFocus(nullptr);
    }
    m_impl->entries.back().node->rebuildContent();
    if (m_impl->input != nullptr) {
      m_impl->input->restoreStashedTabFocus();
    }
    requestLayout();
  }

  void SettingsModalHost::pop() {
    if (m_impl->entries.empty() || m_impl->stackRoot == nullptr) {
      return;
    }
    // The hovered tooltip is a popup whose anchor belongs to this modal subtree.
    // Destroy it before removing that anchor instead of leaving a fade-out to run.
    TooltipManager::instance().forceDestroy();
    if (m_impl->input != nullptr) {
      m_impl->input->cancelPointerCapture();
      m_impl->input->setFocus(nullptr);
    }
    const Impl::Entry entry = m_impl->entries.back();
    const auto onClosed = entry.node->closedCallback();
    m_impl->entries.pop_back();
    auto removed = m_impl->stackRoot->removeChild(entry.node);
    m_impl->refreshFocusExclusion();
    if (onClosed) {
      onClosed();
    }
    removed.reset();
    if (m_impl->input != nullptr) {
      m_impl->input->restoreTabFocus(entry.previousFocus);
      m_impl->input->syncPointerHover();
    }
    if (m_impl->requestLayout) {
      m_impl->requestLayout();
    }
  }

  bool SettingsModalHost::pop(ModalId id) {
    if (!isTop(id)) {
      return false;
    }
    pop();
    return true;
  }

  void SettingsModalHost::closeAll() {
    while (!m_impl->entries.empty()) {
      const std::size_t previousDepth = m_impl->entries.size();
      requestCloseTop();
      if (m_impl->entries.size() == previousDepth) {
        pop();
      }
    }
  }

  void SettingsModalHost::requestCloseTop() {
    if (m_impl->entries.empty()) {
      return;
    }
    const auto close = m_impl->entries.back().node->closeCallback();
    if (close) {
      close();
    } else {
      pop();
    }
  }

  bool SettingsModalHost::isOpen() const noexcept { return !m_impl->entries.empty(); }

  bool SettingsModalHost::isTop(ModalId id) const noexcept {
    return !m_impl->entries.empty() && m_impl->entries.back().id == id;
  }

  Node* SettingsModalHost::topRoot() const noexcept {
    return m_impl->entries.empty() ? nullptr : m_impl->entries.back().node;
  }

  InputArea* SettingsModalHost::focusedArea() const noexcept {
    return m_impl->input != nullptr ? m_impl->input->focusedArea() : nullptr;
  }

  void SettingsModalHost::focusArea(InputArea* area) {
    if (m_impl->input != nullptr) {
      m_impl->input->setFocus(area);
    }
  }

  void SettingsModalHost::requestLayout() {
    if (m_impl->requestLayout) {
      m_impl->requestLayout();
    }
  }

  void SettingsModalHost::requestUpdateOnly() {
    if (m_impl->requestUpdateOnly) {
      m_impl->requestUpdateOnly();
    }
  }

  bool SettingsModalHost::onKeyboardEvent(const KeyboardEvent& event) {
    if (m_impl->entries.empty() || m_impl->input == nullptr) {
      return false;
    }
    const InputArea* const focused = m_impl->input->focusedArea();
    if (focused != nullptr && focused->capturesAllKeys()) {
      m_impl->input->keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
      if (m_impl->requestLayout) {
        m_impl->requestLayout();
      }
      return true;
    }
    if (event.pressed && !event.preedit && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      requestCloseTop();
      return true;
    }
    const auto preDispatch = m_impl->entries.back().node->preDispatchCallback();
    if (preDispatch && preDispatch(event)) {
      if (m_impl->requestLayout) {
        m_impl->requestLayout();
      }
      return true;
    }
    if (event.pressed && !event.preedit) {
      if (KeybindMatcher::matches(KeybindAction::TabPrevious, event.sym, event.modifiers)) {
        return m_impl->input->cycleTabFocusInSubtree(topRoot(), true);
      }
      if (KeybindMatcher::matches(KeybindAction::TabNext, event.sym, event.modifiers)) {
        return m_impl->input->cycleTabFocusInSubtree(topRoot(), false);
      }
    }
    m_impl->input->keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    if (m_impl->requestLayout) {
      m_impl->requestLayout();
    }
    return true;
  }

  void SettingsModalHost::update(Renderer& renderer) {
    if (!m_impl->entries.empty()) {
      m_impl->entries.back().node->updateContent(renderer);
    }
  }

} // namespace settings

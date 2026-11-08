#pragma once

#include "ui/dialogs/dialog_popup_host.h"
#include "ui/ui_settings.h"

#include <functional>
#include <memory>

class Node;
struct xdg_surface;

// A popover under a header control (FILES-UI-SPEC §14 jobs popover): an xdg_popup in the menu material with any
// content. Outside clicks and Escape close it.
class AppPopover final : public DialogPopupHost {
public:
  struct Request {
    xdg_surface* parentXdg = nullptr;
    wl_surface* parentWl = nullptr;
    std::uint32_t serial = 0;
    // The control the popover hangs under, in window surface coordinates.
    std::int32_t anchorX = 0;
    std::int32_t anchorY = 0;
    std::int32_t anchorWidth = 1;
    std::int32_t anchorHeight = 1;
    float width = 0.0F; // physical-size content box, already scaled
    float height = 0.0F;
    float scale = 1.0F;
    std::function<std::unique_ptr<Node>(float scale)> content;
  };

  AppPopover(WaylandConnection& wayland, RenderContext& renderContext);
  ~AppPopover() override;

  void open(Request request);
  void close() { destroyPopup(); }
  // Builds the content again (the jobs list changed); keeps the popover open.
  void refresh();
  void setOnClosed(std::function<void()> closed) { m_closed = std::move(closed); }

  using DialogPopupHost::isOpen;
  using DialogPopupHost::onKeyboardEvent;
  using DialogPopupHost::onPointerEvent;
  using DialogPopupHost::wlSurface;

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override {}
  [[nodiscard]] InputArea* initialFocusArea() override { return nullptr; }
  [[nodiscard]] float computePadding(float /*scale*/) const override { return 0.0F; }
  void onSheetClose() override;

private:
  class Settings final : public UiSettings {
  public:
    float scale = 1.0F;
    [[nodiscard]] float uiScale() const override { return scale; }
    [[nodiscard]] ShadowStyle shadowStyle() const override { return {}; }
  };

  Settings m_settings;
  Request m_request;
  Node* m_parent = nullptr;
  Node* m_body = nullptr;
  std::function<void()> m_closed;
};

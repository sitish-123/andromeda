#include "input/cursor.h"

#include "input/event_time.h"
#include "input/cursor_internal.h"
#include "input/gestures.h"
#include "input/input_capture.h"
#include "input/seat.h"
#include "input/toplevel_drag.h"
#include "input/xcursor_matcher.h"
#include "lock/session_lock.h"
#include "overview/overview.h"
#include "scene/quit_confirm.h"
#include "scene/ssd_frame.h"
#include "server/dsk_surface.h"
#include "view/view.h"
// clang-format off
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
// clang-format on
#include "workspace/scratchpad.h"
#include "xwayland/xwayland.h"

#include <string_view>

namespace andromeda {

  using namespace cursor_detail;

  namespace {
    // Mod+right-click pairing and the motion threshold that promotes its
    // deferred resize grab share one boundary: inside it a press may still
    // pair up, so no grab may run and drop the layout's maximize bookkeeping.
    constexpr uint32_t kDoubleClickMsec = 300;
    constexpr double kDoubleClickSlop = 8.0;

    // `[input.touchpad] scroll_factor` (overridden per direction by `horizontal`/`vertical`) scales a touchpad's smooth
    // scroll delta, never the discrete value120 notches. Read per event so a reload applies on the next axis;
    // non-touchpads and unset values stay at 1.0.
    double touchpadScrollFactor(wlr_pointer* pointer, bool vertical) {
      if (pointer == nullptr || !wlr_input_device_is_libinput(&pointer->base)) {
        return 1.0;
      }
      libinput_device* device = wlr_libinput_get_device_handle(&pointer->base);
      if (device == nullptr || libinput_device_config_tap_get_finger_count(device) == 0) {
        return 1.0;
      }
      const std::optional<Config::Input::Touchpad::ScrollFactor>& factor = config().input.touchpad.scrollFactor;
      return factor ? (vertical ? factor->vertical : factor->horizontal).value_or(1.0) : 1.0;
    }

  } // namespace

  Cursor::Cursor(Server& server) : m_server(&server) {
    m_cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(m_cursor, m_server->outputLayout());
    const Config::Input::Cursor& configured = config().input.cursor;
    m_xcursorTheme = configured.theme;
    m_xcursorSize = configured.size;
    m_xcursorManager =
        wlr_xcursor_manager_create(m_xcursorTheme.empty() ? nullptr : m_xcursorTheme.c_str(), m_xcursorSize);

    m_motion.connect(&m_cursor->events.motion, this, onMotion);
    m_motionAbsolute.connect(&m_cursor->events.motion_absolute, this, onMotionAbsolute);
    m_button.connect(&m_cursor->events.button, this, onButton);
    m_axis.connect(&m_cursor->events.axis, this, onAxis);
    m_frame.connect(&m_cursor->events.frame, this, onFrame);

    m_touchDown.connect(&m_cursor->events.touch_down, this, onTouchDown);
    m_touchUp.connect(&m_cursor->events.touch_up, this, onTouchUp);
    m_touchMotion.connect(&m_cursor->events.touch_motion, this, onTouchMotion);
    m_touchCancel.connect(&m_cursor->events.touch_cancel, this, onTouchCancel);
    m_touchFrame.connect(&m_cursor->events.touch_frame, this, onTouchFrame);

    m_tabletToolAxis.connect(&m_cursor->events.tablet_tool_axis, this, onTabletToolAxis);
    m_tabletToolProximity.connect(&m_cursor->events.tablet_tool_proximity, this, onTabletToolProximity);
    m_tabletToolTip.connect(&m_cursor->events.tablet_tool_tip, this, onTabletToolTip);
    m_tabletToolButton.connect(&m_cursor->events.tablet_tool_button, this, onTabletToolButton);

    updateHideTimer();
  }

  Cursor::~Cursor() {
    if (m_snapDwellTimer != nullptr) {
      wl_event_source_remove(m_snapDwellTimer);
    }
    if (m_dataDragEdgeScrollTimer != nullptr) {
      wl_event_source_remove(m_dataDragEdgeScrollTimer);
    }
    if (m_hotCornerTimer != nullptr) {
      wl_event_source_remove(m_hotCornerTimer);
    }
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
    }
    // These listen on m_cursor, which wlr_cursor_destroy frees before the members die.
    for (Listener* listener : {&m_motion, &m_motionAbsolute, &m_button, &m_axis, &m_frame, &m_touchDown, &m_touchUp,
                               &m_touchMotion, &m_touchCancel, &m_touchFrame, &m_tabletToolAxis, &m_tabletToolProximity,
                               &m_tabletToolTip, &m_tabletToolButton}) {
      listener->disconnect();
    }
    wlr_cursor_destroy(m_cursor);
    wlr_xcursor_manager_destroy(m_xcursorManager);
  }

  void Cursor::attachInputDevice(wlr_input_device* device) { wlr_cursor_attach_input_device(m_cursor, device); }
  void Cursor::mapInputToOutput(wlr_input_device* device, wlr_output* output) {
    wlr_cursor_map_input_to_output(m_cursor, device, output);
  }
  void Cursor::resetWheelAccumulation() { m_wheelAccum[0] = m_wheelAccum[1] = 0; }

  void Cursor::handleDataDragStarted() { updateDataDragEdgeScroll(); }
  void Cursor::handleDataDragEnded() { cancelDataDragEdgeScroll(); }

  void Cursor::applyConfig() {
    const Config::Input::Cursor& configured = config().input.cursor;
    updateHideTimer();
    cancelHotCorner();
    updateHotCorner();
    if (configured.theme == m_xcursorTheme && configured.size == m_xcursorSize) {
      return;
    }

    wlr_xcursor_manager* manager =
        wlr_xcursor_manager_create(configured.theme.empty() ? nullptr : configured.theme.c_str(), configured.size);
    if (manager == nullptr) {
      return;
    }

    wlr_xcursor_manager* oldManager = m_xcursorManager;
    m_xcursorManager = manager;
    m_xcursorTheme = configured.theme;
    m_xcursorSize = configured.size;

    const bool hasXwaylandClientCursor = m_clientCursorFromXwayland && m_clientCursorSurface != nullptr;
    if (hasXwaylandClientCursor) {
      if (const std::optional<std::string> match = matchXwaylandCursor()) {
        m_clientCursorShape = *match;
      } else {
        m_clientCursorShape.clear();
      }
    }

    if (m_compositorOwnsCursor) {
      setXcursor(m_compositorCursorName.c_str());
    } else if (hasXwaylandClientCursor) {
      applyClientCursor();
    } else if (m_activeXcursorManager == oldManager) {
      setXcursor(m_activeXcursorName.c_str());
    } else if (m_server->seat()->wlr()->pointer_state.focused_surface == nullptr) {
      setXcursor("default");
    }
    if (hasXwaylandClientCursor && !m_clientCursorShape.empty()) {
      finishClientCursorFrame();
    }
    // Xwayland's default cursor is a buffer of the manager's image, so it moves over before the old manager dies.
    if (Xwayland* xwayland = m_server->xwayland()) {
      xwayland->applyCursor(manager);
    }
    wlr_xcursor_manager_destroy(oldManager);
  }

  void Cursor::noteActivity() {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr && (capture->capabilities() & 2U) != 0) {
      return;
    }
    if (m_cursorHidden) {
      m_cursorHidden = false;
      forwardEffectPointer();
      if (m_compositorOwnsCursor) {
        setXcursor(m_compositorCursorName.c_str());
      } else {
        restoreClientCursor();
      }
    }
    updateHideTimer();
  }

  void Cursor::noteTyping() {
    if (config().input.cursor.hideWhenTyping) {
      hideCursor();
    }
  }

  void Cursor::updateHideTimer() {
    const int timeoutMs = config().input.cursor.hideTimeoutMs;
    if (timeoutMs == 0) {
      if (m_hideTimer != nullptr) {
        wl_event_source_timer_update(m_hideTimer, 0);
      }
      if (m_cursorHidden) {
        m_cursorHidden = false;
        forwardEffectPointer();
        if (m_compositorOwnsCursor) {
          setXcursor(m_compositorCursorName.c_str());
        } else {
          restoreClientCursor();
        }
      }
      return;
    }
    if (m_hideTimer == nullptr) {
      m_hideTimer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onHideTimer, this);
      if (m_hideTimer == nullptr) {
        return;
      }
    }
    if (!m_cursorHidden) {
      wl_event_source_timer_update(m_hideTimer, timeoutMs);
    }
  }

  void Cursor::hideCursor() {
    // Detaching the cursor surface in the middle of an implicit pointer grab
    // disrupts simultaneous mouse and keyboard input in games and other
    // interactive clients. The release restarts the inactivity timer, and a
    // later keypress can hide the cursor normally.
    if (m_cursorHidden || m_server->seat()->wlr()->pointer_state.button_count != 0) {
      return;
    }
    m_cursorHidden = true;
    forwardEffectPointer();
    wlr_cursor_set_surface(m_cursor, nullptr, 0, 0);
  }

  void Cursor::forwardEffectPointer() const {
    if (m_server->effects().cursorEffectActive()) {
      m_server->effects().pointerMoved(m_cursor->x, m_cursor->y, !m_cursorHidden);
    }
  }

  void Cursor::handleOutputLayoutChange() const {
    m_server->effects().forgetPointerOutput();
    forwardEffectPointer();
  }

  int Cursor::onHideTimer(void* data) {
    static_cast<Cursor*>(data)->hideCursor();
    return 0;
  }

  bool Cursor::tiledMoveDragActive() const {
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    return grab != nullptr && grab->view != nullptr && !grab->pending && grab->target == DragTarget::Tiled;
  }

  void Cursor::setCursorSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY, wl_client* owner) {
    const bool sameSurface = m_clientCursorKnown && surface != nullptr && surface == m_clientCursorSurface;
    const bool sameHotspot = sameSurface && hotspotX == m_clientCursorHotspotX && hotspotY == m_clientCursorHotspotY;
    const bool keepPromotion = sameHotspot && m_clientCursorFromXwayland && !m_clientCursorShape.empty();
    if (!sameSurface) {
      forgetClientCursor();
      m_clientCursorKnown = true;
      m_clientCursorOwner = owner;
      if (owner != nullptr) {
        m_clientCursorOwnerDestroy.connectClient(owner, [this](void* data) { onClientCursorOwnerDestroy(this, data); });
      }
      m_clientCursorSurface = surface;
      if (surface != nullptr) {
        Xwayland* xwayland = m_server->xwayland();
        m_clientCursorFromXwayland = xwayland != nullptr && xwayland->ownsSurface(surface);
        m_clientCursorDestroy.connect(&surface->events.destroy, this, onClientCursorDestroy);
        if (m_clientCursorFromXwayland) {
          m_clientCursorClientCommit.connect(&surface->events.client_commit, this, onClientCursorClientCommit);
          m_clientCursorCommit.connect(&surface->events.commit, this, onClientCursorCommit);
          if (surface->buffer != nullptr) {
            captureXwaylandCursorImage(&surface->buffer->base);
          }
        }
      }
    }
    m_clientCursorHotspotX = hotspotX;
    m_clientCursorHotspotY = hotspotY;
    // Xwayland may commit a static cursor before assigning it to the pointer.
    // Inspect that already-current buffer now; later animation and cursor
    // changes arrive through m_clientCursorCommit.
    if ((!sameSurface || !sameHotspot) && m_clientCursorFromXwayland && !m_clientCursorPixels.empty()) {
      handleClientCursorCommit();
      if (m_compositorOwnsCursor || !m_clientCursorShape.empty()) {
        return;
      }
    } else if (!sameHotspot) {
      m_clientCursorShape.clear();
    }
    if (m_compositorOwnsCursor) {
      // Replayed when the override ends.
      return;
    }
    if (keepPromotion) {
      setXcursor(m_clientCursorShape.c_str());
      return;
    }
    if (!m_cursorHidden) {
      wlr_cursor_set_surface(m_cursor, surface, hotspotX, hotspotY);
    }
    m_activeXcursorManager = nullptr;
    m_activeXcursorName.clear();
  }

  void Cursor::setCursorShape(const char* name, wl_client* owner) {
    forgetClientCursor();
    m_clientCursorKnown = true;
    m_clientCursorOwner = owner;
    if (owner != nullptr) {
      m_clientCursorOwnerDestroy.connectClient(owner, [this](void* data) { onClientCursorOwnerDestroy(this, data); });
    }
    m_clientCursorShape = name;
    if (m_compositorOwnsCursor) {
      return;
    }
    setXcursor(name);
  }

  void Cursor::applyClientCursor() {
    if (!m_clientCursorKnown) {
      setXcursor("default");
      return;
    }
    if (!m_clientCursorShape.empty()) {
      setXcursor(m_clientCursorShape.c_str());
      return;
    }
    if (!m_cursorHidden) {
      wlr_cursor_set_surface(m_cursor, m_clientCursorSurface, m_clientCursorHotspotX, m_clientCursorHotspotY);
    }
    m_activeXcursorManager = nullptr;
    m_activeXcursorName.clear();
  }

  void Cursor::captureXwaylandCursorImage(wlr_buffer* buffer) {
    m_clientCursorPixels.clear();
    m_clientCursorImageWidth = 0;
    m_clientCursorImageHeight = 0;
    if (buffer == nullptr || buffer->width <= 0 || buffer->height <= 0) {
      return;
    }

    void* pixels = nullptr;
    uint32_t format = DRM_FORMAT_INVALID;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format, &stride)) {
      return;
    }

    constexpr size_t kBytesPerPixel = sizeof(uint32_t);
    const auto width = static_cast<uint32_t>(buffer->width);
    const auto height = static_cast<uint32_t>(buffer->height);
    const bool widthOverflows = width > std::numeric_limits<size_t>::max() / kBytesPerPixel;
    const size_t rowBytes = widthOverflows ? 0 : static_cast<size_t>(width) * kBytesPerPixel;
    const bool sizeOverflows = rowBytes == 0 || height > std::numeric_limits<size_t>::max() / rowBytes;
    if (format == DRM_FORMAT_ARGB8888 && pixels != nullptr && stride >= rowBytes && !sizeOverflows) {
      m_clientCursorPixels.resize(rowBytes * height);
      const auto* source = static_cast<const uint8_t*>(pixels);
      for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(
            m_clientCursorPixels.data() + static_cast<size_t>(y) * rowBytes, source + static_cast<size_t>(y) * stride,
            rowBytes
        );
      }
      m_clientCursorImageWidth = width;
      m_clientCursorImageHeight = height;
    }
    wlr_buffer_end_data_ptr_access(buffer);
  }

  std::optional<std::string> Cursor::matchXwaylandCursor() const {
    if (!m_clientCursorFromXwayland
        || m_clientCursorSurface == nullptr
        || m_clientCursorSurface->current.scale != 1
        || m_clientCursorSurface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL
        || m_clientCursorSurface->current.viewport.has_src
        || m_clientCursorSurface->current.viewport.has_dst
        || m_clientCursorPixels.empty()
        || m_clientCursorSurface->current.width != static_cast<int>(m_clientCursorImageWidth)
        || m_clientCursorSurface->current.height != static_cast<int>(m_clientCursorImageHeight)
        || m_clientCursorHotspotX < 0
        || m_clientCursorHotspotY < 0) {
      return std::nullopt;
    }

    return matchXcursorManagerImage(
        m_xcursorManager,
        {
            .width = m_clientCursorImageWidth,
            .height = m_clientCursorImageHeight,
            .hotspotX = static_cast<uint32_t>(m_clientCursorHotspotX),
            .hotspotY = static_cast<uint32_t>(m_clientCursorHotspotY),
            .pixels = m_clientCursorPixels.data(),
            .stride = static_cast<size_t>(m_clientCursorImageWidth) * sizeof(uint32_t),
        },
        m_clientCursorShape
    );
  }

  void Cursor::finishClientCursorFrame() const {
    if (m_clientCursorSurface == nullptr) {
      return;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_surface_send_frame_done(m_clientCursorSurface, &now);
  }

  void Cursor::handleClientCursorCommit() {
    const bool wasPromoted = !m_clientCursorShape.empty();
    if (const std::optional<std::string> match = matchXwaylandCursor()) {
      m_clientCursorShape = *match;
      if (!m_compositorOwnsCursor) {
        setXcursor(m_clientCursorShape.c_str());
      }
      // Xwayland waits for this callback before committing an animated cursor's
      // next frame. wlroots no longer owns the source surface after promotion.
      finishClientCursorFrame();
      return;
    }

    m_clientCursorShape.clear();
    if (!m_compositorOwnsCursor) {
      if (wasPromoted && !m_cursorHidden) {
        wlr_cursor_set_surface(m_cursor, m_clientCursorSurface, m_clientCursorHotspotX, m_clientCursorHotspotY);
      }
      m_activeXcursorManager = nullptr;
      m_activeXcursorName.clear();
    }
    // wlroots paces a visible raw surface. When Andromeda has detached it for a
    // compositor override or cursor hiding, complete Xwayland's callback here.
    if (m_compositorOwnsCursor || m_cursorHidden) {
      finishClientCursorFrame();
    }
  }

  void Cursor::forgetClientCursor() {
    m_clientCursorOwnerDestroy.disconnect();
    m_clientCursorDestroy.disconnect();
    m_clientCursorClientCommit.disconnect();
    m_clientCursorCommit.disconnect();
    m_clientCursorKnown = false;
    m_clientCursorOwner = nullptr;
    m_clientCursorSurface = nullptr;
    m_clientCursorHotspotX = 0;
    m_clientCursorHotspotY = 0;
    m_clientCursorShape.clear();
    m_clientCursorFromXwayland = false;
    m_clientCursorPixels.clear();
    m_clientCursorImageWidth = 0;
    m_clientCursorImageHeight = 0;
  }

  void Cursor::onClientCursorOwnerDestroy(Cursor* self, void* /*data*/) {
    self->forgetClientCursor();
    if (!self->m_compositorOwnsCursor) {
      self->setXcursor("default");
    }
  }

  void Cursor::onClientCursorDestroy(Cursor* self, void* /*data*/) {
    const bool promoted = self->m_clientCursorSurface != nullptr && !self->m_clientCursorShape.empty();
    self->forgetClientCursor();
    if (promoted && !self->m_compositorOwnsCursor) {
      self->setXcursor("default");
    }
  }

  void Cursor::onClientCursorClientCommit(Cursor* self, void* /*data*/) {
    wlr_surface_state& pending = self->m_clientCursorSurface->pending;
    if ((pending.committed & WLR_SURFACE_STATE_BUFFER) != 0) {
      self->captureXwaylandCursorImage(pending.buffer);
    }
  }

  void Cursor::onClientCursorCommit(Cursor* self, void* /*data*/) {
    self->m_clientCursorHotspotX -= self->m_clientCursorSurface->current.dx;
    self->m_clientCursorHotspotY -= self->m_clientCursorSurface->current.dy;
    self->handleClientCursorCommit();
  }

  void Cursor::notePointerFocusChange(wlr_surface* newSurface) {
    if (newSurface != nullptr
        && m_clientCursorOwner != nullptr
        && wl_resource_get_client(newSurface->resource) == m_clientCursorOwner) {
      return;
    }
    forgetClientCursor();
    if (newSurface == nullptr && !m_compositorOwnsCursor) {
      setXcursor("default");
    }
  }

  const char* Cursor::effectiveXcursor(const char* name) const {
    return m_launchBusy && std::string_view(name) == "default" ? "progress" : name;
  }

  void Cursor::setLaunchBusy(bool busy) {
    if (m_launchBusy == busy) {
      return;
    }
    m_launchBusy = busy;
    if (!m_cursorHidden && m_activeXcursorManager != nullptr && m_activeXcursorName == "default") {
      wlr_cursor_set_xcursor(m_cursor, m_activeXcursorManager, effectiveXcursor("default"));
    }
  }

  void Cursor::setXcursor(const char* name) {
    if (!m_cursorHidden) {
      wlr_cursor_set_xcursor(m_cursor, m_xcursorManager, effectiveXcursor(name));
    }
    m_activeXcursorManager = m_xcursorManager;
    m_activeXcursorName = name;
  }

#ifdef ANDROMEDA_TEST_IPC
  std::string Cursor::clientCursorSourceForTest() const {
    if (!m_clientCursorKnown) {
      return "none";
    }
    if (!m_clientCursorShape.empty()) {
      return "xcursor";
    }
    return m_clientCursorSurface != nullptr ? "surface" : "hidden";
  }

  std::optional<Cursor::RenderedCursorStateForTest> Cursor::renderedCursorStateForTest() const {
    wlr_output* output = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    if (output == nullptr) {
      return std::nullopt;
    }

    wlr_output_cursor* outputCursor = nullptr;
    wl_list_for_each(outputCursor, &output->cursors, link) {
      if (outputCursor->enabled && outputCursor->visible && outputCursor->texture != nullptr) {
        return RenderedCursorStateForTest{
            .textureWidth = outputCursor->texture->width,
            .textureHeight = outputCursor->texture->height,
            .renderWidth = outputCursor->width,
            .renderHeight = outputCursor->height,
        };
      }
    }
    return std::nullopt;
  }
#endif

  bool Cursor::isPassthrough() const { return std::holds_alternative<PassthroughGrab>(m_grab); }

  View* Cursor::grabbedView() const {
    if (const auto* grab = std::get_if<MoveGrab>(&m_grab)) {
      return grab->view;
    }
    if (const auto* grab = std::get_if<FloatingResizeGrab>(&m_grab)) {
      return grab->view;
    }
    if (const auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
      return grab->view;
    }
    if (const auto* grab = std::get_if<PendingResizeGrab>(&m_grab)) {
      return grab->view;
    }
    return nullptr;
  }

  bool Cursor::isDraggingView(const View* view) const {
    if (view == nullptr) {
      return false;
    }
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    return grab != nullptr && grab->view == view && !grab->pending;
  }

  bool Cursor::isDraggingIntoLayout() const {
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    return grab != nullptr && !grab->pending && grab->target == DragTarget::Tiled;
  }

  bool Cursor::isResizingView(const View* view) const {
    if (view == nullptr) {
      return false;
    }
    if (const auto* grab = std::get_if<FloatingResizeGrab>(&m_grab)) {
      return grab->view == view;
    }
    const auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    return grab != nullptr && grab->view == view;
  }

  bool Cursor::isResizingWorkspace(const Workspace* workspace) const {
    const auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    return workspace != nullptr && grab != nullptr && grab->workspace == workspace;
  }

  void Cursor::beginToplevelDrag(View* view, int x, int y, bool tileOnDrop) {
    const uint32_t button = m_server->seat()->wlr()->pointer_state.grab_button;
    if (!beginMove(view, button)) {
      return;
    }
    m_toplevelDrag = true;
    auto& grab = std::get<MoveGrab>(m_grab);
    grab.offsetX = x;
    grab.offsetY = y;
    if (tileOnDrop && !view->pinned()) {
      grab.target = DragTarget::Tiled;
      grab.sourceWorkspace = view->workspace();
    }
    if (grab.pending) {
      beginDrag(grab);
    }
    processMotion(monotonicMsec(), m_cursor->x, m_cursor->y, false);
  }

  void Cursor::finishToplevelDrag(bool place) {
    if (!m_toplevelDrag) {
      return;
    }
    if (place && !m_server->sessionLocked()) {
      finishMove();
    } else {
      resetMode();
    }
  }

  std::optional<uint32_t>
  Cursor::clientPointerGrabButton(const View* view, wlr_seat_client* seatClient, uint32_t serial) const {
    if (view == nullptr || !isPassthrough()) {
      return std::nullopt;
    }
    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* focused = seat->pointer_state.focused_surface;
    if (seat->drag != nullptr
        || wlr_seat_pointer_has_grab(seat)
        || focused == nullptr
        || wlr_surface_get_root_surface(focused) != view->rootSurface()
        || seat->pointer_state.button_count != 1) {
      return std::nullopt;
    }
    // X11 requests carry no seat client or serial; the pressed pointer on the window is their only credential.
    if (seatClient == nullptr) {
      return view->xwayland() ? std::optional(seat->pointer_state.grab_button) : std::nullopt;
    }
    if (seatClient->seat != seat || !wlr_seat_validate_pointer_grab_serial(seat, focused, serial)) {
      return std::nullopt;
    }
    return seat->pointer_state.grab_button;
  }

  void Cursor::beginClientMove(View* view, wlr_seat_client* seatClient, uint32_t serial) {
    const std::optional<uint32_t> button = clientPointerGrabButton(view, seatClient, serial);
    if (!button.has_value()
        || *button == 0
        || !m_clientPointerPress
        || m_clientPointerPress->button != *button
        || m_clientPointerPress->serial != m_server->seat()->wlr()->pointer_state.grab_serial
        || !beginMove(view, *button, true)) {
      return;
    }
    auto& grab = std::get<MoveGrab>(m_grab);
    grab.startX = m_clientPointerPress->x;
    grab.startY = m_clientPointerPress->y;
    // xdg-shell transfers this device away from the client for an accepted
    // interactive operation. The notify variant also retires wlroots' implicit
    // button grab; m_grabButton keeps the raw release needed to finish ours.
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
    // The client may already have crossed the threshold before requesting the move. Promote it now without another
    // motion event, keeping the current pointer grip instead of jumping back to the press position.
    processMotion(monotonicMsec(), m_cursor->x, m_cursor->y, false);
  }

  void Cursor::beginClientResize(View* view, wlr_seat_client* seatClient, uint32_t serial, uint32_t edges) {
    const std::optional<uint32_t> button = clientPointerGrabButton(view, seatClient, serial);
    if (!button.has_value() || *button == 0 || !beginResize(view, edges, *button)) {
      return;
    }
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
  }

  void Cursor::warpTo(double lx, double ly) { warpTo(lx, ly, true); }

  void Cursor::warpToPreservingFocus(double lx, double ly) { warpTo(lx, ly, false); }

  bool Cursor::warpToView(View& view) {
    Output* output = view.currentOutput();
    if (output == nullptr) {
      return false;
    }

    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }

    wlr_box target = view.presentedBox();
    Workspace* workspace = view.workspace();
    if (view.layoutFullscreen()) {
      target = outputBox;
    } else if (view.maximizedToEdges()) {
      target = output->usableArea();
    } else if (view.tiled() && workspace != nullptr) {
      // A focus or move may have updated the scrolling offset and marked the layout stale. Flush it before reading the
      // final logical target, while leaving its visual transition animated.
      workspace->flushArrange();
      target = workspace->layout().targetBox(&view);
    } else {
      target.x = view.layoutTargetX();
      target.y = view.layoutTargetY();
    }

    if (target.width <= 0 || target.height <= 0) {
      target = outputBox;
    }
    wlr_box visible{};
    if (!wlr_box_intersection(&visible, &target, &outputBox)) {
      visible = outputBox;
    }
    warpToPreservingFocus(visible.x + visible.width / 2.0, visible.y + visible.height / 2.0);
    return true;
  }

  void Cursor::warpTo(double lx, double ly, bool allowFocusChange) {
    noteActivity();
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_warp(m_cursor, nullptr, lx, ly);
    m_server->notifyIdleActivity();
    processMotion(monotonicMsec(), oldX, oldY, allowFocusChange);
  }

  void Cursor::cancelStaleTiledResize() {
    if (const auto* pending = std::get_if<PendingResizeGrab>(&m_grab); pending != nullptr) {
      // The deferred press carries press-time edges; a layout swap since then
      // makes them mean something else.
      if (pending->view == nullptr
          || (pending->layout != nullptr
              && (pending->view->workspace() != pending->workspace
                  || &pending->workspace->layout() != pending->layout))) {
        resetMode();
      }
      return;
    }
    const auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    if (grab != nullptr
        && (grab->workspace == nullptr
            || grab->session == nullptr
            || grab->session->ownerLayout() != &grab->workspace->layout())) {
      resetMode();
    }
  }

  void Cursor::cancelLayoutInteraction() {
    // An axis change keeps the same layout object, so cancelStaleTiledResize()
    // cannot see it; the session's edges would still mean the old orientation.
    // A deferred resize press stores the same kind of press-time edges.
    if (std::holds_alternative<ScrollDragGrab>(m_grab)
        || std::holds_alternative<TiledResizeGrab>(m_grab)
        || std::holds_alternative<PendingResizeGrab>(m_grab)) {
      resetMode();
    }
  }

  void Cursor::onMotion(Cursor* self, void* data) {
    self->handleMotion(data);
  }

  void Cursor::onMotionAbsolute(Cursor* self, void* data) {
    self->handleMotionAbsolute(data);
  }

  void Cursor::onButton(Cursor* self, void* data) {
    self->handleButton(data);
  }

  void Cursor::onAxis(Cursor* self, void* data) {
    self->handleAxis(data);
  }

  void Cursor::onFrame(Cursor* self, void* /*data*/) {
    self->handleFrame();
  }

  void Cursor::handleMotion(void* data) {
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();

    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      if (m_server->physicalInputDevice(&event->pointer->base)) {
        if (capture->captureMotion(
                event->time_msec, m_cursor->x, m_cursor->y, event->delta_x, event->delta_y, canCaptureInput()
            )) {
          return;
        }
      } else if ((capture->capabilities() & 2U) != 0) {
        return;
      }
    }

    wlr_relative_pointer_manager_v1_send_relative_motion(
        m_server->relativePointerManager(), m_server->seat()->wlr(), static_cast<uint64_t>(event->time_msec) * 1000,
        event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy
    );

    if (m_activeConstraint != nullptr && !constraintSurfaceActive()) {
      clearConstraint();
    }
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      return;
    }

    double dx = event->delta_x;
    double dy = event->delta_y;
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      if (!confineDelta(&dx, &dy)) {
        return;
      }
    }

    confineRuleDelta(&dx, &dy);
    promotePendingModRightResize(m_cursor->x + dx, m_cursor->y + dy);
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleMotionAbsolute(void* data) {
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();
    if (m_activeConstraint != nullptr && !constraintSurfaceActive()) {
      clearConstraint();
    }
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      return;
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      if (m_server->physicalInputDevice(&event->pointer->base)) {
        if (capture->captureAbsoluteMotion(
                event->time_msec, m_cursor->x, m_cursor->y, lx, ly, canCaptureInput(), &event->pointer->base
            )) {
          return;
        }
      } else if ((capture->capabilities() & 2U) != 0) {
        return;
      }
    }

    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    double dx = lx - m_cursor->x;
    double dy = ly - m_cursor->y;
    if (m_activeConstraint != nullptr
        && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED
        && !confineDelta(&dx, &dy)) {
      return;
    }
    confineRuleDelta(&dx, &dy);
    promotePendingModRightResize(m_cursor->x + dx, m_cursor->y + dy);
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleButton(void* data) {
    auto* event = static_cast<wlr_pointer_button_event*>(data);
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr && (capture->capabilities() & 2U) != 0) {
      m_server->notifyIdleActivity();
      if (m_server->physicalInputDevice(&event->pointer->base)) {
        capture->captureButton(event->time_msec, event->button, event->state == WL_POINTER_BUTTON_STATE_PRESSED);
      }
      return;
    }
    processButton(event->time_msec, event->button, event->state);
  }

  bool Cursor::canCaptureInput() const {
    const wlr_seat* seat = m_server->seat()->wlr();
    return isPassthrough()
        && m_activeConstraint == nullptr
        && !m_server->gestures()->movingContent()
        && seat->pointer_state.button_count == 0
        && seat->drag == nullptr
        && seat->keyboard_state.grab == seat->keyboard_state.default_grab
        && wl_list_empty(&seat->touch_state.touch_points)
        && !m_server->sessionLocked()
        && m_server->sessionLock() == nullptr
        && (m_server->session() == nullptr || m_server->session()->active)
        && (m_server->quitConfirm() == nullptr || !m_server->quitConfirm()->visible())
        && (m_server->overview() == nullptr || !m_server->overview()->active());
  }

  void Cursor::setInputCaptured(bool captured) {
    if (captured) {
      cancelHotCorner();
      m_server->cancelModifierTap();
      clearPointerFocus();
      hideCursor();
    } else {
      noteActivity();
      refreshPointerFocus();
    }
  }

  bool Cursor::isModRightDoubleClick(uint32_t timeMsec, const View* view) const {
    return m_lastModRightClick.view == view
        && timeMsec - m_lastModRightClick.timeMsec <= kDoubleClickMsec
        && std::abs(m_cursor->x - m_lastModRightClick.x) <= kDoubleClickSlop
        && std::abs(m_cursor->y - m_lastModRightClick.y) <= kDoubleClickSlop;
  }

  void Cursor::recordModRightClick(uint32_t timeMsec, View* view) {
    m_lastModRightClick = ModRightClick{timeMsec, m_cursor->x, m_cursor->y, view};
  }

  void Cursor::promotePendingModRightResize(double nextX, double nextY) {
    const auto* pending = std::get_if<PendingResizeGrab>(&m_grab);
    if (pending == nullptr) {
      return;
    }
    if (m_server->sessionLocked()) {
      resetMode();
      return;
    }
    if (std::abs(nextX - pending->startX) <= kDoubleClickSlop
        && std::abs(nextY - pending->startY) <= kDoubleClickSlop) {
      return;
    }
    const PendingResizeGrab grab = *pending;
    // A drag is not a double-click's first press.
    m_lastModRightClick = ModRightClick{};
    beginResize(grab.view, grab.edges, grab.button);
  }

  void Cursor::processButton(uint32_t timeMsec, uint32_t button, wl_pointer_button_state state) {
    if (m_trackerDrag && m_trackerDrag->claimed) {
      // The claimed drag swallows every button until the left one comes up.
      if (state == WL_POINTER_BUTTON_STATE_RELEASED && button == BTN_LEFT) {
        m_server->dskSurfaces()->endSwipe(timeMsec);
        m_trackerDrag.reset();
        refreshPointerFocus();
      }
      return;
    }
    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      m_trackerDrag.reset();
    } else if (button == BTN_LEFT && m_server->dskSurfaces() != nullptr && !m_server->sessionLocked()) {
      const wlr_seat_pointer_state& pointer = m_server->seat()->wlr()->pointer_state;
      if (pointer.focused_surface != nullptr) {
        if (const std::optional<bool> horizontal =
                m_server->dskSurfaces()->dragAxisHorizontal(pointer.focused_surface, pointer.sx, pointer.sy)) {
          m_trackerDrag = TrackerDrag{
              .surface = pointer.focused_surface,
              .sx = pointer.sx,
              .sy = pointer.sy,
              .startX = m_cursor->x,
              .startY = m_cursor->y,
              .horizontal = *horizontal,
          };
        }
      }
    }
    if (state == WL_POINTER_BUTTON_STATE_RELEASED && m_clientPointerPress && m_clientPointerPress->button == button) {
      m_clientPointerPress.reset();
    }
    noteActivity();
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      cancelHotCorner();
      m_server->cancelModifierTap();
      // Any pointer press cancels the confirmation without being consumed; the
      // click still reaches whatever it hit.
      if (QuitConfirm* confirm = m_server->quitConfirm(); confirm != nullptr && confirm->visible()) {
        m_server->dismissConfirmation();
      }
    }

    // Config mouse binds win over the overview and the built-in Mod+drag / Mod+resize grabs. Presses consumed here
    // swallow their paired release so clients never see an unmatched release.
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && isPassthrough()) {
      const uint32_t modifiers = m_server->keyboardModifiers();
      bool mouseBindExecuted = false;
      const std::optional<Keybind> bound = m_server->handleMouseBind(button, modifiers, &mouseBindExecuted);
      if (bound.has_value()) {
        if (mouseBindExecuted
            && bound->action == KeybindAction::LayoutScrollDrag
            && m_server->gestures()->beginPointerScroll(m_cursor->x, m_cursor->y)) {
          setActiveConstraint(nullptr);
          m_grab = ScrollDragGrab{
              .button = button,
              .lastX = m_cursor->x,
              .lastY = m_cursor->y,
          };
          m_grabButton = button;
          setCompositorCursor("grabbing");
          clearPointerFocus();
          return;
        }
        m_swallowedButtons.push_back(button);
        return;
      }
    }
    if (state == WL_POINTER_BUTTON_STATE_RELEASED && button == BTN_LEFT && m_ssdButtonPress.view != nullptr) {
      const SsdButtonPress press = m_ssdButtonPress;
      m_ssdButtonPress = {};
      if (press.view->ssdFrame() != nullptr)
        press.view->ssdFrame()->setPressed(SsdFramePart::None);
      const auto hit = m_server->ssdFrameAt(m_cursor->x, m_cursor->y);
      if (hit
          && hit->view == press.view
          && hit->part == press.part
          && m_server->exclusiveKeyboardLayer() == nullptr
          && !press.view->blocked()) {
        if (press.part == SsdFramePart::Close) {
          press.view->requestClose();
        } else if (press.part == SsdFramePart::Maximize && press.view->ssdFrame()->maximizeEnabled()) {
          const bool maximized =
              press.view->maximizedToEdges() || press.view->scheduledMaximized() || press.view->floatingMaximized();
          press.view->setMaximizedLikeClientRequest(!maximized);
        } else if (press.part == SsdFramePart::Minimize) {
          press.view->requestMinimize();
        }
      }
    }
    if (state == WL_POINTER_BUTTON_STATE_RELEASED && std::erase(m_swallowedButtons, button) > 0) {
      return;
    }

    // A client data-device drag owns the seat grab. Its initiating release must reach wlroots even when the drag began
    // from a panel over the overview. Otherwise the drag icon and both input grabs remain active indefinitely.
    if (wlr_seat* seat = m_server->seat()->wlr(); seat->drag != nullptr) {
      m_server->seat()->notifyPointerModifiers();
      if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        m_server->seat()->toplevelDrags().dropWithoutTarget(button);
      }
      wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
      if (seat->drag == nullptr) {
        // The drag grab suppressed normal pointer motion. Re-run hit testing at
        // the unchanged position so the client can restore its hover cursor.
        processMotion(timeMsec, m_cursor->x, m_cursor->y);
      }
      return;
    }

    // An interactive pointer operation ends only when its initiating button is released.
    if (m_grabButton != 0 && button != m_grabButton) {
      if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_swallowedButtons.push_back(button);
        toggleDragTarget(button);
      }
      return;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      if (auto* grab = std::get_if<ScrollDragGrab>(&m_grab)) {
        if (button == grab->button) {
          m_server->gestures()->endPointerScroll(m_server->sessionLocked(), timeMsec);
          resetMode();
          // The grab cleared client focus on press and consumed every motion. Restore pointer delivery at the final
          // position without letting this synthetic pass override the column selected by the gesture.
          processMotion(timeMsec, m_cursor->x, m_cursor->y, false);
        }
        return;
      }
    }

    // A client that received the press owns the implicit grab, so its release
    // reaches it even though the overview now owns the pointer. Otherwise the
    // button stays down in that client for good.
    const bool releasesClientGrab = state == WL_POINTER_BUTTON_STATE_RELEASED && pointerFocusPinned();

    // Overview owns the pointer while it is up: cards are its own hit-test surface and the desktop underneath is inert.
    // Top/overlay layer surfaces (panels) stay fully interactive.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked() && !releasesClientGrab) {
      const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      wlr_seat* seat = m_server->seat()->wlr();
      if (overviewPassthroughLayer(layer) && !overview->dragging()) {
        if (surface != nullptr) {
          setPointerFocus(surface, sx, sy, timeMsec);
        }
        m_server->seat()->notifyPointerModifiers();
        wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
        // The popup's xdg-shell grab already owns focus. Refocusing its parent layer would end the keyboard grab, whose
        // wlroots cancel handler also ends the pointer grab before the menu receives the matching release.
        if (pressed && !isXdgPopupSurface(surface)) {
          layer->focus();
        }
        return;
      }
      overview->handleButton(button, pressed, m_cursor->x, m_cursor->y, timeMsec);
      return;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      if (auto* grab = std::get_if<MoveGrab>(&m_grab)) {
        if (grab->pending) {
          resetMode();
        } else {
          finishMove();
        }
        return;
      }
      if (auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
        if (grab->workspace != nullptr) {
          if (grab->workspace->focusedView() == grab->view) {
            grab->workspace->reevaluateFocusedColumn();
          }
          grab->workspace->markArrange(true);
        }
        resetMode();
        return;
      }
      if (std::holds_alternative<FloatingResizeGrab>(m_grab)) {
        resetMode();
        return;
      }
      if (const auto* pending = std::get_if<PendingResizeGrab>(&m_grab)) {
        revealClickedTile(pending->view);
        // A deferred Mod+right resize that never left the click's slop ends as
        // a plain click. Its press reached no client, so swallow the release
        // too rather than deliver an unmatched one; the click pair stays
        // recorded for the double-click.
        resetMode();
        return;
      }
      m_server->seat()->notifyPointerModifiers();
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), timeMsec, button, state);

      // After the final release, realign pointer focus (pinned by the implicit grab) with the surface under the cursor,
      // retaining panel focus in overview so release-driven menus keep their pointer anchor.
      if (m_server->seat()->wlr()->pointer_state.button_count == 0) {
        refreshPointerFocus();
      }

      resetMode();
      return;
    }

    // An implicit grab belongs to the surface that received the first press.
    // Route additional presses there until every button has been released.
    if (wlr_seat* seat = m_server->seat()->wlr(); seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      m_server->seat()->notifyPointerModifiers();
      wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (m_server->sessionLocked()) {
      m_server->seat()->notifyPointerModifiers();
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), timeMsec, button, state);
      if (surface != nullptr) {
        if (wlr_session_lock_surface_v1* lockSurface = wlr_session_lock_surface_v1_try_from_wlr_surface(surface)) {
          if (auto* node = static_cast<LockSurface*>(lockSurface->data)) {
            node->focus();
          }
        }
      }
      return;
    }

    // Scene hit testing lets covered chrome stay covered by windows and panels.
    if (m_server->exclusiveKeyboardLayer() == nullptr
        && isPassthrough()
        && !wlr_seat_pointer_has_grab(m_server->seat()->wlr())) {
      if (const auto hit = m_server->ssdFrameAt(m_cursor->x, m_cursor->y)) {
        View* target = hit->view;
        clearPointerFocus();
        if (button == BTN_LEFT && hit->part == SsdFramePart::Resize) {
          m_server->focusView(target, FocusReason::Grab);
          if (beginResize(target, hit->edges, button))
            return;
        } else if (button == BTN_LEFT && hit->part == SsdFramePart::TitleBar) {
          m_server->focusView(target, FocusReason::PointerPress);
          const double dx = m_cursor->x - m_lastTitleBarClick.x;
          const double dy = m_cursor->y - m_lastTitleBarClick.y;
          if (m_lastTitleBarClick.view == target
              && timeMsec - m_lastTitleBarClick.timeMsec <= kDoubleClickMsec
              && dx * dx + dy * dy <= kDoubleClickSlop * kDoubleClickSlop) {
            m_lastTitleBarClick = {};
            const bool maximized =
                target->maximizedToEdges() || target->scheduledMaximized() || target->floatingMaximized();
            target->setMaximizedLikeClientRequest(!maximized);
          } else {
            m_lastTitleBarClick = {timeMsec, m_cursor->x, m_cursor->y, target};
            if (beginMove(target, button, true))
              return;
          }
        } else if (
            button == BTN_LEFT
            && !target->blocked()
            && (hit->part == SsdFramePart::Close
                || hit->part == SsdFramePart::Minimize
                || (hit->part == SsdFramePart::Maximize && target->ssdFrame()->maximizeEnabled()))
        ) {
          m_server->focusView(target, FocusReason::PointerPress);
          m_ssdButtonPress = {target, hit->part};
          target->ssdFrame()->setPressed(hit->part);
        }
        m_swallowedButtons.push_back(button);
        return;
      }
    }

    // A press on a tab bar is the compositor's: no client sees it or its release. The left button selects the tab and
    // holds it, so dragging it past the threshold pulls it out of its column like any tiled window.
    if (const std::optional<TabHit> hit = tabBarHitAt(*m_server, m_cursor->x, m_cursor->y, surface, view, layer)) {
      const bool keyboardFree = m_server->exclusiveKeyboardLayer() == nullptr;
      Workspace* home = hit->tab->workspace();
      if (hit->part != TabBarPart::Tab) {
        // A cycle button selects the tab before or after the one on show, wrapping at the ends.
        if (button == BTN_LEFT && keyboardFree && home != nullptr) {
          if (View* target = home->tabs().cycleFrom(hit->tab, hit->part == TabBarPart::PreviousTab ? -1 : 1)) {
            m_server->focusView(target, FocusReason::PointerPress);
          }
        }
      } else if (button == BTN_LEFT && keyboardFree) {
        m_server->focusView(hit->tab, FocusReason::PointerPress);
        if (beginMove(hit->tab, button)) {
          return;
        }
      } else if (
          button == BTN_MIDDLE && keyboardFree && home != nullptr && home->layoutConfig().tabs.middleClickCloses
      ) {
        hit->tab->requestClose();
      }
      m_swallowedButtons.push_back(button);
      return;
    }

    const bool modHeld = (m_server->keyboardModifiers() & m_server->modKey()) != 0;
    if (button == BTN_LEFT && modHeld && view != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      beginMove(view, button);
      return;
    }
    if (button == BTN_RIGHT && modHeld && view != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      if (config().input.modRightDoubleClick == ModRightDoubleClick::None) {
        beginResize(view, view->tiled() ? 0 : floatResizeEdges(view), button);
        return;
      }
      // A second Mod+right-click on the same window within the double-click
      // window applies the opposite maximize state with a titlebar
      // double-click's semantics. The direction is read at this press: the
      // first press only holds a deferred grab (below), so no grab has
      // perturbed the maximize state in between.
      if (isModRightDoubleClick(timeMsec, view)) {
        // Completing a pair clears the record so a third rapid click starts a
        // fresh window. No grab owns this press, so its release would reach the
        // client unmatched; swallow both like the tab-bar path does.
        m_lastModRightClick = ModRightClick{};
        m_swallowedButtons.push_back(button);
        const bool wasMaximized = view->maximizedToEdges() || view->scheduledMaximized() || view->floatingMaximized();
        view->setMaximizedLikeClientRequest(!wasMaximized);
        return;
      }
      recordModRightClick(timeMsec, view);
      // Defer the grab until the pointer leaves the slop: a grab would drop the
      // maximize state the second click reads. Edges are snapshotted at the
      // press point; a press proposing none is a plain click.
      const uint32_t edges = view->tiled() ? tiledResizeEdges(view, m_cursor->x, m_cursor->y) : floatResizeEdges(view);
      if (edges != 0) {
        m_grab = PendingResizeGrab{
            .view = view,
            .button = button,
            .startX = m_cursor->x,
            .startY = m_cursor->y,
            .edges = edges,
            .workspace = view->tiled() ? view->workspace() : nullptr,
            .layout = view->tiled() && view->workspace() != nullptr ? &view->workspace()->layout() : nullptr,
        };
        m_grabButton = button;
        updateInteractiveCursor(view);
      } else if (view->tiled()) {
        revealClickedTile(view);
      }
      return;
    }

    // Pointer focus must match the surface under the cursor before the button
    // event so wl_data_device drag serial validation succeeds.
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface != nullptr) {
      setPointerFocus(surface, sx, sy, timeMsec);
    } else {
      clearPointerFocus();
    }

    m_server->seat()->notifyPointerModifiers();
    const uint32_t serial = wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && serial != 0 && seat->pointer_state.button_count == 1) {
      m_clientPointerPress = ClientPointerPress{.button = button, .serial = serial, .x = m_cursor->x, .y = m_cursor->y};
    }
    if (layer != nullptr) {
      if (!isXdgPopupSurface(surface)) {
        layer->focus();
      }
    } else if (m_server->exclusiveKeyboardLayer() == nullptr) {
      if (view != nullptr) {
        if (!isXdgPopupSurface(surface)) {
          m_server->focusView(view, FocusReason::PointerPress);
        }
      } else if (surface == nullptr || wlr_xwayland_surface_try_from_wlr_surface(surface) == nullptr) {
        // A press on an override-redirect X11 menu leaves the keyboard with the menu.
        wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
        m_server->refocusExplicit(m_server->outputFromWlr(wlrOutput));
      }
    }
  }

  void Cursor::handleAxis(void* data) {
    auto* event = static_cast<wlr_pointer_axis_event*>(data);
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr && (capture->capabilities() & 2U) != 0) {
      m_server->notifyIdleActivity();
      if (m_server->physicalInputDevice(&event->pointer->base)) {
        capture->captureAxis(event->time_msec, event->orientation, event->delta, event->delta_discrete);
      }
      return;
    }
    noteActivity();
    if (event->delta == 0 && event->delta_discrete == 0) {
      m_server->notifyIdleActivity();
    } else {
      m_server->notifyInputActivity();
    }
    m_server->cancelModifierTap();
    cancelHotCorner();

    const uint32_t modifiers = m_server->keyboardModifiers();
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);

    // Determine this event's signed wheel direction from delta and orientation.
    const bool isVertical = event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
    const double rawDelta = event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) : event->delta;
    WheelDirection eventDir;
    if (isVertical) {
      eventDir = rawDelta < 0 ? WheelDirection::Up : WheelDirection::Down;
    } else {
      eventDir = rawDelta < 0 ? WheelDirection::Left : WheelDirection::Right;
    }

    const bool shiftWheel = effective == WLR_MODIFIER_SHIFT && event->source != WL_POINTER_AXIS_SOURCE_FINGER;
    const bool boundShiftWheel = shiftWheel && std::ranges::any_of(config().keybinds, [&](const Keybind& bind) {
                                   return bind.submap == m_server->activeSubmap()
                                       && bind.wheel == eventDir
                                       && effective == (bind.modifiers | (bind.useMod ? m_server->modKey() : 0));
                                 });
    // Shift maps vertical wheel travel onto the horizontal axis. Explicit bindings and panels keep their input.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && (effective == 0 || (shiftWheel && !boundShiftWheel))) {
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (!overviewPassthroughLayer(layer)) {
        if (!overview->interactive()) {
          return;
        }
        if (event->source == WL_POINTER_AXIS_SOURCE_FINGER) {
          resetWheelAccumulation();
          // libinput already applies natural scrolling to axis events.
          overview->handleTouchpadAxis(
              event->pointer, isVertical, event->delta, event->time_msec, m_cursor->x, m_cursor->y
          );
          return;
        }
        const bool overviewVertical = isVertical && !shiftWheel;
        const int axis = overviewVertical ? 0 : 1;
        const double factor =
            overviewVertical ? config().overview.scrollFactorVertical : config().overview.scrollFactorHorizontal;
        m_wheelAccum[axis] +=
            (event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0)
            * factor;
        double& accumulated = m_wheelAccum[axis];
        while (std::abs(accumulated) >= 1.0) {
          overview->handleAxisNotch(overviewVertical, accumulated, m_cursor->x, m_cursor->y);
          accumulated -= std::copysign(1.0, accumulated);
        }
        return;
      }
    }

    // The axis is not going to the filmstrip: a modifier chord, a panel underneath, or no overview at all. Whatever
    // gesture was in flight has lost its input stream.
    if (Overview* overview = m_server->overview()) {
      overview->cancelNavigation();
    }
    // Arm only when a bind matches this exact direction and modifier set.
    bool armed = false;
    for (const Keybind& bind : config().keybinds) {
      if (bind.wheel != eventDir) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? m_server->modKey() : 0);
      if (effective == expected) {
        armed = true;
        break;
      }
    }

    const int orientation = isVertical ? 0 : 1;
    // An unmodified wheel over a tab bar steps through its tabs.
    if (!armed && effective == 0 && scrollTabBar(event, orientation)) {
      return;
    }
    if (!armed) {
      m_wheelAccum[orientation] = 0;
      const double scale = touchpadScrollFactor(event->pointer, isVertical);
      wlr_seat_pointer_notify_axis(
          m_server->seat()->wlr(), event->time_msec, event->orientation, event->delta * scale, event->delta_discrete,
          event->source, event->relative_direction
      );
      return;
    }

    // Accumulate normalized notches.
    const double notches =
        event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
    m_wheelAccum[orientation] += notches;

    double& acc = m_wheelAccum[orientation];
    while (std::abs(acc) >= 1.0) {
      WheelDirection direction;
      if (isVertical) {
        direction = acc < 0 ? WheelDirection::Up : WheelDirection::Down;
      } else {
        direction = acc < 0 ? WheelDirection::Left : WheelDirection::Right;
      }
      if (m_server->handleWheelBind(direction, modifiers)) {
        // A wheel action can move the strip without pointer motion. Recompute
        // the active target so release drops where the pointer now points.
        updateDropTarget();
      }
      acc -= std::copysign(1.0, acc);
    }
  }

  bool Cursor::scrollTabBar(const wlr_pointer_axis_event* event, int orientation) {
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    const std::optional<TabHit> hit = tabBarHitAt(*m_server, m_cursor->x, m_cursor->y, surface, view, layer);
    Workspace* workspace = hit ? hit->tab->workspace() : nullptr;
    if (workspace == nullptr || !workspace->layoutConfig().tabs.scrollSwitchesTabs) {
      return false;
    }
    double& accumulated = m_wheelAccum[orientation];
    accumulated +=
        event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
    while (std::abs(accumulated) >= 1.0) {
      const int direction = accumulated < 0 ? -1 : 1;
      accumulated -= direction;
      if (View* target = workspace->tabs().stepFrom(hit->tab, direction)) {
        m_server->focusView(target, FocusReason::PointerPress);
      }
    }
    return true;
  }

  void Cursor::handleFrame() {
    if (InputCapture* capture = m_server->inputCapture();
        capture != nullptr && capture->captureFrame(monotonicMsec())) {
      return;
    }
    if (Overview* overview = m_server->overview()) {
      overview->handleTouchpadFrame();
    }
    wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
  }

  void Cursor::processMotion(uint32_t timeMsec, double oldX, double oldY, bool allowFocusChange) {
    if (processTrackerDragMotion(timeMsec, oldX, oldY)) {
      return;
    }
    updateHotCorner();
    forwardEffectPointer();
    if (const wlr_drag* drag = m_server->seat()->wlr()->drag; drag != nullptr && drag->dropped) {
      return;
    }
    if (auto* grab = std::get_if<ScrollDragGrab>(&m_grab)) {
      if (m_server->sessionLocked()) {
        m_server->gestures()->endPointerScroll(true, timeMsec);
        resetMode();
      } else {
        m_server->gestures()->updatePointerScroll(m_cursor->x - grab->lastX, m_cursor->y - grab->lastY, timeMsec);
        grab->lastX = m_cursor->x;
        grab->lastY = m_cursor->y;
      }
      return;
    }
    // Overview owns motion: cards follow a drag, panels keep passthrough, and
    // the inert desktop underneath never receives enter/motion or hover focus.
    if (Overview* overview = m_server->overview(); overview != nullptr
        && overview->active()
        && !m_server->sessionLocked()
        && m_server->seat()->wlr()->drag == nullptr) {
      overview->handleMotion(m_cursor->x, m_cursor->y, timeMsec);
      if (overview->dragging()) {
        clearPointerFocus();
        return;
      }
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (overviewPassthroughLayer(layer) && surface != nullptr) {
        setPointerFocus(surface, sx, sy, timeMsec);
        wlr_seat_pointer_notify_motion(m_server->seat()->wlr(), timeMsec, sx, sy);
        return;
      }
      clearPointerFocus();
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      return;
    }

    if (auto* grab = std::get_if<MoveGrab>(&m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        if (grab->pending) {
          constexpr double kDragThreshold = 10.0;
          const double dx = m_cursor->x - grab->startX;
          const double dy = m_cursor->y - grab->startY;
          if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
            return;
          }
          beginDrag(*grab);
        }
        processMove();
        updateDropTarget();
        if (config().input.windowSnapping && grab->target == DragTarget::Floating && !m_toplevelDrag) {
          updateSnapPreview();
        } else {
          clearSnapPreview();
        }
        updateDataDragEdgeScroll();
        // A toplevel drag moves a real window while wlroots keeps delivering
        // data-device enter and motion to the surfaces underneath it.
        if (!m_toplevelDrag) {
          return;
        }
      }
    }
    if (std::holds_alternative<FloatingResizeGrab>(m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        processResize();
        return;
      }
    }
    if (std::holds_alternative<TiledResizeGrab>(m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        processResizeTile();
        return;
      }
    }
    if (std::holds_alternative<PendingResizeGrab>(m_grab)) {
      // Motion inside the slop belongs to the deferred press, not to clients
      // or focus-follows-mouse.
      if (!m_server->sessionLocked()) {
        return;
      }
      resetMode();
    }

    wlr_seat* seat = m_server->seat()->wlr();
    updateDataDragEdgeScroll();
    if (seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      // Keep an implicit grab in the coordinate space established by the press. Re-resolving against the scene
      // would turn compositor-driven window animation into apparent pointer travel and make small clicks look like
      // client drags.
      const double scale = surfaceScale(seat->pointer_state.focused_surface);
      const double sx = seat->pointer_state.sx + ((m_cursor->x - oldX) * scale);
      const double sy = seat->pointer_state.sy + ((m_cursor->y - oldY) * scale);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
      updateConstraintForSurface(seat->pointer_state.focused_surface);
      return;
    }
    updatePointerOutput(allowFocusChange);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (allowFocusChange
        && config().input.focus.followsMouse
        && layer == nullptr
        && view != nullptr
        && view->mapped()) {
      view = hoverFocus(view, &surface, &sx, &sy, &layer, oldX, oldY);
    }

    if (surface != nullptr) {
      setPointerFocus(surface, sx, sy, timeMsec);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
    } else {
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      clearPointerFocus();
    }

    // Update the drag icon after seat motion so drop targets are recognized.
    if (seat->drag != nullptr && seat->drag->icon != nullptr) {
      wlr_scene_node_set_position(
          &m_server->dragIconTree()->node, static_cast<int>(m_cursor->x), static_cast<int>(m_cursor->y)
      );
    }

    const auto frameHit = m_server->ssdFrameAt(m_cursor->x, m_cursor->y);
    View* frameView = frameHit ? frameHit->view : nullptr;
    if (m_ssdHoverView != frameView && m_ssdHoverView != nullptr && m_ssdHoverView->ssdFrame() != nullptr) {
      m_ssdHoverView->ssdFrame()->setHovered(SsdFramePart::None);
    }
    m_ssdHoverView = frameView;
    if (frameView != nullptr)
      frameView->ssdFrame()->setHovered(frameHit->part);
    if (m_ssdButtonPress.view != nullptr && m_ssdButtonPress.view->ssdFrame() != nullptr) {
      m_ssdButtonPress.view->ssdFrame()->setPressed(
          frameHit && frameView == m_ssdButtonPress.view && frameHit->part == m_ssdButtonPress.part
              ? m_ssdButtonPress.part
              : SsdFramePart::None
      );
    }
    updateConstraintForSurface(surface);
    updateInteractiveCursor(view);
  }

  void Cursor::updatePointerOutput(bool allowFocusChange) {
    // Crossing outputs updates keyboard / foreign-toplevel focus so clients that follow the
    // focused screen match preferredOutput() / workspace-switch behavior.
    wlr_output* pointerOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    if (pointerOutput != m_pointerOutput) {
      m_pointerOutput = pointerOutput;
      // `focused` in the workspaces payload is the active workspace of the cursor's output, so crossing heads changes
      // it even when no workspace activates: a switch to a workspace already active elsewhere only warps the cursor.
      m_server->scheduleIpcWorkspacesEvent();
      wlr_seat* seat = m_server->seat()->wlr();
      if (allowFocusChange
          && config().input.focus.followsMouse
          && !wlr_seat_pointer_has_grab(seat)
          && !wlr_seat_keyboard_has_grab(seat)
          && !m_server->sessionLocked()
          && m_server->exclusiveKeyboardLayer() == nullptr) {
        m_server->refocusExplicit(m_server->outputFromWlr(pointerOutput));
      }
    }
  }

  View* Cursor::hoverFocus(
      View* view, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer, double oldX, double oldY
  ) {
    wlr_seat* seat = m_server->seat()->wlr();
    // Hover must preserve a popup's seat grabs. Transferring keyboard focus would cancel the grab and dismiss it.
    // Leave explicit focus actions and outside clicks free to transfer focus through their normal paths.
    if (seat->drag != nullptr
        || wlr_seat_pointer_has_grab(seat)
        || wlr_seat_keyboard_has_grab(seat)
        || m_server->sessionLocked()
        || *layer != nullptr
        || view == nullptr
        || !view->mapped()) {
      return view;
    }
    Workspace* workspace = view->workspace();
    const bool scrollRefocus = workspace != nullptr && workspace->scrollHoverFocusDeferred();
    if (scrollRefocus) {
      // Keep the refresh through rapid scroll retargets and zero-distance synthetic motion, such as button release.
      if (workspace->arrangePending()
          || workspace->layoutMotionActive()
          || m_server->gestures()->movingContent()
          || (oldX == m_cursor->x && oldY == m_cursor->y)) {
        return view;
      }
      workspace->resumeScrollHoverFocus();
    }
    // Consume the invalidation only after every hover-focus eligibility gate. PointerHover never arms it, so layout
    // motion caused by this focus cannot turn into another focus on the next input event.
    const bool refocus = scrollRefocus || m_hoverFocusInvalidated;
    m_hoverFocusInvalidated = false;
    wlr_surface* oldSurface = nullptr;
    double oldSx = 0;
    double oldSy = 0;
    View* oldView = m_server->viewAt(oldX, oldY, &oldSurface, &oldSx, &oldSy);
    // A transient can open while the pointer remains over its parent. Refreshing hover within that ancestor must
    // leave the child focused so the pointer can reach it; crossing back into the ancestor still transfers focus.
    bool hoveredAncestor = false;
    const View* focused = View::fromSurface(seat->keyboard_state.focused_surface);
    if (!scrollRefocus && refocus && view == oldView && focused != nullptr && focused->workspace() == workspace) {
      for (const View* parent = focused->transientParent(); parent != nullptr; parent = parent->transientParent()) {
        if (parent == view) {
          hoveredAncestor = true;
          break;
        }
      }
    }
    // Keyboard focus is an escape from rule confinement. While the pointer remains inside that window, an
    // invalidated hover must not immediately focus it again. A real re-entry (or a click) can still focus it.
    const bool entered = view != oldView || (refocus && !hoveredAncestor && !ruleConstraintApplies(*view));
    // Workspace focus is remembered independently from the seat. A pinned window from another workspace can own the
    // seat while this view remains its active workspace's remembered focus, so only seat-global activation of the
    // window that would take the keyboard (a dialog attached to this one) makes this handoff redundant.
    const bool alreadyFocused = view->placementRoot()->inputDelegate()->activated();
    if (entered && !alreadyFocused) {
      m_server->focusView(view, FocusReason::PointerHover);
      // Scroll may have moved another surface under the cursor; refresh hit-test for
      // pointer notify only. Keyboard focus stays on the entered view until a real enter.
      view = m_server->viewAt(m_cursor->x, m_cursor->y, surface, sx, sy, layer);
    }
    return view;
  }

  void Cursor::onTabletToolAxis(Cursor* self, void* data) {
    self->handleTabletToolAxis(data);
  }

  void Cursor::onTabletToolProximity(Cursor* self, void* data) {
    self->handleTabletToolProximity(data);
  }

  void Cursor::onTabletToolTip(Cursor* self, void* data) {
    self->handleTabletToolTip(data);
  }

  void Cursor::onTabletToolButton(Cursor* self, void* data) {
    self->handleTabletToolButton(data);
  }

  void Cursor::onToolDestroy(TabletToolState* watch, void* /*data*/) {
    Cursor* cursor = watch->cursor;
    std::erase_if(cursor->m_tools, [watch](const std::unique_ptr<TabletToolState>& entry) {
      return entry.get() == watch;
    });
  }

  void Cursor::onToolSetCursor(TabletToolState* watch, void* data) {
    auto* event = static_cast<wlr_tablet_v2_event_cursor*>(data);
    if (watch->cursor->compositorOwnsCursor()) {
      return;
    }
    if (watch->v2->focused_surface == nullptr
        || event->seat_client == nullptr
        || wl_resource_get_client(watch->v2->focused_surface->resource) != event->seat_client->client) {
      return;
    }
    watch->cursor->setCursorSurface(event->surface, event->hotspot_x, event->hotspot_y, event->seat_client->client);
  }

  Cursor::TabletToolState* Cursor::toolState(wlr_tablet_tool* tool) {
    for (const auto& entry : m_tools) {
      if (entry->tool == tool) {
        return entry.get();
      }
    }
    auto state = std::make_unique<TabletToolState>();
    state->cursor = this;
    state->tool = tool;
    state->v2 = wlr_tablet_tool_create(m_server->tabletManager(), m_server->seat()->wlr(), tool);
    state->destroy.connect(&tool->events.destroy, state.get(), onToolDestroy);
    state->setCursor.connect(&state->v2->events.set_cursor, state.get(), onToolSetCursor);
    m_tools.push_back(std::move(state));
    return m_tools.back().get();
  }

  void
  Cursor::processTabletMotion(uint32_t timeMsec, double oldX, double oldY, TabletToolState* state, wlr_tablet* tablet) {
    wlr_tablet_v2_tablet* v2tablet = m_server->tabletV2FromWlr(tablet);
    wlr_seat* seat = m_server->seat()->wlr();
    // Emulation wholesale: no tablet-v2 handle for this device, or a compositor state (overview, grab, lock, drag) that
    // must see a plain pointer. A stroke already being emulated stays emulated for its whole tip-down so a mid-stroke
    // bind of tablet-v2 cannot split it.
    const bool emulate = v2tablet == nullptr
        || m_server->overview()->active()
        || !isPassthrough()
        || m_server->sessionLocked()
        || seat->drag != nullptr
        || (state->emulating && state->tipDown);
    if (emulate) {
      setToolEmulating(state, true);
      processMotion(timeMsec, oldX, oldY);
      wlr_seat_pointer_notify_frame(seat);
      return;
    }

    // Native implicit grab: tip or a button is held on the focused surface, so motion belongs to that surface's
    // coordinate space even when the cursor leaves it.
    if (state->v2->focused_surface != nullptr && (state->tipDown || wlr_tablet_tool_v2_has_implicit_grab(state->v2))) {
      double sx = 0;
      double sy = 0;
      wlr_surface* focused = state->v2->focused_surface;
      surfaceLocalCoordinates(m_server->scene(), focused, surfaceScale(focused), m_cursor->x, m_cursor->y, &sx, &sy);
      wlr_tablet_v2_tablet_tool_notify_motion(state->v2, sx, sy);
      forwardEffectPointer();
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (surface == nullptr || !wlr_surface_accepts_tablet_v2(surface, v2tablet)) {
      setToolEmulating(state, true);
      processMotion(timeMsec, oldX, oldY);
      wlr_seat_pointer_notify_frame(seat);
      return;
    }

    // Native hover.
    setToolEmulating(state, false);
    updatePointerOutput();
    if (config().input.focus.followsMouse && layer == nullptr && view != nullptr && view->mapped()) {
      view = hoverFocus(view, &surface, &sx, &sy, &layer, oldX, oldY);
      if (surface == nullptr) {
        setToolEmulating(state, true);
        processMotion(timeMsec, oldX, oldY);
        wlr_seat_pointer_notify_frame(seat);
        return;
      }
    }
    wlr_tablet_v2_tablet_tool_notify_proximity_in(state->v2, v2tablet, surface);
    wlr_tablet_v2_tablet_tool_notify_motion(state->v2, sx, sy);
    forwardEffectPointer();
  }

  void Cursor::processMove() {
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr) {
      resetMode();
      return;
    }
    grab->view->setDragPosition(
        static_cast<int>(m_cursor->x - grab->offsetX), static_cast<int>(m_cursor->y - grab->offsetY),
        grab->floatingLayout
    );
    if (grab->physics) {
      grab->view->moveDragPhysics(m_cursor->x - grab->lastX, m_cursor->y - grab->lastY);
      grab->lastX = m_cursor->x;
      grab->lastY = m_cursor->y;
    }
    presentGrabbedViewSpanning();
  }

  void Cursor::presentGrabbedViewSpanning() {
    View* view = grabbedView();
    if (view == nullptr) {
      return;
    }
    // A window dragged across a monitor boundary must span both outputs, not be
    // clipped to one. Native per-output rendering draws each half.
    view->setNodeEnabled(true);
    view->applyDragPresentation();
  }

  void Cursor::beginDrag(MoveGrab& grab) {
    if (grab.target == DragTarget::Floating && (grab.view->floatingMaximized() || grab.view->snapRegion())) {
      View* view = grab.view;
      const int width = view->floatingSize()[0];
      const double fraction = width > 0 ? std::clamp((grab.startX - view->layoutX()) / width, 0.0, 1.0) : 0.5;
      view->restoreMaximizedForMove();
      view->unsnap(false);
      grab.preDragX = view->layoutTargetX();
      grab.preDragY = view->layoutTargetY();
      const auto restored = view->floatingSize();
      grab.offsetX = fraction * restored[0];
      const int h = view->titleBarHeight();
      if (h > 0)
        grab.offsetY = std::clamp(grab.offsetY, -static_cast<double>(h), -1.0);
      view->setDragPosition(m_cursor->x - grab.offsetX, m_cursor->y - grab.offsetY, grab.floatingLayout);
    }
    m_lastTitleBarClick = {};
    grab.pending = false;
    if (grab.sourceWorkspace != nullptr) {
      grab.sourceWorkspace->layoutDetach(grab.view);
    }
    grab.view->enterDragPresentation();
    grab.physics = grab.view->beginDragPhysics(grab.offsetX, grab.offsetY);
    grab.lastX = m_cursor->x;
    grab.lastY = m_cursor->y;
    updateInteractiveCursor(grab.view);
  }

  void Cursor::updateDropTarget() {
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || grab->pending) {
      return;
    }
    // Only a drop back into the strip has a target to draw and choose.
    if (grab->target != DragTarget::Tiled) {
      return;
    }
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (output == nullptr || output->workspaceGroup() == nullptr || output->workspaceGroup()->active() == nullptr) {
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (layer != nullptr) {
      return;
    }

    Workspace* workspace = output->workspaceGroup()->active();
    grab->drop = computeDropTarget(
        *workspace, m_cursor->x, m_cursor->y, grab->view,
        DropTargetOptions{
            .clipHintToUsable = true,
            .reserveScrollingViewportEdges = true,
            .endpointGapsOutsideColumns = false,
        }
    );
    if (grab->drop.hintBox.width > 0 && grab->drop.hintBox.height > 0) {
      m_server->insertHint().show(output, grab->drop.hintBox, config().appearance.cornerRadius);
    } else {
      m_server->hideInsertHint();
    }
    grab->view->raiseToTop();
  }

  void Cursor::clearSnapPreview() {
    if (m_snapDwellTimer != nullptr) {
      wl_event_source_timer_update(m_snapDwellTimer, 0);
    }
    if (auto* grab = std::get_if<MoveGrab>(&m_grab)) {
      if (grab->snap) {
        m_server->hideInsertHint();
      }
      grab->snap.reset();
      grab->snapOutput = nullptr;
      grab->dwellOutput = nullptr;
      grab->dwellEdges = 0;
      grab->dwellReady = false;
    }
  }

  int Cursor::onSnapDwellTimer(void* data) {
    auto* self = static_cast<Cursor*>(data);
    if (auto* grab = std::get_if<MoveGrab>(&self->m_grab)) {
      grab->dwellReady = true;
      self->updateSnapPreview();
    }
    return 0;
  }

  void Cursor::finishMove() {
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || !grab->view->mapped()) {
      resetMode();
      return;
    }
    View* view = grab->view;
    if (grab->physics) {
      view->endDragPhysics();
    }
    // Where the drag left the window. Read before the state change: becoming
    // floating re-places the window at its remembered origin, immediately when
    // position animations are off.
    const int dropX = view->layoutX();
    const int dropY = view->layoutY();
    // State first, placement second: a window that refused the drag's target
    // (a fullscreen window cannot be pinned) is still tiled here and drops back
    // into the layout rather than staying detached.
    Output* destination =
        m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y));
    Workspace* workspace = destination != nullptr ? destination->workspaceGroup()->active() : nullptr;
    if (grab->target == DragTarget::Tiled && workspace != nullptr && workspace->layoutMode() == LayoutMode::Floating) {
      view->floatForLayout();
    } else {
      applyDragTarget(*grab);
    }
    if (view->tiled()) {
      finishTileMove();
    } else {
      finishFloatMove(dropX, dropY);
    }
  }

  void Cursor::applyDragTarget(const MoveGrab& grab) {
    View* view = grab.view;
    switch (grab.target) {
    case DragTarget::Tiled:
      if (!view->tiled()) {
        // finishTileMove inserts it at the drop target, so skip the layout
        // placement setFloating would do on its own.
        view->setFloating(false, false, View::TilePlacement::Detached);
      }
      break;
    case DragTarget::Floating:
      if (view->pinned()) {
        view->setPinned(false, false);
      }
      if (view->tiled()) {
        view->setFloating(true, false);
      }
      break;
    case DragTarget::Pinned:
      // Pinning a tiled window floats it and remembers to re-tile on unpin.
      view->setPinned(true, false);
      break;
    }
  }

  void Cursor::finishTileMove() {
    m_server->hideInsertHint();
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr) {
      resetMode();
      return;
    }
    View* view = grab->view;
    Workspace* target = grab->drop.workspace != nullptr ? grab->drop.workspace : grab->sourceWorkspace;
    if (target == nullptr && view != nullptr) {
      target = view->workspace();
    }
    if (view != nullptr && view->mapped() && target != nullptr) {
      applyDrop(
          *m_server, *view, *target, grab->drop, grab->sourceWidth.has_value() ? &*grab->sourceWidth : nullptr,
          /*animate=*/true
      );
    }
    resetMode();
  }

  void Cursor::toggleDragTarget(uint32_t button) {
    // The drag owns its initiating button, so the other main button is the one
    // free to retarget it.
    const uint32_t toggleButton = m_grabButton == BTN_LEFT ? BTN_RIGHT : BTN_LEFT;
    if (button != toggleButton) {
      return;
    }
    const WindowDragToggle toggle = config().input.windowDragToggle;
    if (toggle == WindowDragToggle::None) {
      return;
    }
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || !grab->view->mapped()) {
      return;
    }
    // A scratchpad window has no layout to be dropped into.
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(grab->view)) {
      return;
    }
    if (grab->pending) {
      beginDrag(*grab);
    }

    clearSnapPreview();
    if (toggle == WindowDragToggle::Pinned) {
      grab->target = grab->target == DragTarget::Pinned ? grab->unpinned : DragTarget::Pinned;
    } else {
      grab->target = grab->target == DragTarget::Tiled ? DragTarget::Floating : DragTarget::Tiled;
    }

    if (grab->target == DragTarget::Tiled) {
      processMove();
      updateDropTarget();
      return;
    }
    m_server->hideInsertHint();
    retargetDragSize(*grab);
  }

  void Cursor::retargetDragSize(MoveGrab& grab) {
    View* view = grab.view;
    const auto [width, height] = view->floatingRestoreSize();
    if (width <= 0 || height <= 0) {
      processMove();
      return;
    }
    // Keep the pointer's grip on the window: it stays over the same fraction of
    // the window it grabbed, whatever the drop resizes it to.
    const int presentedWidth = view->presentation().width();
    const int presentedHeight = view->presentation().height();
    if (presentedWidth > 0 && presentedHeight > 0) {
      grab.offsetX *= static_cast<double>(width) / presentedWidth;
      grab.offsetY *= static_cast<double>(height) / presentedHeight;
    }
    view->requestFloatingSize(width, height);
    view->beginResizeAnimation(width, height);
    if (grab.physics) {
      view->setDragPhysicsGrab(grab.offsetX, grab.offsetY);
    }
    processMove();
  }

  void Cursor::revealClickedTile(View* view) {
    Workspace* workspace = view->workspace();
    if (view->tiled() && workspace != nullptr && workspace->focusedView() == view) {
      workspace->ensureFocusedVisible();
      workspace->markArrange(true);
    }
  }

  uint32_t Cursor::tiledResizeEdges(View* view, double x, double y) const {
    Workspace* workspace = view->workspace();
    if (workspace == nullptr || workspace->group() == nullptr || workspace->group()->output() == nullptr) {
      return 0;
    }
    const wlr_box presented = workspace->presentedTiledBox(view);
    const wlr_box visible = workspace->usableArea();
    wlr_box reachable{};
    if (!wlr_box_intersection(&reachable, &presented, &visible)) {
      return 0;
    }
    return workspace->layout().sanitizeResizeEdges(view, resizeEdgesForPoint(reachable, x, y));
  }

  uint32_t Cursor::floatResizeEdges(View* view) const {
    if (view == nullptr || view->sceneTree() == nullptr) {
      return WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
    }
    const wlr_box& geo = view->geometryBox();
    const int x = view->layoutX() + geo.x;
    const int y = view->layoutY() + geo.y;
    const wlr_box box{.x = x, .y = y, .width = geo.width, .height = geo.height};
    return resizeEdgesForPoint(box, m_cursor->x, m_cursor->y);
  }

  void Cursor::setCompositorCursor(const char* name) {
    if (name == nullptr) {
      if (m_compositorOwnsCursor) {
        restoreClientCursor();
      }
      return;
    }
    if (m_compositorOwnsCursor && m_compositorCursorName == name) {
      return;
    }
    m_compositorOwnsCursor = true;
    m_compositorCursorName = name;
    setXcursor(name);
  }

  void Cursor::restoreClientCursor() {
    m_compositorOwnsCursor = false;
    m_compositorCursorName.clear();
    applyClientCursor();
    refreshPointerFocus();
  }

  bool Cursor::pointerFocusPinned() const {
    const wlr_seat* seat = m_server->seat()->wlr();
    // A client drag owns the seat grab and moves its own focus, so it is not an
    // implicit grab.
    return seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr;
  }

  bool Cursor::pointerContentsStale(const wlr_surface* surface, double sx, double sy) const {
    const wlr_seat* seat = m_server->seat()->wlr();
    if (surface != seat->pointer_state.focused_surface) {
      return true;
    }
    // Compare what the client receives, as wlroots does before sending motion.
    return surface != nullptr
        && (wl_fixed_from_double(sx) != wl_fixed_from_double(seat->pointer_state.sx)
            || wl_fixed_from_double(sy) != wl_fixed_from_double(seat->pointer_state.sy));
  }

  void Cursor::setPointerFocus(wlr_surface* surface, double sx, double sy, uint32_t timeMsec) {
    if (surface == nullptr) {
      clearPointerFocus();
      return;
    }
    if (pointerFocusPinned()) {
      return;
    }
    wlr_seat* seat = m_server->seat()->wlr();
    // Drop placement can change the scene before wlroots releases its grab.
    // Retargeting then would invalidate the offer the recipient must finish.
    if (seat->drag != nullptr && seat->drag->dropped) {
      return;
    }
    if (surface == seat->pointer_state.focused_surface) {
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
    } else {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    }
  }

  void Cursor::clearPointerFocus() {
    if (pointerFocusPinned()) {
      return;
    }
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
  }

  void Cursor::clearPointerFocusOverridingGrab() { wlr_seat_pointer_clear_focus(m_server->seat()->wlr()); }

  void Cursor::refreshPointerFocus() {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr && (capture->capabilities() & 2U) != 0) {
      return;
    }
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (const Overview* overview = m_server->overview(); overview != nullptr
        && overview->active()
        && !m_server->sessionLocked()
        && (overview->dragging() || !overviewPassthroughLayer(layer))) {
      surface = nullptr;
    }
    const bool stale = !pointerFocusPinned() && pointerContentsStale(surface, sx, sy);
    setPointerFocus(surface, sx, sy, monotonicMsec());
    if (stale) {
      wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
    }
  }

  void Cursor::refreshPointerContents(const Output* output) {
    wlr_seat* seat = m_server->seat()->wlr();
    if (output == nullptr
        || m_cursorHidden
        || std::ranges::any_of(m_tools, [](const auto& tool) { return tool->inProximity; })
        || wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y) != output->wlr()) {
      return;
    }
    if (seat->drag != nullptr) {
      updateDataDragEdgeScroll();
      if (seat->drag->dropped) {
        return;
      }
      if (m_server->sessionLocked()) {
        wlr_seat_pointer_notify_clear_focus(seat);
        wlr_seat_pointer_notify_frame(seat);
        return;
      }
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
      if (surface != nullptr) {
        const uint32_t timeMsec = monotonicMsec();
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
      } else {
        wlr_seat_pointer_notify_clear_focus(seat);
      }
      wlr_seat_pointer_notify_frame(seat);
      return;
    }
    if (tiledMoveDragActive()) {
      // The compositor owns drop targeting during a window move; client pointer focus stays suspended.
      updateDataDragEdgeScroll();
      updateDropTarget();
      return;
    }
    if (!isPassthrough() || seat->pointer_state.button_count != 0) {
      return;
    }
    // Content still in motion would flicker hover state on every frame. The next press resolves it regardless.
    if (!m_server->sessionLocked()
        && ((m_server->overview() != nullptr && m_server->overview()->active())
            || m_server->animationsActiveFor(output)
            || (m_server->gestures() != nullptr && m_server->gestures()->movingContent()))) {
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (!pointerContentsStale(surface, sx, sy)) {
      return;
    }
    if (surface != nullptr) {
      setPointerFocus(surface, sx, sy, monotonicMsec());
    } else {
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      clearPointerFocus();
    }
    wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
    updateConstraintForSurface(surface);
    updateInteractiveCursor(view);
  }

  void Cursor::updateInteractiveCursor(View* under) {
    if (m_server->sessionLocked()) {
      setCompositorCursor(nullptr);
      return;
    }

    uint32_t resizeEdges = 0;
    if (const auto* grab = std::get_if<FloatingResizeGrab>(&m_grab)) {
      resizeEdges = grab->edges;
    } else if (const auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
      resizeEdges = grab->edges;
    } else if (const auto* grab = std::get_if<PendingResizeGrab>(&m_grab)) {
      resizeEdges = grab->edges;
    }
    if (resizeEdges != 0) {
      const char* name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(resizeEdges));
      setCompositorCursor(name != nullptr ? name : "default");
      return;
    }
    if (const auto* grab = std::get_if<MoveGrab>(&m_grab)) {
      if (!grab->pending) {
        setCompositorCursor("grabbing");
      }
      return;
    }

    const bool modHeld = (m_server->keyboardModifiers() & m_server->modKey()) != 0;
    if (modHeld && under != nullptr && under->mapped()) {
      const uint32_t edges = hoverResizeEdges(under);
      if (edges != 0) {
        const char* name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(edges));
        setCompositorCursor(name != nullptr ? name : "default");
        return;
      }
      setCompositorCursor("grab");
      return;
    }

    if (!modHeld) {
      if (const auto hit = m_server->ssdFrameAt(m_cursor->x, m_cursor->y)) {
        const char* name = "default";
        if (hit->part == SsdFramePart::Resize) {
          name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(hit->edges));
        } else if (
            !hit->view->blocked()
            && (hit->part == SsdFramePart::Close
                || hit->part == SsdFramePart::Minimize
                || (hit->part == SsdFramePart::Maximize && hit->view->ssdFrame()->maximizeEnabled()))
        ) {
          name = "pointer";
        }
        setCompositorCursor(name != nullptr ? name : "default");
        return;
      }
    }
    setCompositorCursor(nullptr);
  }

  void Cursor::refreshInteractiveCursor() {
    if (!isPassthrough()) {
      updateInteractiveCursor(grabbedView());
      return;
    }
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    updateInteractiveCursor(view);
  }

  void Cursor::processResizeTile() {
    auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    if (grab == nullptr
        || grab->workspace == nullptr
        || grab->workspace->group() == nullptr
        || grab->workspace->group()->output() == nullptr
        || grab->view == nullptr
        || grab->session == nullptr) {
      resetMode();
      return;
    }
    if (grab->session->ownerLayout() != &grab->workspace->layout()) {
      resetMode();
      return;
    }
    const wlr_box usable = grab->workspace->tiledArea();
    grab->session->applyDelta(m_cursor->x - grab->startX, m_cursor->y - grab->startY, usable);
    grab->view->setMaximizedState(false);
    grab->workspace->markArrange(false);
  }

} // namespace andromeda

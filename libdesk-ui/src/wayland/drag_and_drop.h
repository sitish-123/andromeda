#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class WaylandConnection;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_data_source;
struct wl_surface;

// Drag and drop with other clients over wl_data_device (v3): this client as the source of a drag, and its surfaces as
// drop targets. One instance per connection (WaylandConnection::dragAndDrop).
class DragAndDrop {
public:
  enum Action : std::uint32_t { None = 0, Copy = 1, Move = 2, Ask = 4 };
  // wl_data_device has no link action: a link drop is Ask, and the target resolves it from the modifiers it sees.

  struct Drag {
    std::vector<std::string> mimeTypes;
    std::function<std::string(const std::string& mime)> data; // bytes for one offered type
    std::uint32_t actions = Copy | Move | Ask;
    wl_surface* icon = nullptr; // the drag image, or none
    // The action the target took and whether it dropped; called once.
    std::function<void(std::uint32_t action, bool dropped)> finished;
  };

  struct Target {
    // Pointer entered with these types; return false to refuse every drop on this surface.
    std::function<bool(const std::vector<std::string>& mimes)> enter;
    // Surface-local position; returns the action a drop here would take (None = not here).
    std::function<std::uint32_t(float x, float y)> motion;
    std::function<void()> leave;
    // The dropped data of the first accepted type, read off the main thread and delivered on it.
    std::function<void(float x, float y, std::uint32_t action, const std::string& mime, std::string data)> drop;
  };

  [[nodiscard]] static std::unique_ptr<DragAndDrop> create(WaylandConnection& connection,
                                                           wl_data_device_manager* manager);
  ~DragAndDrop();

  // Starts a drag from a pointer press on `origin` (its serial). False when the compositor has no data device.
  bool startDrag(wl_surface* origin, std::uint32_t serial, Drag drag);
  [[nodiscard]] bool dragging() const noexcept { return m_source != nullptr; }
  void setTarget(wl_surface* surface, Target target);
  void clearTarget(wl_surface* surface);
  // The types the target accepts, best first.
  void setAcceptedTypes(std::vector<std::string> types) { m_accepted = std::move(types); }

  // The clipboard (wl_data_device selection). `serial` comes from the input event that asked for it.
  struct Selection {
    std::vector<std::string> mimeTypes;
    std::function<std::string(const std::string& mime)> data;
  };
  bool setSelection(std::uint32_t serial, Selection selection);
  // True while this client owns the clipboard.
  [[nodiscard]] bool ownsSelection() const noexcept { return m_selectionSource != nullptr; }
  // The types the current clipboard offers (empty when it is empty or ours).
  [[nodiscard]] std::vector<std::string> selectionTypes() const;
  // Reads the clipboard as the first of `accepted` it offers, off the main thread; `done` runs on the main loop with
  // the type and bytes ("" and "" when nothing matched or the source did not answer).
  void readSelection(const std::vector<std::string>& accepted,
                     std::function<void(const std::string& mime, std::string bytes)> done,
                     std::size_t maxBytes = static_cast<std::size_t>(-1));
  // Runs after each clipboard change, ours included (the compositor sends it while a surface has keyboard focus).
  void setSelectionChanged(std::function<void()> changed) { m_selectionChanged = std::move(changed); }

  // wl listener entry points.
  void onSelectionSend(const char* mime, int fd);
  void onSelectionCancelled();
  void onSourceSend(const char* mime, int fd);
  void onSourceAction(std::uint32_t action) { m_sourceAction = action; }
  void onOfferAction(wl_data_offer* offer, std::uint32_t action) {
    if (offer == m_offer) {
      m_offerAction = action; // v3 dnd_action: what the compositor chose from both sides
    }
  }
  void onSourceDone(bool dropped);
  void onDataOffer(wl_data_offer* offer);
  void onOfferMime(wl_data_offer* offer, const char* mime);
  void onEnter(std::uint32_t serial, wl_surface* surface, float x, float y, wl_data_offer* offer);
  void onLeave();
  void onMotion(float x, float y);
  void onDrop();
  void onSelection(wl_data_offer* offer);

private:
  explicit DragAndDrop(WaylandConnection& connection, wl_data_device_manager* manager);
  void updateAction(float x, float y);
  void releaseOffer(wl_data_offer* offer);

  WaylandConnection& m_connection;
  wl_data_device_manager* m_manager;
  wl_data_device* m_device = nullptr;

  wl_data_source* m_source = nullptr;
  Drag m_drag;
  std::uint32_t m_sourceAction = None;

  std::map<wl_data_offer*, std::vector<std::string>> m_offers;
  wl_data_offer* m_offer = nullptr; // the offer of the drag over us
  wl_surface* m_over = nullptr;
  std::uint32_t m_enterSerial = 0;
  std::uint32_t m_action = None;
  std::uint32_t m_offerAction = None;
  float m_x = 0.0F;
  float m_y = 0.0F;
  std::string m_mime;
  std::vector<std::string> m_accepted{"text/uri-list"};
  std::map<wl_surface*, Target> m_targets;
  wl_data_source* m_selectionSource = nullptr;
  Selection m_selection;
  wl_data_offer* m_selectionOffer = nullptr; // the clipboard of another client
  std::function<void()> m_selectionChanged;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

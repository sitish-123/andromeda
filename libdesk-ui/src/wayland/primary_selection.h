#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class WaylandConnection;
struct zwp_primary_selection_device_manager_v1;
struct zwp_primary_selection_device_v1;
struct zwp_primary_selection_offer_v1;
struct zwp_primary_selection_source_v1;

// The primary selection (select to copy, middle click to paste) over zwp_primary_selection_v1, text only. Paste is
// synchronous in Input, so the text of each new selection is read ahead, off the main thread.
class PrimarySelection {
public:
  PrimarySelection(WaylandConnection& connection, zwp_primary_selection_device_manager_v1* manager);
  ~PrimarySelection();
  PrimarySelection(const PrimarySelection&) = delete;
  PrimarySelection& operator=(const PrimarySelection&) = delete;

  [[nodiscard]] std::optional<std::string> text() const { return m_text; }
  // The text once known: at once, or when the read of a selection that just arrived ends.
  void whenText(std::function<void(std::optional<std::string>)> done);
  void setText(std::uint32_t serial, std::string text);

  void onDataOffer(zwp_primary_selection_offer_v1* offer);
  void onOfferMime(zwp_primary_selection_offer_v1* offer, const char* mime);
  void onSelection(zwp_primary_selection_offer_v1* offer);
  void onSend(const char* mime, int fd);
  void onCancelled();

private:
  void finishRead();

  WaylandConnection& m_connection;
  zwp_primary_selection_device_manager_v1* m_manager;
  zwp_primary_selection_device_v1* m_device = nullptr;
  zwp_primary_selection_source_v1* m_source = nullptr;
  std::string m_ownText;
  zwp_primary_selection_offer_v1* m_pending = nullptr; // announced, its types arriving
  std::vector<std::string> m_pendingMimes;
  zwp_primary_selection_offer_v1* m_offer = nullptr; // the current selection of another client
  std::optional<std::string> m_text;
  unsigned m_generation = 0;
  bool m_reading = false;
  std::vector<std::function<void(std::optional<std::string>)>> m_waiters;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

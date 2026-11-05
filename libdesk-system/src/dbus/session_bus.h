#pragma once

#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>

class SessionBus {
public:
  SessionBus();

  [[nodiscard]] sdbus::IConnection& connection() noexcept { return *m_connection; }
  [[nodiscard]] sdbus::IConnection::PollData getPollData() const;
  [[nodiscard]] bool hasPendingEvents() const noexcept { return m_hasPendingEvents; }
  void processPendingEvents();

  // False once the bus went away (the session is ending): nothing is polled or dispatched after that.
  [[nodiscard]] bool connected() const noexcept { return m_connected; }
  // Runs once when the bus goes away; an app's loop typically quits.
  void setDisconnectedCallback(std::function<void()> callback) { m_onDisconnected = std::move(callback); }

private:
  void dispatchBatch();

  std::unique_ptr<sdbus::IConnection> m_connection;
  std::function<void()> m_onDisconnected;
  bool m_hasPendingEvents = false;
  bool m_connected = true;
};

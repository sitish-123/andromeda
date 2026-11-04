#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

// User-facing notices from libdesk-system services (a failed Bluetooth connect, a Wi-Fi auth error). The host
// routes them to its notification UI; without a sink they are dropped.
namespace service_notice {

  enum class Level : std::uint8_t { Info, Error };
  using Sink = std::function<void(Level level, std::string app, std::string title, std::string body)>;

  void setSink(Sink sink);
  void post(Level level, std::string app, std::string title, std::string body);

  inline void info(std::string app, std::string title, std::string body) {
    post(Level::Info, std::move(app), std::move(title), std::move(body));
  }
  inline void error(std::string app, std::string title, std::string body) {
    post(Level::Error, std::move(app), std::move(title), std::move(body));
  }

} // namespace service_notice

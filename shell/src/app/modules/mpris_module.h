#pragma once

#include "app/service_host.h"
#include "dbus/mpris/mpris_service.h"

#include <memory>
#include <string>

class SessionBus;

// MPRIS player discovery on the session bus. The service is null when it fails to start.
class MprisModule final : public ServiceModule {
public:
  explicit MprisModule(SessionBus& bus);
  ~MprisModule() override;

  [[nodiscard]] std::string_view name() const override { return "mpris"; }
  [[nodiscard]] bool reads(const ConfigChangeSet& change) const override;
  void start(const Config& config) override;
  void stop() override;
  void reload(const Config& config) override;

  [[nodiscard]] MprisService* service() const noexcept { return m_service.get(); }
  // Reason the service failed to start; empty otherwise.
  [[nodiscard]] const std::string& startError() const noexcept { return m_startError; }

private:
  SessionBus& m_bus;
  std::unique_ptr<MprisService> m_service;
  std::string m_startError;
};

#pragma once

#include "app/service_host.h"
#include "pipewire/pipewire_service.h"

#include <memory>
#include <string>

// PipeWire graph, devices and streams. The service is null when it fails to start.
class PipeWireModule final : public ServiceModule {
public:
  [[nodiscard]] std::string_view name() const override { return "pipewire"; }
  void start(const Config& config) override;
  void stop() override;

  [[nodiscard]] PipeWireService* service() const noexcept { return m_service.get(); }
  // Reason the service failed to start; empty otherwise.
  [[nodiscard]] const std::string& startError() const noexcept { return m_startError; }

private:
  std::unique_ptr<PipeWireService> m_service;
  std::string m_startError;
};

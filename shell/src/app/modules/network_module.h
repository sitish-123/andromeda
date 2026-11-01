#pragma once

#include "app/service_host.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/network/network_secret_agent.h"

#include <functional>
#include <memory>

class SystemBus;

// NetworkManager, else wpa_supplicant, else iwd. The service is null when none is available.
class NetworkModule final : public ServiceModule {
public:
  struct Hooks {
    // Runs once the service exists and its change callback is set.
    std::function<void(INetworkService&)> started;
    std::function<void(const NetworkState&, NetworkChangeOrigin)> changed;
  };

  NetworkModule(SystemBus& bus, Hooks hooks);

  [[nodiscard]] std::string_view name() const override { return "network"; }
  void start(const Config& config) override;
  void stop() override;

  [[nodiscard]] INetworkService* service() const noexcept { return m_service.get(); }
  [[nodiscard]] NetworkSecretAgent* secretAgent() const noexcept { return m_secretAgent.get(); }

private:
  SystemBus& m_bus;
  Hooks m_hooks;
  std::unique_ptr<INetworkService> m_service;
  std::unique_ptr<NetworkSecretAgent> m_secretAgent;
  std::unique_ptr<IwdSecretAgent> m_iwdSecretAgent;
};

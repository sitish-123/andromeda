#pragma once

#include <string>

// Inert: nothing creates this service, so the bar network widget's pointer stays null. Delete it with the bar's
// WAN IP row.
class ExternalIpService {
public:
  [[nodiscard]] const std::string& externalIp() const noexcept { return m_externalIp; }

private:
  std::string m_externalIp;
};

#include "ipc/service_ipc.h"

#include "dbus/power/power_profiles_service.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string>

void registerPowerProfilesIpc(
    IpcService& ipc, PowerProfilesService& service, const std::function<void(std::string_view)>& stateFeedback
) {
  ipc.bind(noctalia::cli::msg::powerSet, [&service, stateFeedback](const std::string& args) -> std::string {
    const std::string profile = StringUtils::trim(args);
    if (profile.empty()) {
      return "error: profile required (power-set <profile>); typical values: performance, balanced, "
             "power-saver\n";
    }
    const auto& available = service.profiles();
    if (!available.empty() && !std::ranges::contains(available, profile)) {
      std::string suffix = "; available:";
      for (const auto& availableProfile : available) {
        suffix.push_back(' ');
        suffix += availableProfile;
      }
      suffix.push_back('\n');
      return "error: unknown profile \"" + profile + "\"" + suffix;
    }

    const std::string previous = service.activeProfile();
    if (!service.setActiveProfile(profile)) {
      return "error: failed to set power profile\n";
    }
    if (stateFeedback && previous != service.activeProfile() && !service.activeProfile().empty()) {
      stateFeedback(service.activeProfile());
    }
    return "ok\n";
  });
  ipc.bindCycle(noctalia::cli::msg::powerCycle, [&service, stateFeedback](const std::string& args) -> std::string {
    const std::string direction = StringUtils::trim(args);
    int step = 1;
    if (direction == "prev") {
      step = -1;
    } else if (!direction.empty() && direction != "next") {
      return "error: power-cycle takes next or prev\n";
    }
    const std::string previous = service.activeProfile();
    if (!service.cycleActiveProfile(step)) {
      if (service.profiles().empty() && service.activeProfile().empty()) {
        return "error: could not cycle power profile (UPower profile list not loaded)\n";
      }
      return "error: could not cycle power profile (set failed)\n";
    }
    if (stateFeedback && previous != service.activeProfile() && !service.activeProfile().empty()) {
      stateFeedback(service.activeProfile());
    }
    return "ok\n";
  });
}

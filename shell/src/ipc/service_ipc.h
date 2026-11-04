#pragma once

#include <functional>
#include <string_view>

class IpcService;
class PowerProfilesService;

// IPC commands for libdesk-system services: the services hold no shell IPC code.
void registerPowerProfilesIpc(
    IpcService& ipc, PowerProfilesService& service, const std::function<void(std::string_view)>& stateFeedback = {}
);

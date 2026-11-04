#pragma once

#include <functional>
#include <string_view>

class BluetoothService;
class INetworkService;
class IpcService;
class MprisService;
class PowerProfilesService;

// IPC commands for libdesk-system services: the services hold no shell IPC code.
void registerPowerProfilesIpc(
    IpcService& ipc, PowerProfilesService& service, const std::function<void(std::string_view)>& stateFeedback = {}
);
void registerBluetoothIpc(
    IpcService& ipc, BluetoothService& service, const std::function<void(bool)>& stateFeedback = {}
);
void registerNetworkIpc(
    IpcService& ipc, INetworkService& service, const std::function<void(bool)>& wirelessFeedback = {}
);
void registerMprisIpc(IpcService& ipc, MprisService& service);

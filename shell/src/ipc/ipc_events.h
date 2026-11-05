#pragma once

#include <nlohmann/json_fwd.hpp>

class IpcService;
class MprisService;
struct BluetoothState;
struct NetworkState;
struct PowerProfilesState;

// `msg subscribe` topics (cli::msg::kSubscribeTopics) and the typed data that `msg --json <topic>-status` replies with.
// Services stay free of IPC code: their shell modules call these on each change.
namespace ipc_events {

  // The instance that receives events; null drops them (tests, IPC off).
  void setSink(IpcService* ipc);

  [[nodiscard]] nlohmann::json toJson(const BluetoothState& state);
  [[nodiscard]] nlohmann::json toJson(const NetworkState& state);
  [[nodiscard]] nlohmann::json toJson(const PowerProfilesState& state);
  [[nodiscard]] nlohmann::json mediaJson(const MprisService& service);

  // Each sends only when the data differs from the last event of its topic.
  void bluetooth(const BluetoothState& state);
  void network(const NetworkState& state);
  void powerProfile(const PowerProfilesState& state);
  void media(const MprisService& service);

} // namespace ipc_events

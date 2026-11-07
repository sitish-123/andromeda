#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class IpcService;

enum class AudioEffectsProfileKind : std::uint8_t {
  Output,
  Input,
};

class EasyEffectsService {
public:
  using ChangeCallback = std::function<void()>;
  using EffectsProfileFeedbackCallback = std::function<void(AudioEffectsProfileKind kind, std::string_view profile)>;

  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }
  [[nodiscard]] const std::vector<std::string>& effectsProfiles(AudioEffectsProfileKind kind) const;
  [[nodiscard]] std::string activeEffectsProfile(AudioEffectsProfileKind kind) const;
  void refreshProfiles();
  void refreshActiveEffectsProfiles();
  // Both refreshes on the SharedWorker: the server query blocks up to 250 ms per kind.
  void refreshAsync();
  bool loadEffectsProfile(AudioEffectsProfileKind kind, std::string_view profile);
  void registerIpc(
      IpcService& ipc, const ConfigService& config, const EffectsProfileFeedbackCallback& effectsProfileFeedback = {}
  );

private:
  void emitChanged();

  std::vector<std::string> m_outputEffectsProfiles;
  std::vector<std::string> m_inputEffectsProfiles;
  std::optional<std::string> m_activeOutputEffectsProfile;
  std::optional<std::string> m_activeInputEffectsProfile;
  ChangeCallback m_changeCallback;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

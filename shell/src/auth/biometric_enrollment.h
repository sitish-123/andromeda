#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Face (gazed) and fingerprint (fprintd) enrollment for the current user: what is enrolled, enroll, rename, remove.
// Shared by the setup wizard and Settings › Privacy and Security. All calls are async; nothing blocks the caller.
class BiometricEnrollment {
public:
  enum class Method : std::uint8_t { None, Face, Finger };
  // Removal and rename result: ok, or the daemon's error text.
  using DoneCallback = std::function<void(bool ok)>;

  BiometricEnrollment(SystemBus* bus, std::string user);
  ~BiometricEnrollment();
  BiometricEnrollment(const BiometricEnrollment&) = delete;
  BiometricEnrollment& operator=(const BiometricEnrollment&) = delete;

  // Runs after each change of presence, lists or enroll progress.
  void setChangedCallback(std::function<void()> callback) { m_onChanged = std::move(callback); }
  // Runs once when an enrollment ends; ok = the new face or finger is stored.
  void setFinishedCallback(std::function<void(Method, bool ok)> callback) { m_onFinished = std::move(callback); }

  void probe();
  // Face: `name` is the stored face name. Finger: `name` is an fprintd finger name; empty = the first free finger.
  void start(Method method, const std::string& name = {});
  void stop();

  void removeFace(const std::string& name, DoneCallback done);
  void renameFace(const std::string& from, const std::string& to, DoneCallback done);
  void removeFinger(const std::string& finger, DoneCallback done);

  [[nodiscard]] bool facePresent() const noexcept { return m_facePresent; }
  [[nodiscard]] bool fingerPresent() const noexcept { return m_fingerPresent; }
  [[nodiscard]] const std::vector<std::string>& faces() const noexcept { return m_faces; }
  [[nodiscard]] const std::vector<std::string>& fingers() const noexcept { return m_fingers; }
  [[nodiscard]] Method enrolling() const noexcept { return m_enrolling; }
  // 0..1 progress of the running enrollment and its current prompt (translated).
  [[nodiscard]] float progress() const noexcept { return m_progress; }
  [[nodiscard]] const std::string& prompt() const noexcept { return m_prompt; }

  // A face name not in `faces()`: "Face", then "Face 2", "Face 3", ...
  [[nodiscard]] std::string nextFaceName() const;
  // The translated name of an fprintd finger ("right-index-finger" -> "Right index finger").
  [[nodiscard]] static std::string fingerLabel(const std::string& finger);

private:
  void probeFace();
  void probeFinger();
  void listFingers();
  void finish(bool ok);
  void releaseFace();
  void releaseFinger();
  void changed();

  SystemBus* m_bus = nullptr;
  std::string m_user;
  std::function<void()> m_onChanged;
  std::function<void(Method, bool)> m_onFinished;

  std::unique_ptr<sdbus::IProxy> m_gaze;
  std::unique_ptr<sdbus::IProxy> m_fprintDevice;
  std::string m_fingerDevice;
  bool m_facePresent = false;
  bool m_fingerPresent = false;
  std::vector<std::string> m_faces;
  std::vector<std::string> m_fingers;

  Method m_enrolling = Method::None;
  float m_progress = 0.0F;
  std::string m_prompt;
  int m_fingerStages = 0;
  int m_fingerPassed = 0;
  bool m_faceClaimed = false;
  bool m_fingerClaimed = false;
  // A reply can arrive after this object is gone.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

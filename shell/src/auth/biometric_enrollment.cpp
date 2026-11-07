#include "auth/biometric_enrollment.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <format>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

namespace {

  constexpr Logger kLog("enrollment");

  const sdbus::ServiceName kGazeBusName{"com.gundulabs.Gaze"};
  const sdbus::ObjectPath kGazePath{"/com/gundulabs/Gaze"};
  constexpr auto kGazeInterface = "com.gundulabs.Gaze";

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kFprintManagerPath{"/net/reactivated/Fprint/Manager"};
  constexpr auto kFprintManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kFprintDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr const char* kFingers[] = {
      "right-index-finger", "left-index-finger", "right-middle-finger", "left-middle-finger", "right-thumb",
      "left-thumb",         "right-ring-finger", "left-ring-finger",    "right-little-finger", "left-little-finger",
  };

  // name, template count, has RGB, has IR.
  using FaceEntry = sdbus::Struct<std::string, std::uint32_t, bool, bool>;

  template <typename Call> bool tryCall(const char* what, Call&& call) {
    try {
      call();
      return true;
    } catch (const sdbus::Error& e) {
      kLog.info("could not call {}: {}", what, e.what());
      return false;
    }
  }

  std::string facePrompt(const std::string& message) {
    if (message == "Tilt your face slightly up") {
      return i18n::tr("setup-wizard.face-up");
    }
    if (message == "Tilt your face slightly down") {
      return i18n::tr("setup-wizard.face-down");
    }
    if (message == "Turn your face slightly left") {
      return i18n::tr("setup-wizard.face-left");
    }
    if (message == "Turn your face slightly right") {
      return i18n::tr("setup-wizard.face-right");
    }
    if (message == "Face the camera") {
      return i18n::tr("setup-wizard.face-straight");
    }
    return {};
  }

  void ignoreReply(std::optional<sdbus::Error> /*error*/) {}

} // namespace

BiometricEnrollment::BiometricEnrollment(SystemBus* bus, std::string user) : m_bus(bus), m_user(std::move(user)) {}

BiometricEnrollment::~BiometricEnrollment() {
  *m_alive = false;
  stop();
}

void BiometricEnrollment::changed() {
  if (m_onChanged) {
    m_onChanged();
  }
}

void BiometricEnrollment::probe() {
  if (m_bus == nullptr || m_enrolling != Method::None) {
    return;
  }
  probeFace();
  probeFinger();
}

void BiometricEnrollment::probeFace() {
  if (m_gaze == nullptr) {
    try {
      m_gaze = sdbus::createProxy(m_bus->connection(), kGazeBusName, kGazePath);
      m_gaze->uponSignal("EnrollStatus")
          .onInterface(kGazeInterface)
          .call([this](const std::string& /*name*/, std::uint32_t progress, std::uint32_t max, bool done,
                       const std::string& message, double /*remaining*/) {
            if (m_enrolling != Method::Face ||
                !m_bus->isNameOwner(kGazeBusName, m_gaze->getCurrentlyProcessedMessage().getSender())) {
              return;
            }
            m_progress = max > 0 ? static_cast<float>(progress) / static_cast<float>(max) : 0.0F;
            if (const std::string prompt = facePrompt(message); !prompt.empty()) {
              m_prompt = prompt;
            }
            if (done) {
              finish(message == "Completed");
            } else {
              changed();
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("no gazed proxy: {}", e.what());
      m_gaze.reset();
      return;
    }
  }
  tryCall("gazed IsCameraAvailable", [&] {
    m_gaze->callMethodAsync("IsCameraAvailable")
        .onInterface(kGazeInterface)
        .uponReplyInvoke([this](std::optional<sdbus::Error> e, bool available) {
          m_facePresent = !e.has_value() && available;
          if (!m_facePresent) {
            m_faces.clear();
            changed();
            return;
          }
          tryCall("gazed ListFaces", [&] {
            m_gaze->callMethodAsync("ListFaces")
                .onInterface(kGazeInterface)
                .withArguments(m_user)
                .uponReplyInvoke([this](std::optional<sdbus::Error> e, const std::vector<FaceEntry>& faces) {
                  m_faces.clear();
                  if (!e.has_value()) {
                    for (const auto& face : faces) {
                      m_faces.push_back(face.get<0>());
                    }
                    std::ranges::sort(m_faces);
                  }
                  changed();
                });
          });
        });
  });
}

void BiometricEnrollment::probeFinger() {
  tryCall("fprintd GetDefaultDevice", [&] {
    auto manager = std::shared_ptr<sdbus::IProxy>(
        sdbus::createProxy(m_bus->connection(), kFprintBusName, kFprintManagerPath)
    );
    std::weak_ptr<bool> alive = m_alive;
    manager->callMethodAsync("GetDefaultDevice")
        .onInterface(kFprintManagerInterface)
        .uponReplyInvoke([this, manager, alive](std::optional<sdbus::Error> e, const sdbus::ObjectPath& path) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            m_fingerPresent = false;
            m_fingers.clear();
            changed();
            return;
          }
          if (m_fprintDevice == nullptr || m_fingerDevice != path) {
            try {
              m_fprintDevice = sdbus::createProxy(m_bus->connection(), kFprintBusName, path);
            } catch (const sdbus::Error& error) {
              kLog.info("no fprintd device proxy: {}", error.what());
              m_fprintDevice.reset();
              return;
            }
            m_fingerDevice = path;
            m_fprintDevice->uponSignal("EnrollStatus")
                .onInterface(kFprintDeviceInterface)
                .call([this](const std::string& result, bool done) {
                  if (m_enrolling != Method::Finger) {
                    return;
                  }
                  if (result == "enroll-completed") {
                    m_progress = 1.0F;
                    finish(true);
                    return;
                  }
                  if (result == "enroll-stage-passed") {
                    ++m_fingerPassed;
                    m_progress = m_fingerStages > 0
                                     ? static_cast<float>(m_fingerPassed) / static_cast<float>(m_fingerStages)
                                     : 0.0F;
                    m_prompt = i18n::tr("setup-wizard.finger-again");
                  } else {
                    m_prompt = i18n::tr("setup-wizard.finger-retry");
                  }
                  if (done) {
                    finish(false);
                  } else {
                    changed();
                  }
                });
            m_fprintDevice->callMethodAsync("Get")
                .onInterface(kPropertiesInterface)
                .withArguments(std::string{kFprintDeviceInterface}, std::string{"num-enroll-stages"})
                .uponReplyInvoke([this](std::optional<sdbus::Error> e, const sdbus::Variant& value) {
                  m_fingerStages = e.has_value() ? 0 : value.get<std::int32_t>();
                });
          }
          m_fingerPresent = true;
          listFingers();
        });
  });
}

void BiometricEnrollment::listFingers() {
  if (m_fprintDevice == nullptr) {
    return;
  }
  tryCall("fprintd ListEnrolledFingers", [&] {
    m_fprintDevice->callMethodAsync("ListEnrolledFingers")
        .onInterface(kFprintDeviceInterface)
        .withArguments(m_user)
        .uponReplyInvoke([this](std::optional<sdbus::Error> e, const std::vector<std::string>& fingers) {
          // fprintd answers NoEnrolledPrints instead of an empty list.
          m_fingers = e.has_value() ? std::vector<std::string>{} : fingers;
          changed();
        });
  });
}

void BiometricEnrollment::start(Method method, const std::string& name) {
  if (m_enrolling != Method::None || method == Method::None) {
    return;
  }
  m_enrolling = method;
  m_progress = 0.0F;
  m_fingerPassed = 0;
  m_prompt = i18n::tr(method == Method::Face ? "setup-wizard.face-turn" : "setup-wizard.finger-place");
  changed();

  if (method == Method::Face) {
    const std::string faceName = name.empty() ? nextFaceName() : name;
    if (m_gaze == nullptr || !tryCall("gazed Claim", [&] {
          m_gaze->callMethodAsync("Claim")
              .onInterface(kGazeInterface)
              .withArguments(m_user)
              .uponReplyInvoke([this, faceName](std::optional<sdbus::Error> e) {
                if (e.has_value()) {
                  kLog.info("gazed Claim failed: {}", e->what());
                  if (m_enrolling == Method::Face) {
                    finish(false);
                  }
                  return;
                }
                m_faceClaimed = true;
                // Cancelled while the claim was pending: a held claim blocks face unlock on the lock screen.
                if (m_enrolling != Method::Face) {
                  releaseFace();
                  return;
                }
                tryCall("gazed EnrollStart", [&] {
                  m_gaze->callMethodAsync("EnrollStart")
                      .onInterface(kGazeInterface)
                      .withArguments(faceName)
                      .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
                        if (e.has_value() && m_enrolling == Method::Face) {
                          kLog.info("gazed EnrollStart failed: {}", e->what());
                          finish(false);
                        }
                      });
                });
              });
        })) {
      finish(false);
    }
    return;
  }

  std::string finger = name;
  if (finger.empty()) {
    for (const char* candidate : kFingers) {
      if (std::ranges::find(m_fingers, candidate) == m_fingers.end()) {
        finger = candidate;
        break;
      }
    }
  }
  if (m_fprintDevice == nullptr || finger.empty() || !tryCall("fprintd Claim", [&] {
        m_fprintDevice->callMethodAsync("Claim")
            .onInterface(kFprintDeviceInterface)
            .withArguments(m_user)
            .uponReplyInvoke([this, finger](std::optional<sdbus::Error> e) {
              if (e.has_value()) {
                kLog.info("fprintd Claim failed: {}", e->what());
                if (m_enrolling == Method::Finger) {
                  finish(false);
                }
                return;
              }
              m_fingerClaimed = true;
              if (m_enrolling != Method::Finger) {
                releaseFinger();
                return;
              }
              tryCall("fprintd EnrollStart", [&] {
                m_fprintDevice->callMethodAsync("EnrollStart")
                    .onInterface(kFprintDeviceInterface)
                    .withArguments(finger)
                    .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
                      if (e.has_value() && m_enrolling == Method::Finger) {
                        kLog.info("fprintd EnrollStart failed: {}", e->what());
                        finish(false);
                      }
                    });
              });
            });
      })) {
    finish(false);
  }
}

void BiometricEnrollment::releaseFace() {
  if (!m_faceClaimed || m_gaze == nullptr) {
    m_faceClaimed = false;
    return;
  }
  m_faceClaimed = false;
  tryCall("gazed EnrollStop", [&] {
    m_gaze->callMethodAsync("EnrollStop").onInterface(kGazeInterface).uponReplyInvoke(ignoreReply);
    m_gaze->callMethodAsync("Release").onInterface(kGazeInterface).uponReplyInvoke(ignoreReply);
  });
}

void BiometricEnrollment::releaseFinger() {
  if (!m_fingerClaimed || m_fprintDevice == nullptr) {
    m_fingerClaimed = false;
    return;
  }
  m_fingerClaimed = false;
  tryCall("fprintd EnrollStop", [&] {
    m_fprintDevice->callMethodAsync("EnrollStop").onInterface(kFprintDeviceInterface).uponReplyInvoke(ignoreReply);
    m_fprintDevice->callMethodAsync("Release").onInterface(kFprintDeviceInterface).uponReplyInvoke(ignoreReply);
  });
}

void BiometricEnrollment::stop() {
  m_enrolling = Method::None;
  releaseFace();
  releaseFinger();
}

void BiometricEnrollment::finish(bool ok) {
  const Method method = m_enrolling;
  stop();
  probe();
  if (m_onFinished) {
    m_onFinished(method, ok);
  }
  changed();
}

void BiometricEnrollment::removeFace(const std::string& name, DoneCallback done) {
  if (m_gaze == nullptr || m_enrolling != Method::None) {
    done(false);
    return;
  }
  if (!tryCall("gazed DeleteFace", [&] {
        m_gaze->callMethodAsync("DeleteFace")
            .onInterface(kGazeInterface)
            .withArguments(m_user, name)
            .uponReplyInvoke([this, done](std::optional<sdbus::Error> e, bool ok) {
              if (e.has_value()) {
                kLog.info("gazed DeleteFace failed: {}", e->what());
              }
              probeFace();
              done(!e.has_value() && ok);
            });
      })) {
    done(false);
  }
}

void BiometricEnrollment::renameFace(const std::string& from, const std::string& to, DoneCallback done) {
  if (m_gaze == nullptr || m_enrolling != Method::None || to.empty() || from == to) {
    done(false);
    return;
  }
  if (!tryCall("gazed RenameFace", [&] {
        m_gaze->callMethodAsync("RenameFace")
            .onInterface(kGazeInterface)
            .withArguments(m_user, from, to)
            .uponReplyInvoke([this, done](std::optional<sdbus::Error> e, bool ok) {
              if (e.has_value()) {
                kLog.info("gazed RenameFace failed: {}", e->what());
              }
              probeFace();
              done(!e.has_value() && ok);
            });
      })) {
    done(false);
  }
}

// fprintd deletes only on a claimed device: Claim, DeleteEnrolledFinger, Release.
void BiometricEnrollment::removeFinger(const std::string& finger, DoneCallback done) {
  if (m_fprintDevice == nullptr || m_enrolling != Method::None) {
    done(false);
    return;
  }
  if (!tryCall("fprintd Claim", [&] {
        m_fprintDevice->callMethodAsync("Claim")
            .onInterface(kFprintDeviceInterface)
            .withArguments(m_user)
            .uponReplyInvoke([this, finger, done](std::optional<sdbus::Error> e) {
              if (e.has_value()) {
                kLog.info("fprintd Claim failed: {}", e->what());
                done(false);
                return;
              }
              if (!tryCall("fprintd DeleteEnrolledFinger", [&] {
                    m_fprintDevice->callMethodAsync("DeleteEnrolledFinger")
                        .onInterface(kFprintDeviceInterface)
                        .withArguments(finger)
                        .uponReplyInvoke([this, done](std::optional<sdbus::Error> e) {
                          if (e.has_value()) {
                            kLog.info("fprintd DeleteEnrolledFinger failed: {}", e->what());
                          }
                          tryCall("fprintd Release", [&] {
                            m_fprintDevice->callMethodAsync("Release")
                                .onInterface(kFprintDeviceInterface)
                                .uponReplyInvoke(ignoreReply);
                          });
                          listFingers();
                          done(!e.has_value());
                        });
                  })) {
                tryCall("fprintd Release", [&] {
                  m_fprintDevice->callMethodAsync("Release").onInterface(kFprintDeviceInterface).uponReplyInvoke(
                      ignoreReply
                  );
                });
                done(false);
              }
            });
      })) {
    done(false);
  }
}

std::string BiometricEnrollment::nextFaceName() const {
  std::string name = "Face";
  for (int n = 2; std::ranges::find(m_faces, name) != m_faces.end(); ++n) {
    name = std::format("Face {}", n);
  }
  return name;
}

std::string BiometricEnrollment::fingerLabel(const std::string& finger) {
  return i18n::tr(std::format("auth.finger.{}", finger));
}

#include "greeter/gaze_host.h"

#include "core/log.h"

#include <gio/gio.h>
#include <string_view>

namespace {
  constexpr Logger kLog("gaze-host");

  constexpr char kGazeBusName[] = "com.gundulabs.Gaze";
  constexpr char kGazePath[] = "/com/gundulabs/Gaze";
  constexpr char kGazeInterface[] = "com.gundulabs.Gaze";
  // Must match greetd's [general] service: pam_gaze hands the keyring password over only under this name.
  constexpr char kLoginService[] = "gdm-face";
} // namespace

GazeHost::~GazeHost() {
  auto* connection = m_connection;
  if (connection == nullptr) {
    return;
  }
  if (m_gazeSubscription != 0) {
    g_dbus_connection_signal_unsubscribe(connection, m_gazeSubscription);
  }
  if (m_ownerSubscription != 0) {
    g_dbus_connection_signal_unsubscribe(connection, m_ownerSubscription);
  }
  g_object_unref(connection);
}

void GazeHost::start(StatusCallback onStatus, MatchCallback onMatch, MissCallback onMiss) {
  GError* error = nullptr;
  auto* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (connection == nullptr) {
    kLog.info("face login off: no system bus ({})", error != nullptr ? error->message : "unknown");
    g_clear_error(&error);
    return;
  }
  m_connection = connection;
  m_onStatus = std::move(onStatus);
  m_onMatch = std::move(onMatch);
  m_onMiss = std::move(onMiss);

  const GDBusSignalCallback callback = &GazeHost::onSignal;
  m_gazeSubscription = g_dbus_connection_signal_subscribe(
      connection, kGazeBusName, kGazeInterface, nullptr, kGazePath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, callback, this,
      nullptr
  );
  // gazed keeps the host list in memory, so a restarted gazed needs the registration again.
  m_ownerSubscription = g_dbus_connection_signal_subscribe(
      connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus",
      kGazeBusName, G_DBUS_SIGNAL_FLAGS_NONE, callback, this, nullptr
  );
  registerService();
}

void GazeHost::registerService() {
  g_dbus_connection_call(
      m_connection, kGazeBusName, kGazePath, kGazeInterface, "AddPamInternal", g_variant_new("(s)", kLoginService),
      nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer /*userData*/) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (reply == nullptr) {
          kLog.info("face login prompts off: {}", error != nullptr ? error->message : "no reply");
          g_clear_error(&error);
          return;
        }
        g_variant_unref(reply);
        kLog.info("gazed sends face prompts for {} to the greeter", kLoginService);
      },
      nullptr
  );
}

void GazeHost::onSignal(
    GDBusConnection* /*connection*/, const char* /*sender*/, const char* /*objectPath*/, const char* /*interfaceName*/,
    const char* signalName, GVariant* params, void* userData
) {
  auto* self = static_cast<GazeHost*>(userData);
  const std::string_view signal = signalName;

  if (signal == "NameOwnerChanged") {
    const char* newOwner = nullptr;
    g_variant_get(params, "(&s&s&s)", nullptr, nullptr, &newOwner);
    if (newOwner != nullptr && newOwner[0] != '\0') {
      self->registerService();
    }
    return;
  }
  if (signal == "FaceStatus" && g_variant_is_of_type(params, G_VARIANT_TYPE("(s)"))) {
    const char* status = nullptr;
    g_variant_get(params, "(&s)", &status);
    if (self->m_onStatus && status != nullptr) {
      self->m_onStatus(status);
    }
    return;
  }
  if (signal == "VerifyStatus" && g_variant_n_children(params) >= 1) {
    GVariant* result = g_variant_get_child_value(params, 0);
    const bool matched = std::string_view(g_variant_get_string(result, nullptr)) == "verify-match";
    g_variant_unref(result);
    kLog.info("gazed verdict: {}", matched ? "match" : "no match");
    if (matched && self->m_onMatch) {
      self->m_onMatch();
    } else if (!matched && self->m_onMiss && g_variant_n_children(params) >= 3) {
      GVariant* rgb = g_variant_get_child_value(params, 2);
      const std::string rgbStatus = g_variant_get_string(rgb, nullptr);
      g_variant_unref(rgb);
      self->m_onMiss(rgbStatus);
    }
  }
}

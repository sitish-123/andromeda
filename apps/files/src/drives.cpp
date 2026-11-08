#include "drives.h"

#include "state_file.h"
#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-drives");
  constexpr const char* kUdisks = "org.freedesktop.UDisks2";

  std::string takeString(char* s) {
    std::string out = s != nullptr ? s : "";
    g_free(s);
    return out;
  }

  std::string idOf(GVolume* v) {
    for (const char* kind : {G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE, G_VOLUME_IDENTIFIER_KIND_UUID}) {
      if (std::string id = takeString(g_volume_get_identifier(v, kind)); !id.empty()) {
        return id;
      }
    }
    return takeString(g_volume_get_name(v));
  }

  std::string scheme(GVolume* v) {
    GFile* root = g_volume_get_activation_root(v);
    if (root == nullptr) {
      return {};
    }
    std::string out = takeString(g_file_get_uri_scheme(root));
    g_object_unref(root);
    return out;
  }

  // The udisks2 object for a /dev node; empty when udisks does not know it.
  std::string udisksObject(GDBusConnection* bus, const std::string& device) {
    GVariantBuilder spec;
    g_variant_builder_init(&spec, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&spec, "{sv}", "path", g_variant_new_string(device.c_str()));
    GVariant* reply = g_dbus_connection_call_sync(
        bus, kUdisks, "/org/freedesktop/UDisks2/Manager", "org.freedesktop.UDisks2.Manager", "ResolveDevice",
        g_variant_new("(a{sv}a{sv})", &spec, nullptr), G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        nullptr);
    if (reply == nullptr) {
      return {};
    }
    GVariantIter* it = nullptr;
    g_variant_get(reply, "(ao)", &it);
    const char* path = nullptr;
    std::string out;
    if (g_variant_iter_next(it, "&o", &path)) {
      out = path;
    }
    g_variant_iter_free(it);
    g_variant_unref(reply);
    return out;
  }

  bool isCrypto(const std::string& device) {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (bus == nullptr) {
      return false;
    }
    bool crypto = false;
    if (const std::string object = udisksObject(bus, device); !object.empty()) {
      GVariant* reply = g_dbus_connection_call_sync(
          bus, kUdisks, object.c_str(), "org.freedesktop.DBus.Properties", "Get",
          g_variant_new("(ss)", "org.freedesktop.UDisks2.Block", "IdUsage"), G_VARIANT_TYPE("(v)"),
          G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
      if (reply != nullptr) {
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        crypto = std::string_view(g_variant_get_string(value, nullptr)) == "crypto";
        g_variant_unref(value);
        g_variant_unref(reply);
      }
    }
    g_object_unref(bus);
    return crypto;
  }

  // Owns the mount operation until the mount ends: GIO's proxy volume does not keep it alive for later signals.
  struct MountCall {
    Drives::AskPassword ask;
    Drives::Mounted done;
    std::string name;
    GMountOperation* op = nullptr;
    ~MountCall() { g_clear_object(&op); }
  };
} // namespace

Drives::Drives(std::function<void()> changed) : m_monitor(g_volume_monitor_get()), m_changed(std::move(changed)) {
  for (const char* signal : {"volume-added", "volume-removed", "volume-changed", "mount-added", "mount-removed"}) {
    g_signal_connect_swapped(m_monitor, signal, G_CALLBACK(+[](Drives* self) {
                               if (self->m_changed) {
                                 self->m_changed();
                               }
                             }),
                             this);
  }
}

Drives::~Drives() {
  g_signal_handlers_disconnect_by_data(m_monitor, this);
  g_object_unref(m_monitor);
}

std::vector<Drives::Drive> Drives::list() const {
  std::vector<Drive> out;
  GList* volumes = g_volume_monitor_get_volumes(m_monitor);
  for (GList* l = volumes; l != nullptr; l = l->next) {
    auto* v = G_VOLUME(l->data);
    Drive d{.id = idOf(v), .name = takeString(g_volume_get_name(v))};
    const std::string kind = scheme(v);
    d.device = takeString(g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
    if (kind == "mtp") {
      d.kind = Kind::Phone;
    } else if (kind == "gphoto2") {
      d.kind = Kind::Camera;
    } else if (GDrive* drive = g_volume_get_drive(v)) {
      d.kind = g_drive_is_removable(drive) ? Kind::Usb : Kind::Drive;
      g_object_unref(drive);
    }
    if (GMount* mount = g_volume_get_mount(v)) {
      d.mounted = true;
      GFile* root = g_mount_get_root(mount);
      d.root = takeString(g_file_get_path(root));
      g_object_unref(root);
      g_object_unref(mount);
    } else if (!d.device.empty()) {
      d.encrypted = isCrypto(d.device);
    }
    // Mounted block drives already show through listVolumes(); only phones and cameras stay here once mounted.
    if (!d.mounted || d.device.empty()) {
      out.push_back(std::move(d));
    }
  }
  g_list_free_full(volumes, g_object_unref);
  return out;
}

void Drives::mount(const std::string& id, AskPassword ask, Mounted done) {
  GVolume* found = nullptr;
  GList* volumes = g_volume_monitor_get_volumes(m_monitor);
  for (GList* l = volumes; l != nullptr; l = l->next) {
    if (idOf(G_VOLUME(l->data)) == id) {
      found = G_VOLUME(g_object_ref(l->data));
      break;
    }
  }
  g_list_free_full(volumes, g_object_unref);
  if (found == nullptr) {
    done({}, "The drive is gone.");
    return;
  }
  auto* call = new MountCall{.ask = std::move(ask), .done = std::move(done), .name = takeString(g_volume_get_name(found))};
  GMountOperation* op = g_mount_operation_new();
  call->op = op;
  g_signal_connect(op, "ask-password",
                   G_CALLBACK(+[](GMountOperation* o, const char* message, const char*, const char*, GAskPasswordFlags,
                                  gpointer data) {
                     auto* c = static_cast<MountCall*>(data);
                     kLog.debug("drive asks password for {}", c->name);
                     if (!c->ask) {
                       g_mount_operation_reply(o, G_MOUNT_OPERATION_ABORTED);
                       return;
                     }
                     static_cast<void>(g_object_ref(o));
                     c->ask(c->name, message != nullptr ? message : "", [o](const std::string* password) {
                       if (password == nullptr) {
                         g_mount_operation_reply(o, G_MOUNT_OPERATION_ABORTED);
                       } else {
                         g_mount_operation_set_password(o, password->c_str());
                         g_mount_operation_reply(o, G_MOUNT_OPERATION_HANDLED);
                       }
                       g_object_unref(o);
                     });
                   }),
                   call);
  kLog.debug("drive mount {}", id);
  g_volume_mount(
      found, G_MOUNT_MOUNT_NONE, op, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<MountCall> c(static_cast<MountCall*>(data));
        auto* v = G_VOLUME(source);
        GError* error = nullptr;
        if (!g_volume_mount_finish(v, result, &error)) {
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED);
          std::string message = cancelled ? std::string() : error->message;
          kLog.debug("drive mount failed: {}", error->message);
          g_clear_error(&error);
          c->done({}, cancelled ? std::string("cancelled") : message);
        } else {
          fs::path root;
          if (GMount* mount = g_volume_get_mount(v)) {
            GFile* file = g_mount_get_root(mount);
            root = takeString(g_file_get_path(file));
            g_object_unref(file);
            g_object_unref(mount);
          }
          kLog.debug("drive mounted at {}", root.string());
          c->done(root, root.empty() ? std::string("The drive has no folder to show.") : std::string());
        }
        g_object_unref(v);
      },
      call);
}

namespace {
  struct UnlockCall {
    Drives::Mounted done;
    GDBusConnection* bus;
    ~UnlockCall() { g_object_unref(bus); }
  };

  std::string dbusError(GError* error) {
    g_dbus_error_strip_remote_error(error);
    std::string message = error->message;
    g_clear_error(&error);
    return message;
  }

  void mountCleartext(std::unique_ptr<UnlockCall> call, const std::string& object) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    UnlockCall* raw = call.release();
    g_dbus_connection_call(
        raw->bus, kUdisks, object.c_str(), "org.freedesktop.UDisks2.Filesystem", "Mount",
        g_variant_new("(a{sv})", &options), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<UnlockCall> c(static_cast<UnlockCall*>(data));
          GError* error = nullptr;
          GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
          if (reply == nullptr) {
            const std::string message = dbusError(error);
            kLog.debug("drive mount failed: {}", message);
            c->done({}, message);
            return;
          }
          const char* path = nullptr;
          g_variant_get(reply, "(&s)", &path);
          const fs::path root = path;
          g_variant_unref(reply);
          kLog.debug("drive mounted at {}", root.string());
          c->done(root, {});
        },
        raw);
  }
} // namespace

void Drives::unlock(const std::string& device, const std::string& passphrase, Mounted done) {
  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (bus == nullptr) {
    done({}, dbusError(error));
    return;
  }
  const std::string object = udisksObject(bus, device);
  if (object.empty()) {
    g_object_unref(bus);
    done({}, "The drive is not known to the system.");
    return;
  }
  kLog.debug("drive unlock {} ({} characters)", device, passphrase.size());
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  auto* call = new UnlockCall{.done = std::move(done), .bus = bus};
  g_dbus_connection_call(
      bus, kUdisks, object.c_str(), "org.freedesktop.UDisks2.Encrypted", "Unlock",
      g_variant_new("(sa{sv})", passphrase.c_str(), &options), G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<UnlockCall> c(static_cast<UnlockCall*>(data));
        GError* err = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &err);
        if (reply == nullptr) {
          // udisks reports a wrong passphrase as a failed activation (cryptsetup EPERM).
          const bool wrong = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
                             std::string_view(err->message).find("activate") != std::string_view::npos;
          const std::string message = dbusError(err);
          kLog.debug("drive unlock failed: {}", message);
          c->done({}, wrong ? std::string("The password is not correct.") : message);
          return;
        }
        const char* cleartext = nullptr;
        g_variant_get(reply, "(&o)", &cleartext);
        const std::string cleartextObject = cleartext;
        g_variant_unref(reply);
        mountCleartext(std::move(c), cleartextObject);
      },
      call);
}

std::string Drives::deviceOf(const fs::path& mount) {
  std::ifstream in("/proc/self/mounts");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string device;
    std::string point;
    if (fields >> device >> point && point == mount.string() && device.starts_with("/dev/")) {
      return device;
    }
  }
  return {};
}

std::vector<std::string> Drives::formats() {
  std::vector<std::string> out;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
  if (bus == nullptr) {
    return out;
  }
  for (const char* type : {"exfat", "vfat", "ntfs", "ext4"}) {
    GVariant* reply = g_dbus_connection_call_sync(bus, kUdisks, "/org/freedesktop/UDisks2/Manager",
                                                  "org.freedesktop.UDisks2.Manager", "CanFormat",
                                                  g_variant_new("(s)", type), G_VARIANT_TYPE("((bs))"),
                                                  G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (reply != nullptr) {
      gboolean can = FALSE;
      const char* missing = nullptr;
      g_variant_get(reply, "((b&s))", &can, &missing);
      if (can) {
        out.emplace_back(type);
      }
      g_variant_unref(reply);
    }
  }
  g_object_unref(bus);
  return out;
}

void Drives::format(const std::string& device, const std::string& type, const std::string& label,
                    std::function<void(const std::string& error)> done) {
  kLog.debug("drive format {} as {} \"{}\"", device, type, label);
  // Sync D-Bus calls on a worker. Unmount and lock come first, done here: udisks's tear-down option also edits
  // fstab/crypttab and so asks for the admin password (modify-system-configuration).
  std::thread([device, type, label, done = std::move(done)] {
    std::string error;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    const auto call = [&](const std::string& object, const char* iface, const char* method, GVariant* args) {
      GError* err = nullptr;
      GVariant* reply = g_dbus_connection_call_sync(bus, kUdisks, object.c_str(), iface, method, args, nullptr,
                                                    G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
                                                    nullptr, &err);
      if (reply == nullptr) {
        error = dbusError(err);
        return false;
      }
      g_variant_unref(reply);
      return true;
    };
    const auto noOptions = [] {
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
      return g_variant_builder_end(&b);
    };
    const auto property = [&](const std::string& object, const char* name) {
      std::string out;
      if (GVariant* reply = g_dbus_connection_call_sync(
              bus, kUdisks, object.c_str(), "org.freedesktop.DBus.Properties", "Get",
              g_variant_new("(ss)", "org.freedesktop.UDisks2.Block", name), G_VARIANT_TYPE("(v)"),
              G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr)) {
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        out = g_variant_get_string(value, nullptr);
        g_variant_unref(value);
        g_variant_unref(reply);
      }
      return out;
    };
    std::string object = bus != nullptr ? udisksObject(bus, device) : std::string();
    if (bus == nullptr || object.empty()) {
      error = "The drive is not known to the system.";
    } else {
      const std::string backing = property(object, "CryptoBackingDevice");
      if (call(object, "org.freedesktop.UDisks2.Filesystem", "Unmount", g_variant_new("(@a{sv})", noOptions())) &&
          !backing.empty() && backing != "/" &&
          call(backing, "org.freedesktop.UDisks2.Encrypted", "Lock", g_variant_new("(@a{sv})", noOptions()))) {
        object = backing; // the whole drive is erased, its encryption with it
      }
      if (error.empty()) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "label", g_variant_new_string(label.c_str()));
        g_variant_builder_add(&options, "{sv}", "update-partition-type", g_variant_new_boolean(TRUE));
        call(object, "org.freedesktop.UDisks2.Block", "Format", g_variant_new("(sa{sv})", type.c_str(), &options));
      }
    }
    g_clear_object(&bus);
    DeferredCall::callLater([error, done] {
      kLog.debug("drive format {}", error.empty() ? std::string("done") : "failed: " + error);
      done(error);
    });
  }).detach();
}

PhotoImport::PhotoImport(std::string file) : m_file(std::move(file)) {
  std::ifstream in(fs::path(g_get_user_data_dir()) / "andromeda" / "files" / m_file);
  const nlohmann::json list = nlohmann::json::parse(in, nullptr, false);
  if (list.is_array()) {
    for (const auto& k : list) {
      if (k.is_string()) {
        m_done.push_back(k.get<std::string>());
      }
    }
  }
}

fs::path PhotoImport::dcim(const fs::path& root) {
  std::error_code ec;
  if (fs::is_directory(root / "DCIM", ec)) {
    return root / "DCIM";
  }
  // Phones over MTP show storages first ("Internal shared storage/DCIM").
  for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code e2;
    if (entry.is_directory(e2) && fs::is_directory(entry.path() / "DCIM", e2)) {
      return entry.path() / "DCIM";
    }
  }
  return {};
}

std::string PhotoImport::key(const fs::path& file) {
  struct stat st{};
  if (::stat(file.c_str(), &st) != 0) {
    return {};
  }
  // The DCIM-relative path: the same card shows under another mount point next time.
  const std::string path = file.string();
  const auto at = path.find("/DCIM/");
  return std::format("{}|{}|{}", at == std::string::npos ? path : path.substr(at + 1), st.st_size, st.st_mtim.tv_sec);
}

std::vector<fs::path> PhotoImport::pending(const fs::path& dcim) const {
  static const std::set<std::string> kMedia{".jpg", ".jpeg", ".png", ".heic", ".heif", ".dng", ".raw", ".cr2", ".cr3",
                                            ".nef", ".arw", ".orf", ".rw2", ".mp4", ".mov", ".m4v", ".3gp", ".avi", ".webp"};
  const std::set<std::string> done(m_done.begin(), m_done.end());
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dcim, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) {
      continue;
    }
    std::string ext = it->path().extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (kMedia.contains(ext) && !done.contains(key(it->path()))) {
      out.push_back(it->path());
    }
  }
  std::ranges::sort(out);
  return out;
}

void PhotoImport::remember(const std::vector<fs::path>& files) {
  for (const fs::path& f : files) {
    if (std::string k = key(f); !k.empty()) {
      m_done.push_back(std::move(k));
    }
  }
  save();
}

void PhotoImport::save() const {
  const fs::path file = fs::path(g_get_user_data_dir()) / "andromeda" / "files" / m_file;
  writeStateFile(file, nlohmann::json(m_done).dump());
}

fs::path PhotoImport::destination() {
  const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
  const fs::path base = pictures != nullptr ? fs::path(pictures) : fs::path(g_get_home_dir()) / "Pictures";
  const auto today = std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
  return base / std::format("{:%F}", std::chrono::sys_days(today));
}

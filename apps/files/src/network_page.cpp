#include "network_page.h"

#include "core/log.h"
#include "ui/controls/flex.h"
#include "ui/files/this_pc.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <gio/gio.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-network");
  constexpr guint kLookingSeconds = 3;
  constexpr std::size_t kSavedMax = 12;

  using Weak = std::weak_ptr<NetworkPage>;

  std::string protocolLabel(std::string_view uri) {
    const std::string_view scheme = uri.substr(0, uri.find(':'));
    if (scheme == "smb") {
      return "Windows share (SMB)";
    }
    if (scheme == "sftp" || scheme == "ssh") {
      return "SFTP";
    }
    if (scheme == "dav" || scheme == "davs") {
      return "WebDAV";
    }
    if (scheme == "nfs") {
      return "NFS";
    }
    if (scheme == "ftp") {
      return "FTP";
    }
    return std::string(scheme);
  }

  bool offlineError(const GError* error) {
    return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED);
  }

  // One key per server: gvfs and saved servers write "sftp://host/", discovery may write "sftp://host".
  std::string serverKey(std::string_view uri) {
    while (uri.ends_with('/') && !uri.ends_with("://")) {
      uri.remove_suffix(1);
    }
    return std::string(uri);
  }
} // namespace

struct NetworkPage::MountJob {
  Weak page;
  std::string uri;
  std::string label;
  std::string user;
  std::string password;
  bool remember = false;
  bool fromSheet = false;     // started by the Connect sheet, which shows the error text
  bool answer = false;        // reply to a question with its first choice
  bool sentPassword = false;  // a second password request means the password was wrong
  bool needsPassword = false;
  std::string question;
  MountDone done;
  GMountOperation* op = nullptr;
  GFile* file = nullptr;

  ~MountJob() {
    if (op != nullptr) {
      g_object_unref(op);
    }
    if (file != nullptr) {
      g_object_unref(file);
    }
  }
};

NetworkPage::NetworkPage(Actions actions) : m_actions(std::move(actions)), m_saved(loadServers()) {}

NetworkPage::~NetworkPage() {
  *m_alive = false;
  if (m_monitor != nullptr) {
    g_file_monitor_cancel(m_monitor);
    g_object_unref(m_monitor);
  }
}

fs::path NetworkPage::serversPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  const fs::path base = xdg != nullptr && *xdg != '\0' ? fs::path(xdg) : fs::path(home != nullptr ? home : "") / ".config";
  return base / "andromeda" / "files" / "servers.json";
}

std::vector<NetworkPage::Server> NetworkPage::loadServers() {
  std::vector<Server> out;
  std::ifstream in(serversPath());
  if (!in) {
    return out;
  }
  const auto doc = nlohmann::json::parse(in, nullptr, false);
  if (!doc.is_array()) {
    return out;
  }
  for (const auto& s : doc) {
    const std::string uri = s.is_object() ? s.value("uri", "") : "";
    // Only addresses Connect would take: a hand-edited file cannot inject a password or a scheme we do not mount.
    if (const auto url = parseServerUrl(uri); url.has_value()) {
      out.push_back({.name = s.value("name", url->label()), .uri = url->uri()});
    }
  }
  return out;
}

void NetworkPage::saveServer(const Server& server) {
  std::vector<Server> all = loadServers();
  std::erase_if(all, [&](const Server& s) { return s.uri == server.uri; });
  all.insert(all.begin(), server);
  if (all.size() > kSavedMax) {
    all.resize(kSavedMax);
  }
  nlohmann::json doc = nlohmann::json::array();
  for (const Server& s : all) {
    doc.push_back({{"name", s.name}, {"uri", s.uri}});
  }
  const fs::path path = serversPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << doc.dump(2) << '\n';
    if (!out) {
      return;
    }
  }
  fs::rename(tmp, path, ec);
}

void NetworkPage::start() {
  GFile* net = g_file_new_for_uri("network:///");
  GError* error = nullptr;
  m_monitor = g_file_monitor_directory(net, G_FILE_MONITOR_NONE, nullptr, &error);
  if (m_monitor != nullptr) {
    g_signal_connect_data(
        m_monitor, "changed",
        G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer data) {
          if (auto page = static_cast<Weak*>(data)->lock()) {
            page->enumerate();
          }
        }),
        new Weak(weak_from_this()), +[](gpointer data, GClosure*) { delete static_cast<Weak*>(data); },
        GConnectFlags{});
  } else {
    kLog.debug("no network:/// monitor: {}", error != nullptr ? error->message : "?");
    g_clear_error(&error);
  }
  g_object_unref(net);
  g_timeout_add_seconds_full(
      G_PRIORITY_DEFAULT, kLookingSeconds,
      +[](gpointer data) -> gboolean {
        if (auto page = static_cast<Weak*>(data)->lock()) {
          page->m_looking = false;
          if (page->m_actions.rebuild) {
            page->m_actions.rebuild();
          }
        }
        return G_SOURCE_REMOVE;
      },
      new Weak(weak_from_this()), +[](gpointer data) { delete static_cast<Weak*>(data); });
  enumerate();
}

void NetworkPage::enumerate() {
  if (m_enumerating) {
    return;
  }
  m_enumerating = true;
  GFile* net = g_file_new_for_uri("network:///");
  g_file_enumerate_children_async(
      net, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, G_FILE_QUERY_INFO_NONE,
      G_PRIORITY_DEFAULT, nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Weak> weak(static_cast<Weak*>(data));
        GError* error = nullptr;
        GFileEnumerator* children = g_file_enumerate_children_finish(G_FILE(source), result, &error);
        auto page = weak->lock();
        if (children == nullptr) {
          kLog.debug("network:/// unavailable: {}", error != nullptr ? error->message : "?");
          g_clear_error(&error);
          if (page) {
            page->m_enumerating = false;
            page->m_found.clear();
          }
          return;
        }
        g_file_enumerator_next_files_async(
            children, 256, G_PRIORITY_DEFAULT, nullptr,
            +[](GObject* list, GAsyncResult* listed, gpointer listData) {
              std::unique_ptr<Weak> owner(static_cast<Weak*>(listData));
              GError* nextError = nullptr;
              GList* infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(list), listed, &nextError);
              g_clear_error(&nextError);
              std::vector<Server> found;
              for (GList* l = infos; l != nullptr; l = l->next) {
                auto* info = G_FILE_INFO(l->data);
                const char* name = g_file_info_get_display_name(info);
                const char* target = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
                if (name != nullptr && target != nullptr) {
                  found.push_back({.name = name, .uri = target});
                }
              }
              g_list_free_full(infos, g_object_unref);
              g_object_unref(list);
              if (auto self = owner->lock()) {
                self->m_enumerating = false;
                std::ranges::sort(found, {}, &Server::name);
                self->m_found = std::move(found);
                if (!self->m_found.empty()) {
                  self->m_looking = false;
                }
                if (self->m_actions.rebuild) {
                  self->m_actions.rebuild();
                }
              }
            },
            weak.release());
      },
      new Weak(weak_from_this()));
  g_object_unref(net);
}

std::unique_ptr<Node> NetworkPage::build(float scale) {
  const auto cell = [this](const Server& server, std::string line) {
    const bool offline = m_offline.contains(serverKey(server.uri));
    // The URI rides in `path`: place cells do not read it, and openPlace() needs it back.
    return ThisPcPlace{.label = server.name, .path = fs::path(server.uri), .art = "network",
                       .caption = offline ? std::string("Not responding") : std::move(line), .warn = offline};
  };
  std::vector<PlaceSection> sections;
  PlaceSection found{.title = "On this network"};
  if (m_found.empty()) {
    found.caption = m_looking ? "Looking…" : "Nothing found on this network";
  }
  for (const Server& server : m_found) {
    found.places.push_back(cell(server, protocolLabel(server.uri)));
  }
  sections.push_back(std::move(found));
  if (!m_saved.empty()) {
    PlaceSection saved{.title = "Saved servers"};
    for (const Server& server : m_saved) {
      saved.places.push_back(cell(server, server.uri));
    }
    sections.push_back(std::move(saved));
  }
  return buildPlaceSections(
      scale, std::move(sections), [weak = weak_from_this()](const ThisPcPlace& place) {
        if (auto page = weak.lock()) {
          page->openPlace(place);
        }
      },
      PlaceAction{.label = "Connect to Server…", .run = [weak = weak_from_this()] {
                    if (auto page = weak.lock(); page && page->m_actions.connect) {
                      page->m_actions.connect({});
                    }
                  }});
}

void NetworkPage::openPlace(const ThisPcPlace& place) {
  const std::string uri = place.path.string();
  const auto url = parseServerUrl(uri);
  if (!url.has_value()) {
    // A gvfs browse target (network:///…) or a scheme Connect does not take: mount it as gvfs lists it.
    startMount(ServerUrl{.scheme = uri.substr(0, uri.find(':')), .host = uri}, {}, {}, false, {}, false);
    return;
  }
  startMount(*url, url->user, {}, false, [weak = weak_from_this(), key = serverKey(uri), uri](const std::string& error,
                                                                                              bool) {
    auto page = weak.lock();
    if (!page || error.empty()) {
      return;
    }
    // A server that does not answer shows on its cell; a password, a question (never answered from a cell: its text
    // must show first) or another error goes to the sheet, whose Connect shows it.
    if (!page->m_offline.contains(key) && page->m_actions.connect) {
      page->m_actions.connect(uri);
    }
  }, false);
}

void NetworkPage::forgetQuestion() { m_questionKey.clear(); }

void NetworkPage::mount(const ServerUrl& url, const std::string& user, const std::string& password, bool remember,
                        MountDone done) {
  startMount(url, user, password, remember, std::move(done), true);
}

void NetworkPage::startMount(const ServerUrl& url, const std::string& user, const std::string& password,
                             bool remember, MountDone done, bool fromSheet) {
  auto* job = new MountJob;
  job->page = weak_from_this();
  // A gvfs browse target keeps its own URI (openPlace puts it in host).
  job->uri = url.host.find("://") != std::string::npos ? url.host : url.uri();
  job->label = url.host.find("://") != std::string::npos ? url.host : url.label();
  job->user = user;
  job->password = password;
  job->remember = remember;
  job->fromSheet = fromSheet;
  // Only the sheet's next Connect after it showed this server's question accepts it; any other mount forgets it.
  job->answer = fromSheet && !m_questionKey.empty() && m_questionKey == serverKey(job->uri);
  m_questionKey.clear();
  job->done = std::move(done);
  job->file = g_file_new_for_uri(job->uri.c_str());
  job->op = g_mount_operation_new();
  g_signal_connect(job->op, "ask-password",
                   G_CALLBACK(+[](GMountOperation* op, gchar*, gchar* defaultUser, gchar*, GAskPasswordFlags flags,
                                  gpointer data) {
                     auto* j = static_cast<MountJob*>(data);
                     const bool needPassword = (flags & G_ASK_PASSWORD_NEED_PASSWORD) != 0;
                     if (j->sentPassword || (needPassword && j->password.empty() &&
                                               (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) == 0)) {
                       j->needsPassword = true;
                       if (j->user.empty() && defaultUser != nullptr) {
                         j->user = defaultUser;
                       }
                       g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
                       return;
                     }
                     if (j->user.empty() && j->password.empty() && (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0) {
                       g_mount_operation_set_anonymous(op, TRUE);
                     } else {
                       g_mount_operation_set_username(op, j->user.empty() ? defaultUser : j->user.c_str());
                       g_mount_operation_set_password(op, j->password.c_str());
                       g_mount_operation_set_password_save(
                           op, j->remember ? G_PASSWORD_SAVE_PERMANENTLY : G_PASSWORD_SAVE_NEVER);
                     }
                     j->sentPassword = true;
                     g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
                   }),
                   job);
  g_signal_connect(job->op, "ask-question",
                   G_CALLBACK(+[](GMountOperation* op, gchar* message, GStrv, gpointer data) {
                     auto* j = static_cast<MountJob*>(data);
                     if (j->answer) {
                       g_mount_operation_set_choice(op, 0);
                       g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
                       return;
                     }
                     j->question = message != nullptr ? message : "";
                     g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
                   }),
                   job);
  g_file_mount_enclosing_volume(job->file, G_MOUNT_MOUNT_NONE, job->op, nullptr,
                                reinterpret_cast<GAsyncReadyCallback>(&NetworkPage::onMounted), job);
}

void NetworkPage::onMounted(void* source, void* result, void* data) {
  std::unique_ptr<MountJob> job(static_cast<MountJob*>(data));
  GError* error = nullptr;
  const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(source), static_cast<GAsyncResult*>(result), &error) ||
                  g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
  auto page = job->page.lock();
  std::string message;
  bool needsPassword = false;
  if (ok) {
    gchar* path = g_file_get_path(job->file);
    if (path == nullptr) {
      message = "The server is connected, but Files cannot open it: gvfs-fuse is not running.";
    } else if (page) {
      page->m_offline.erase(serverKey(job->uri));
      saveServer({.name = job->label, .uri = job->uri});
      page->m_saved = loadServers();
      if (page->m_actions.open) {
        page->m_actions.open(fs::path(path));
      }
    }
    g_free(path);
  } else if (!job->question.empty()) {
    // The sheet shows this text (an SSH key fingerprint): its next Connect is the user's explicit answer.
    if (page && job->fromSheet) {
      page->m_questionKey = serverKey(job->uri);
    }
    message = job->question;
  } else if (job->needsPassword) {
    needsPassword = true;
    message = job->sentPassword ? "The user name or password is not correct." : "This server needs a password.";
  } else if (offlineError(error)) {
    if (page) {
      page->m_offline.insert(serverKey(job->uri));
    }
    message = "The server does not answer. Check the address and the network.";
  } else {
    message = error != nullptr ? error->message : "The server could not be connected.";
  }
  kLog.debug("mount {}: {}", job->uri, message.empty() ? "ok" : message);
  g_clear_error(&error);
  if (page && page->m_actions.rebuild) {
    page->m_actions.rebuild();
  }
  if (job->done) {
    job->done(message, needsPassword);
  }
}

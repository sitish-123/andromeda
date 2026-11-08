#pragma once

#include "server_url.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Node;
struct ThisPcPlace;
typedef struct _GFileMonitor GFileMonitor;

// Network page (mockup files-v2-live.html §10, FILES-UI-SPEC §9) for FileBrowser::Listing::page: servers gvfs finds
// on the LAN (network:///, which reads mDNS through Avahi in gvfsd, outside our process), saved servers, and a
// "Connect to Server…" action. Mounts run through GIO (g_file_mount_enclosing_volume), so the share opens as a folder
// under the gvfs FUSE path. All GIO calls are async on the GLib loop that libdesk-app pumps; nothing blocks the UI.
class NetworkPage : public std::enable_shared_from_this<NetworkPage> {
public:
  struct Actions {
    std::function<void()> rebuild;                           // the page changed: build it again
    std::function<void(const std::filesystem::path&)> open;  // a mount is ready: show this folder
    std::function<void(const std::string& address)> connect; // open the Connect sheet with this address ("" = empty)
  };
  // `error` empty = mounted and opened. needsPassword: ask for one and call mount() again.
  using MountDone = std::function<void(const std::string& error, bool needsPassword)>;

  struct Server {
    std::string name;
    std::string uri;
    bool reconnect = false; // mount again at login (andromeda-files --reconnect)
  };

  explicit NetworkPage(Actions actions);
  ~NetworkPage();
  NetworkPage(const NetworkPage&) = delete;
  NetworkPage& operator=(const NetworkPage&) = delete;

  // Starts discovery and the network:/// monitor; call once, after make_shared.
  void start();
  [[nodiscard]] std::unique_ptr<Node> build(float scale);

  // Mounts `url` and opens it. remember: gvfs saves the password in the Secret Service keyring. A server that asks
  // a question (an unknown SSH host key) fails with the question as the error; the next mount() of the same address,
  // with no other mount between, answers it with the first choice. Only the Connect sheet calls mount(), so the user
  // has read the question in the sheet before it is accepted.
  void mount(const ServerUrl& url, const std::string& user, const std::string& password, bool remember, MountDone done,
             bool reconnect = false);
  // `andromeda-files --reconnect` at login: mounts every saved server marked reconnect with the keyring password,
  // then calls done(mounted, failed). No window, no questions: a server that asks one counts as failed.
  static void reconnectSaved(std::function<void(int mounted, int failed)> done);
  // A new Connect sheet opens: a question an earlier sheet showed is not accepted from this one.
  void forgetQuestion();
  // The last mount() failed with a question (its error is the question text): the sheet asks Accept.
  [[nodiscard]] bool questionPending() const noexcept { return !m_questionKey.empty(); }

  // $XDG_CONFIG_HOME/andromeda/files/servers.json: saved servers, newest first; no passwords.
  [[nodiscard]] static std::filesystem::path serversPath();
  [[nodiscard]] static std::vector<Server> loadServers();
  static void saveServer(const Server& server);

private:
  struct MountJob;
  void enumerate();
  void openPlace(const ThisPcPlace& place);
  void startMount(const ServerUrl& url, const std::string& user, const std::string& password, bool remember,
                  MountDone done, bool fromSheet, bool reconnect = false);
  static void onMounted(void* source, void* result, void* data);

  Actions m_actions;
  std::vector<Server> m_found;
  std::vector<Server> m_saved;
  std::set<std::string> m_offline; // server keys whose last mount found no server
  std::string m_questionKey;       // the server whose question the Connect sheet shows now
  bool m_looking = true;
  bool m_enumerating = false;
  GFileMonitor* m_monitor = nullptr;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

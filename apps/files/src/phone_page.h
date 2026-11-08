#pragma once

#include "link_share.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Node;

// Phone page and Import view (FILES-UI-SPEC §20, mockup files-v2.html §26) for a paired Link phone: hero card with
// status and storage, Recent Photos, the browse folders under ~/Phone/<name>, files received from the phone; Import
// copies photos into ~/Pictures/<date> through SavePhotoTo and remembers each one per phone.
class PhonePage : public std::enable_shared_from_this<PhonePage> {
public:
  struct Actions {
    std::function<void()> rebuild;
    std::function<void(const std::filesystem::path&)> open;      // a folder: show it; a file: reveal it
    std::function<void(const std::filesystem::path&)> quickLook; // a saved photo
    std::function<void()> sendFiles;                             // the host's file picker, then LinkShare::send
    std::function<void(std::shared_ptr<PhonePage>)> showImport;  // the host shows this page's Import listing
    std::function<void(const std::string& text)> toast;
    std::function<void(std::function<void(const std::filesystem::path&)>)> chooseFolder; // Import destination
  };

  PhonePage(LinkShare& link, std::string deviceId, Actions actions);

  [[nodiscard]] const std::string& deviceId() const noexcept { return m_deviceId; }
  // Asks the phone for its recent photos (and their thumbnails); keeps the old strip until the answer.
  void load();
  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  [[nodiscard]] std::unique_ptr<Node> buildImport(float scale);
  // Imports the picked photos, or every photo not imported yet when none is picked.
  void import(bool all);
  // Test hooks (actions phone.pick, phone.import).
  void pick(std::size_t index);
  [[nodiscard]] std::size_t photoCount() const noexcept { return m_photos.size(); }

  // "Pixel 9" -> ~/Phone/Pixel 9 when the browse mount shows it.
  [[nodiscard]] std::filesystem::path browseRoot() const;
  // Files in Downloads (one level) whose user.xdg.origin.url is link://<id>, newest first.
  [[nodiscard]] static std::vector<std::filesystem::path> received(const std::string& deviceId, std::size_t limit);

private:
  struct Shot {
    LinkShare::Photo photo;
    std::shared_ptr<std::vector<std::uint8_t>> png; // empty until the thumbnail arrives
  };

  [[nodiscard]] const LinkShare::Device* device() const { return m_link.device(m_deviceId); }
  [[nodiscard]] bool imported(const std::string& path) const { return m_imported.contains(path); }
  [[nodiscard]] std::size_t pendingCount() const;
  void loadImported();
  void saveImported() const;
  void later(std::function<void()> fn);
  void openPhoto(const LinkShare::Photo& photo);
  [[nodiscard]] std::filesystem::path importFolder() const; // the chosen folder, else Pictures/<phone>/<date>
  void importNext(std::vector<std::string> paths, std::filesystem::path folder, std::size_t done);

  [[nodiscard]] std::unique_ptr<Node> buildHero(float s) const;
  [[nodiscard]] std::unique_ptr<Node> buildState(float s, std::string title, std::string body, std::string button,
                                                 std::function<void()> onClick) const;
  [[nodiscard]] std::unique_ptr<Node> buildSection(float s, std::string title, std::string count,
                                                   std::vector<std::pair<std::string, std::function<void()>>> links) const;
  [[nodiscard]] std::unique_ptr<Node> buildTile(float s, std::size_t index, float size, bool importView);

  LinkShare& m_link;
  std::string m_deviceId;
  Actions m_actions;
  std::vector<Shot> m_photos;
  std::string m_photoError; // "denied": Photos Off; other: no answer
  bool m_loading = false;
  bool m_loaded = false; // an answer came while connected; cleared while away so a reconnect loads again
  std::set<std::string> m_imported; // phone paths already imported
  std::set<std::size_t> m_picked;   // indexes into m_photos (Import view)
  bool m_showAll = false;           // Import view: All instead of Not Imported
  bool m_importing = false;
  std::filesystem::path m_destination; // chosen in the Import bar; empty = Pictures/<phone>/<date>
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

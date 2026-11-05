#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

struct MusicTrack {
  std::filesystem::path path;
  std::string title;
  std::string artist;
  std::string albumArtist;
  std::string album;
  int track = 0;
  int disc = 0;
  std::string year;
  double duration = 0.0; // seconds
  std::int64_t mtimeNs = 0;
};

struct MusicAlbum {
  std::string title;
  std::string artist; // album artist, else the first track's artist
  std::string year;
  std::vector<std::size_t> tracks; // indices into MusicLibrary::tracks(), in disc/track order
};

struct MusicPlaylist {
  std::string name; // file stem
  std::filesystem::path file;
  std::vector<std::filesystem::path> entries; // absolute paths, in file order
};

// The music library: tags read with libavformat on a worker thread (no decoding), cached by path + mtime so a restart
// rescans only changed files. Playlists are the .m3u8 files in <music>/Playlists.
class MusicLibrary {
public:
  explicit MusicLibrary(std::filesystem::path root, std::filesystem::path cacheFile);
  ~MusicLibrary();

  MusicLibrary(const MusicLibrary&) = delete;
  MusicLibrary& operator=(const MusicLibrary&) = delete;

  // Starts a scan; `done` runs on the loop thread when the model is replaced.
  void scan(std::function<void()> done);
  [[nodiscard]] bool scanning() const noexcept { return m_scanning; }

  [[nodiscard]] const std::vector<MusicTrack>& tracks() const noexcept { return m_tracks; }
  [[nodiscard]] const std::vector<MusicAlbum>& albums() const noexcept { return m_albums; }
  [[nodiscard]] const std::vector<MusicPlaylist>& playlists() const noexcept { return m_playlists; }
  // `artist` empty: the first album with that title.
  [[nodiscard]] const MusicAlbum* findAlbum(std::string_view title, std::string_view artist = {}) const;
  [[nodiscard]] const MusicPlaylist* findPlaylist(std::string_view name) const;

  // Reads the tags of one file (headers only); false when it is not audio. `stop` interrupts a stuck read.
  [[nodiscard]] static bool readTags(const std::filesystem::path& path, MusicTrack& out,
                                     const std::stop_token* stop = nullptr);
  [[nodiscard]] static std::vector<MusicAlbum> groupAlbums(const std::vector<MusicTrack>& tracks);
  [[nodiscard]] static MusicPlaylist readPlaylist(const std::filesystem::path& file);

private:
  struct Result;

  std::filesystem::path m_root;
  std::filesystem::path m_cacheFile;
  std::vector<MusicTrack> m_tracks;
  std::vector<MusicAlbum> m_albums;
  std::vector<MusicPlaylist> m_playlists;
  std::atomic<bool> m_scanning = false;
  std::jthread m_worker;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

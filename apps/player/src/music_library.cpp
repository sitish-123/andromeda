#include "music_library.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <fstream>
#include <unistd.h>
#include <thread>
#include <format>
#include <glib.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("library");
  constexpr int kCacheVersion = 2; // 2: hasArt

  bool audioExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::string_view kExts[] = {".flac", ".mp3", ".ogg", ".opus", ".m4a", ".aac", ".wav", ".wv", ".ape", ".alac", ".aiff"};
    return std::ranges::find(kExts, ext) != std::end(kExts);
  }

  std::int64_t mtimeOf(const fs::path& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? st.st_mtim.tv_sec * 1'000'000'000LL + st.st_mtim.tv_nsec : 0;
  }

  // "3/12" -> 3
  int leadingNumber(const char* value) { return value != nullptr ? std::atoi(value) : 0; }

  nlohmann::json toJson(const MusicTrack& t) {
    return {{"path", t.path.string()}, {"title", t.title}, {"artist", t.artist}, {"albumArtist", t.albumArtist},
            {"album", t.album}, {"track", t.track}, {"disc", t.disc}, {"year", t.year}, {"duration", t.duration},
            {"mtime", t.mtimeNs}, {"art", t.hasArt}};
  }

  MusicTrack fromJson(const nlohmann::json& j) {
    MusicTrack t;
    t.path = j.value("path", "");
    t.title = j.value("title", "");
    t.artist = j.value("artist", "");
    t.albumArtist = j.value("albumArtist", "");
    t.album = j.value("album", "");
    t.track = j.value("track", 0);
    t.disc = j.value("disc", 0);
    t.year = j.value("year", "");
    t.duration = j.value("duration", 0.0);
    t.mtimeNs = j.value("mtime", std::int64_t{0});
    t.hasArt = j.value("art", false);
    return t;
  }
} // namespace

struct MusicLibrary::Result {
  std::vector<MusicTrack> tracks;
  std::vector<MusicAlbum> albums;
  std::vector<MusicPlaylist> playlists;
};

MusicLibrary::MusicLibrary(fs::path root, fs::path cacheFile) : m_root(std::move(root)), m_cacheFile(std::move(cacheFile)) {}

MusicLibrary::~MusicLibrary() {
  *m_alive = false;
  // jthread joins: a scan in progress finishes reading its current file first.
}

bool MusicLibrary::readTags(const fs::path& path, MusicTrack& out, const std::stop_token* stop) {
  AVFormatContext* ctx = avformat_alloc_context();
  if (ctx == nullptr) {
    return false;
  }
  // A quit interrupts a read stuck on a slow mount.
  if (stop != nullptr) {
    ctx->interrupt_callback = {
        .callback = [](void* data) { return static_cast<const std::stop_token*>(data)->stop_requested() ? 1 : 0; },
        .opaque = const_cast<std::stop_token*>(stop)};
  }
  AVDictionary* options = nullptr;
  // Tags and duration only: probe little and decode nothing beyond the headers.
  av_dict_set(&options, "probesize", "65536", 0);
  av_dict_set(&options, "analyzeduration", "0", 0);
  const int opened = avformat_open_input(&ctx, path.c_str(), nullptr, &options);
  av_dict_free(&options);
  if (opened != 0) {
    return false; // avformat_open_input frees ctx on failure
  }
  // Headers only: av_find_best_stream needs find_stream_info, so pick the first audio stream that is not cover art.
  int audio = -1;
  for (unsigned i = 0; i < ctx->nb_streams && audio < 0; ++i) {
    const AVStream* st = ctx->streams[i];
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      audio = static_cast<int>(i);
    }
  }
  if (audio < 0) {
    avformat_close_input(&ctx);
    return false;
  }
  const auto get = [&](const char* key) -> const char* {
    if (const AVDictionaryEntry* e = av_dict_get(ctx->metadata, key, nullptr, 0)) {
      return e->value;
    }
    if (const AVDictionaryEntry* e = av_dict_get(ctx->streams[audio]->metadata, key, nullptr, 0)) {
      return e->value;
    }
    return nullptr;
  };
  const auto text = [&](const char* key) { const char* v = get(key); return v != nullptr ? std::string(v) : std::string(); };
  out.path = path;
  out.title = text("title");
  if (out.title.empty()) {
    out.title = path.stem().string();
  }
  out.artist = text("artist");
  out.albumArtist = text("album_artist");
  out.album = text("album");
  out.track = leadingNumber(get("track"));
  out.disc = leadingNumber(get("disc"));
  out.year = text("date").substr(0, 4);
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    out.hasArt = out.hasArt || (ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
  }
  const AVStream* stream = ctx->streams[audio];
  out.duration = ctx->duration > 0                         ? static_cast<double>(ctx->duration) / AV_TIME_BASE
      : stream->duration > 0 && stream->time_base.den > 0 ? static_cast<double>(stream->duration) * av_q2d(stream->time_base)
                                                          : 0.0;
  out.mtimeNs = mtimeOf(path);
  avformat_close_input(&ctx);
  return true;
}

std::vector<MusicAlbum> MusicLibrary::groupAlbums(const std::vector<MusicTrack>& tracks) {
  // With an album artist the album is (album artist, album). Without one, the tracks of one album share its folder
  // (a compilation tags each track's own artist), so the key is (album, folder); a CD1/Disc 2 folder counts as its
  // parent.
  const auto folderOf = [](const fs::path& path) {
    fs::path dir = path.parent_path();
    std::string name = dir.filename().string();
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.starts_with("cd") || name.starts_with("disc") || name.starts_with("disk")) {
      dir = dir.parent_path();
    }
    return dir.string();
  };
  std::map<std::pair<std::string, std::string>, MusicAlbum> byKey;
  std::map<std::pair<std::string, std::string>, bool> mixedArtists;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const MusicTrack& t = tracks[i];
    if (t.album.empty()) {
      continue;
    }
    const auto key = !t.albumArtist.empty() ? std::pair{"artist:" + t.albumArtist, t.album}
                                            : std::pair{"folder:" + folderOf(t.path), t.album};
    MusicAlbum& album = byKey[key];
    if (album.tracks.empty()) {
      album.title = t.album;
      album.artist = !t.albumArtist.empty() ? t.albumArtist : t.artist;
      album.year = t.year;
    } else if (t.albumArtist.empty() && t.artist != album.artist) {
      mixedArtists[key] = true;
    }
    album.tracks.push_back(i);
  }
  for (const auto& [key, mixed] : mixedArtists) {
    byKey[key].artist = "Various Artists";
  }
  std::vector<MusicAlbum> out;
  for (auto& [key, album] : byKey) {
    std::ranges::sort(album.tracks, [&](std::size_t a, std::size_t b) {
      const MusicTrack& x = tracks[a];
      const MusicTrack& y = tracks[b];
      return std::tie(x.disc, x.track, x.path) < std::tie(y.disc, y.track, y.path);
    });
    out.push_back(std::move(album));
  }
  return out;
}

MusicPlaylist MusicLibrary::readPlaylist(const fs::path& file) {
  MusicPlaylist out{.name = file.stem().string(), .file = file, .entries = {}};
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.starts_with("\xEF\xBB\xBF")) {
      line.erase(0, 3);
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    // Paths are bytes on Linux, so a Latin-1 .m3u still names its files; URLs go to mpv as they are.
    if (line.starts_with("file://")) {
      if (gchar* file = g_filename_from_uri(line.c_str(), nullptr, nullptr)) {
        line = file;
        g_free(file);
      }
    } else if (line.find("://") != std::string::npos) {
      out.entries.emplace_back(line);
      continue;
    }
    fs::path entry = line;
    // Relative entries are relative to the playlist file, so playlists stay portable.
    out.entries.push_back(entry.is_absolute() ? entry.lexically_normal() : (file.parent_path() / entry).lexically_normal());
  }
  return out;
}

void MusicLibrary::findArt(MusicAlbum& album, const std::vector<MusicTrack>& tracks, const fs::path& artDir,
                           const std::stop_token* stop) {
  if (album.tracks.empty()) {
    return;
  }
  std::error_code ec;
  const fs::path folder = tracks[album.tracks.front()].path.parent_path();
  for (const char* name : {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "Cover.jpg", "Folder.jpg"}) {
    if (fs::is_regular_file(folder / name, ec)) {
      album.art = folder / name;
      return;
    }
  }
  const auto withArt = std::ranges::find_if(album.tracks, [&](std::size_t i) { return tracks[i].hasArt; });
  if (withArt == album.tracks.end()) {
    return;
  }
  const MusicTrack& source = tracks[*withArt];
  // Keyed by the source file, so albums that share a title never share a cover; a retagged file is newer than it.
  gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, source.path.c_str(), -1);
  const fs::path cached = artDir / (std::string(sum) + ".img");
  g_free(sum);
  struct stat cachedStat{};
  if (::stat(cached.c_str(), &cachedStat) == 0
      && cachedStat.st_mtim.tv_sec * 1'000'000'000LL + cachedStat.st_mtim.tv_nsec >= source.mtimeNs) {
    album.art = cached;
    return;
  }
  AVFormatContext* ctx = avformat_alloc_context();
  if (ctx == nullptr) {
    return;
  }
  if (stop != nullptr) {
    ctx->interrupt_callback = {
        .callback = [](void* data) { return static_cast<const std::stop_token*>(data)->stop_requested() ? 1 : 0; },
        .opaque = const_cast<std::stop_token*>(stop)};
  }
  AVDictionary* options = nullptr;
  // The cover is a header block (FLAC PICTURE, ID3 APIC, MP4 covr): nothing past the headers is needed.
  av_dict_set(&options, "probesize", "65536", 0);
  av_dict_set(&options, "analyzeduration", "0", 0);
  const int opened = avformat_open_input(&ctx, source.path.c_str(), nullptr, &options);
  av_dict_free(&options);
  if (opened != 0) {
    return;
  }
  // The cover packet is read with the headers; the bytes are a complete JPEG or PNG file.
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    const AVStream* st = ctx->streams[i];
    if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 && st->attached_pic.size > 0) {
      fs::create_directories(artDir, ec);
      // Per thread: the scan and the player's lookup for a file outside the library can write the same cover.
      const fs::path tmp = std::format("{}.{}.{}.tmp", cached.string(), ::getpid(),
                                       std::hash<std::thread::id>{}(std::this_thread::get_id()));
      std::ofstream out(tmp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(st->attached_pic.data), st->attached_pic.size);
      out.close();
      // A short write (disk full) must not become the cached cover.
      if (out && (fs::rename(tmp, cached, ec), !ec)) {
        album.art = cached;
      } else {
        fs::remove(tmp, ec);
      }
      if (album.art.empty() && fs::is_regular_file(cached, ec)) {
        album.art = cached; // the other writer won the rename
      }
      break;
    }
  }
  avformat_close_input(&ctx);
}

const MusicAlbum* MusicLibrary::findAlbum(std::string_view title, std::string_view artist) const {
  const auto it = std::ranges::find_if(m_albums, [&](const MusicAlbum& a) {
    return a.title == title && (artist.empty() || a.artist == artist);
  });
  return it != m_albums.end() ? &*it : nullptr;
}

const MusicPlaylist* MusicLibrary::findPlaylist(std::string_view name) const {
  const auto it = std::ranges::find_if(m_playlists, [&](const MusicPlaylist& p) { return p.name == name; });
  return it != m_playlists.end() ? &*it : nullptr;
}

void MusicLibrary::scan(std::function<void()> done) {
  if (m_scanning.exchange(true)) {
    return;
  }
  // Start from the previous result (or the cache) so unchanged files are not opened again.
  std::vector<MusicTrack> previous = m_tracks;
  m_worker = std::jthread([this, previous = std::move(previous), done = std::move(done),
                           alive = std::weak_ptr<bool>(m_alive)](std::stop_token stop) mutable {
    std::unordered_map<std::string, MusicTrack> known;
    if (previous.empty()) {
      std::ifstream in(m_cacheFile);
      const nlohmann::json cache = nlohmann::json::parse(in, nullptr, false);
      // A damaged or foreign cache only costs a full rescan.
      try {
        if (cache.is_object() && cache.value("version", 0) == kCacheVersion && cache.contains("tracks")
            && cache.at("tracks").is_array()) {
          for (const auto& j : cache.at("tracks")) {
            MusicTrack t = fromJson(j);
            known.emplace(t.path.string(), std::move(t));
          }
        }
      } catch (const nlohmann::json::exception& e) {
        kLog.warn("library cache ignored: {}", e.what());
        known.clear();
      }
    } else {
      for (MusicTrack& t : previous) {
        known.emplace(t.path.string(), std::move(t));
      }
    }

    auto result = std::make_shared<Result>();
    std::size_t opened = 0;
    std::error_code ec;
    // Own walk: a folder that fails to read (EIO on a network mount) is skipped, not the rest of the library.
    std::vector<fs::path> pending{m_root};
    while (!pending.empty() && !stop.stop_requested()) {
      const fs::path dir = std::move(pending.back());
      pending.pop_back();
      for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end && !stop.stop_requested(); it.increment(ec)) {
        const fs::path& path = it->path();
        std::error_code typeError;
        if (it->is_directory(typeError) && !it->is_symlink(typeError)) {
          pending.push_back(path);
          continue;
        }
        if (!it->is_regular_file(typeError) || !audioExtension(path)) {
          continue;
        }
        const std::int64_t mtime = mtimeOf(path);
        if (const auto k = known.find(path.string()); k != known.end() && k->second.mtimeNs == mtime) {
          result->tracks.push_back(k->second);
          continue;
        }
        MusicTrack track;
        ++opened;
        if (readTags(path, track, &stop)) {
          result->tracks.push_back(std::move(track));
        }
      }
      if (ec) {
        kLog.warn("library: skipped {}: {}", dir.string(), ec.message());
        ec.clear();
      }
    }
    // A stopped scan (quit) is partial: it must not replace the cache or the model.
    if (stop.stop_requested()) {
      return;
    }
    std::ranges::sort(result->tracks, {}, &MusicTrack::path);
    result->albums = groupAlbums(result->tracks);
    for (MusicAlbum& album : result->albums) {
      if (stop.stop_requested()) {
        return;
      }
      findArt(album, result->tracks, m_cacheFile.parent_path() / "art", &stop);
    }
    for (const auto& entry : fs::directory_iterator(m_root / "Playlists", ec)) {
      if (entry.path().extension() == ".m3u8" || entry.path().extension() == ".m3u") {
        result->playlists.push_back(readPlaylist(entry.path()));
      }
    }
    std::ranges::sort(result->playlists, {}, &MusicPlaylist::name);

    nlohmann::json cache{{"version", kCacheVersion}, {"tracks", nlohmann::json::array()}};
    for (const MusicTrack& t : result->tracks) {
      cache["tracks"].push_back(toJson(t));
    }
    fs::create_directories(m_cacheFile.parent_path(), ec);
    const fs::path tmp = m_cacheFile.string() + ".tmp";
    if (std::ofstream out(tmp); out) {
      out << cache.dump();
      out.close();
      fs::rename(tmp, m_cacheFile, ec);
    }
    kLog.debug("scan read {} files", opened);

    DeferredCall::callLater([this, alive, result, done = std::move(done)] {
      if (alive.expired()) {
        return;
      }
      m_tracks = std::move(result->tracks);
      m_albums = std::move(result->albums);
      m_playlists = std::move(result->playlists);
      m_scanning = false;
      kLog.info("library: {} albums, {} tracks, {} playlists", m_albums.size(), m_tracks.size(), m_playlists.size());
      if (done) {
        done();
      }
    });
  });
}

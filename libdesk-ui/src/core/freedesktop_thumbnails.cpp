#include "core/freedesktop_thumbnails.h"

#include "core/log.h"

#include <gio/gio.h>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace freedesktop_thumbnails {

  namespace {
    constexpr Logger kLog("thumbnails");
    constexpr std::uintmax_t kMaxPng = 32U << 20;
    const char* const kService = "org.freedesktop.thumbnails.Thumbnailer1";
    const char* const kPath = "/org/freedesktop/thumbnails/Thumbnailer1";

    struct Flavor {
      const char* name;
      int px;
    };
    constexpr std::array<Flavor, 4> kFlavors{{{"normal", 128}, {"large", 256}, {"x-large", 512}, {"xx-large", 1024}}};

    std::size_t flavorFor(int targetPx) {
      for (std::size_t i = 0; i < kFlavors.size(); ++i) {
        if (kFlavors[i].px >= targetPx) {
          return i;
        }
      }
      return kFlavors.size() - 1;
    }

    std::filesystem::path cacheRoot() { return std::filesystem::path(g_get_user_cache_dir()) / "thumbnails"; }

    struct Identity {
      std::string uri;
      std::string md5;
      long long mtime = 0;
    };

    std::optional<Identity> identify(const std::filesystem::path& path) {
      struct stat st{};
      std::error_code ec;
      // Normalized like g_file_get_uri, so other apps' entries match; symlinks stay as they are.
      const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
      if (ec || stat(absolute.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return std::nullopt;
      }
      char* uri = g_filename_to_uri(absolute.c_str(), nullptr, nullptr);
      if (uri == nullptr) {
        return std::nullopt;
      }
      char* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
      Identity id{uri, md5, static_cast<long long>(st.st_mtim.tv_sec)};
      g_free(md5);
      g_free(uri);
      return id;
    }

    std::uint32_t be32(const unsigned char* p) {
      return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
    }

    // True when the PNG's Thumb::URI and Thumb::MTime text chunks name this version of the file.
    bool matches(const std::filesystem::path& png, const Identity& id) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(png, ec);
      if (ec || size < 8 || size > kMaxPng) {
        return false;
      }
      std::vector<unsigned char> bytes(size);
      std::ifstream in(png, std::ios::binary);
      if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return false;
      }
      static constexpr std::array<unsigned char, 8> kSignature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      if (std::memcmp(bytes.data(), kSignature.data(), kSignature.size()) != 0) {
        return false;
      }
      std::optional<std::string_view> uri;
      std::optional<std::string_view> mtime;
      for (std::size_t at = 8; at + 12 <= size;) {
        const std::size_t length = be32(&bytes[at]);
        if (length > size - at - 12) {
          return false;
        }
        const std::string_view type(reinterpret_cast<const char*>(&bytes[at + 4]), 4);
        const std::string_view data(reinterpret_cast<const char*>(&bytes[at + 8]), length);
        if (type == "tEXt") {
          const auto nul = data.find('\0');
          if (nul != std::string_view::npos) {
            const auto key = data.substr(0, nul);
            if (key == "Thumb::URI") {
              uri = data.substr(nul + 1);
            } else if (key == "Thumb::MTime") {
              mtime = data.substr(nul + 1);
            }
          }
        } else if (type == "IEND") {
          break;
        }
        at += 12 + length;
      }
      if (!uri || !mtime || *uri != id.uri) {
        return false;
      }
      // Some writers store fractional seconds; the standard compares whole seconds.
      const auto whole = mtime->substr(0, mtime->find('.'));
      return whole == std::to_string(id.mtime);
    }
  } // namespace

  std::optional<std::filesystem::path> lookup(const std::filesystem::path& path, int targetPx, bool allowSmaller) {
    const auto id = identify(path);
    if (!id) {
      return std::nullopt;
    }
    const std::filesystem::path root = cacheRoot();
    const std::size_t first = flavorFor(targetPx);
    // The fitting size, then larger ones (scaled down), then smaller ones (the largest there is).
    std::vector<std::size_t> order;
    for (std::size_t i = first; i < kFlavors.size(); ++i) {
      order.push_back(i);
    }
    for (std::size_t i = first; allowSmaller && i-- > 0;) {
      order.push_back(i);
    }
    for (const std::size_t i : order) {
      const auto png = root / kFlavors[i].name / (id->md5 + ".png");
      if (matches(png, *id)) {
        return png;
      }
    }
    return std::nullopt;
  }

  bool knownFailure(const std::filesystem::path& path) {
    const auto id = identify(path);
    std::error_code ec;
    if (!id) {
      return false;
    }
    for (const auto& app : std::filesystem::directory_iterator(cacheRoot() / "fail", ec)) {
      if (matches(app.path() / (id->md5 + ".png"), *id)) {
        return true;
      }
    }
    return false;
  }

  bool generate(const std::filesystem::path& path, int targetPx, std::chrono::milliseconds timeout) {
    const auto id = identify(path);
    if (!id) {
      return false;
    }
    gboolean uncertain = FALSE;
    char* type = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain);
    char* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
    const std::string mimeType = mime != nullptr ? mime : "application/octet-stream";
    g_free(mime);
    g_free(type);

    try {
      auto connection = sdbus::createSessionBusConnection();
      auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{kService}, sdbus::ObjectPath{kPath});
      const sdbus::InterfaceName iface{kService};
      std::uint32_t handle = 0;
      bool finished = false;
      bool ready = false;
      // Signals queue on the connection until processPendingEvent(), so the handle is known by then.
      proxy->uponSignal(sdbus::SignalName{"Ready"})
          .onInterface(iface)
          .call([&](std::uint32_t h, const std::vector<std::string>&) { ready = ready || h == handle; });
      proxy->uponSignal(sdbus::SignalName{"Finished"})
          .onInterface(iface)
          .call([&](std::uint32_t h) { finished = finished || h == handle; });
      proxy->callMethod("Queue")
          .onInterface(iface)
          .withArguments(std::vector<std::string>{id->uri}, std::vector<std::string>{mimeType},
                         std::string(kFlavors[flavorFor(targetPx)].name), std::string("background"), std::uint32_t{0})
          .storeResultsTo(handle);

      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (!finished) {
        while (connection->processPendingEvent()) {
        }
        if (finished) {
          break;
        }
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
          kLog.debug("thumbnailer timed out for {}", path.string());
          break;
        }
        const auto data = connection->getEventLoopPollData();
        pollfd fds[2] = {{data.fd, data.events, 0}, {data.eventFd, POLLIN, 0}};
        // -1 means sd-bus has no timer of its own; the deadline still bounds the wait.
        const int busTimeout = data.getPollTimeout();
        const int wait = static_cast<int>(left.count());
        (void)poll(fds, 2, busTimeout < 0 ? wait : std::min(wait, busTimeout));
      }
      return ready;
    } catch (const sdbus::Error& e) {
      // No session bus or no thumbnailer: the caller falls back to its own decoder.
      kLog.debug("thumbnailer unavailable: {}", e.getMessage());
      return false;
    }
  }

} // namespace freedesktop_thumbnails

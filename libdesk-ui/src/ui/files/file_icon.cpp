#include "ui/files/file_icon.h"

#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/files/resource_paths.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <dirent.h>
#include <glib.h>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace {
  // The art is drawn on a 64 grid (mockup .fold / .pg); every offset below is in those units.
  constexpr float kGrid = 64.0F;
  // Art geometry in 64-grid units, scaled to the icon size.
  constexpr float grid(int units, float u) { return static_cast<float>(units) * u; }

  // The drawn page per kind (mockup .pg variants): text lines, a PDF with a chart, a spreadsheet grid, a slide card,
  // a zip with its zipper.
  enum class Art : std::uint8_t { Doc, Text, Pdf, Sheet, Slide, Zip };
  struct Kind {
    const char* badge;   // "" = no badge
    std::uint32_t color; // badge colour
    Art art = Art::Doc;
  };

  std::string extensionOf(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
      return {};
    }
    std::string ext = name.substr(dot + 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
  }

  Kind pageKind(const FileInfo& info) {
    const std::string ext = extensionOf(info.displayName);
    static const std::map<std::string, Kind> kKinds{
        {"pdf", {"PDF", 0xd93025, Art::Pdf}},     {"ods", {"ODS", 0x1a7f37, Art::Sheet}},
        {"xlsx", {"XLSX", 0x1a7f37, Art::Sheet}}, {"csv", {"CSV", 0x1a7f37, Art::Sheet}},
        {"odp", {"ODP", 0xe0457b, Art::Slide}},   {"pptx", {"PPTX", 0xe0457b, Art::Slide}},
        {"key", {"KEY", 0xe0457b, Art::Slide}},   {"doc", {"DOC", 0x2a6fd8}},
        {"docx", {"DOCX", 0x2a6fd8}},             {"odt", {"ODT", 0x2a6fd8}},
        {"rtf", {"RTF", 0x2a6fd8}},               {"md", {"MD", 0x5b5b66, Art::Text}},
        {"txt", {"TXT", 0x5b5b66, Art::Text}},    {"json", {"JSON", 0x5b5b66, Art::Text}},
        {"zip", {"ZIP", 0xb45309, Art::Zip}},     {"7z", {"7Z", 0xb45309, Art::Zip}},
        {"tar", {"TAR", 0xb45309, Art::Zip}},     {"gz", {"GZ", 0xb45309, Art::Zip}},
        {"xz", {"XZ", 0xb45309, Art::Zip}},       {"zst", {"ZST", 0xb45309, Art::Zip}},
        {"rar", {"RAR", 0xb45309, Art::Zip}},     {"iso", {"ISO", 0x6b7280}},
        {"cpp", {"C++", 0x5147e5, Art::Text}},    {"h", {"H", 0x5147e5, Art::Text}},
        {"rs", {"RS", 0xb7410e, Art::Text}},      {"py", {"PY", 0x3776ab, Art::Text}},
        {"sh", {"SH", 0x1a7f37, Art::Text}},      {"html", {"HTML", 0xe34c26, Art::Text}},
    };
    if (auto it = kKinds.find(ext); it != kKinds.end()) {
      return it->second;
    }
    return {"", 0x5b5b66, Art::Doc};
  }

  // A user folder's emblem (mockup place()): XDG dirs and home.
  const char* emblemFor(const fs::path& path) {
    static const std::array<std::pair<GUserDirectory, const char*>, 6> kDirs{{
        {G_USER_DIRECTORY_DESKTOP, "desktop"},     {G_USER_DIRECTORY_DOCUMENTS, "documents"},
        {G_USER_DIRECTORY_DOWNLOAD, "downloads"},  {G_USER_DIRECTORY_PICTURES, "pictures"},
        {G_USER_DIRECTORY_MUSIC, "music"},         {G_USER_DIRECTORY_VIDEOS, "videos"},
    }};
    const fs::path home = g_get_home_dir();
    for (const auto& [dir, emblem] : kDirs) {
      const char* p = g_get_user_special_dir(dir);
      if (p != nullptr && fs::path(p) != home && fs::path(p) == path) {
        return emblem;
      }
    }
    return nullptr;
  }

  // One texture per (asset, pixel size) for every icon on screen: SVG rasterising stays off the scroll path.
  TextureHandle iconTexture(Renderer& renderer, const std::string& asset, int px) {
    static std::map<std::tuple<TextureManager*, std::string, int>, TextureHandle> cache;
    TextureManager* tm = &renderer.textureManager();
    const auto key = std::tuple{tm, asset, px};
    if (auto it = cache.find(key); it != cache.end()) {
      return it->second;
    }
    const TextureHandle handle = tm->loadFromFile(paths::assetPath("files-icons/" + asset + ".svg").string(), px);
    cache.emplace(key, handle);
    return handle;
  }

  // Folder peeks: up to 3 visible entries per folder, read on a worker, cached by path + mtime.
  struct PeekEntry {
    std::int64_t mtimeNs = 0;
    std::vector<FileIcon::Peek> peeks;
    bool ready = false;
  };
  std::mutex g_peekMutex;
  std::map<std::string, PeekEntry> g_peeks;
  std::map<int, std::function<void()>> g_peekListeners;
  int g_nextPeekListener = 1;

  // One worker reads every folder peek in order: a fast scroll through 1000 folders queues work, not 1000 threads.
  void enqueuePeek(std::function<void()> job) {
    static std::mutex mutex;
    static std::condition_variable wake;
    static std::deque<std::function<void()>> queue;
    static bool started = false;
    std::scoped_lock lock(mutex);
    queue.push_back(std::move(job));
    if (!started) {
      started = true;
      std::thread([] {
        while (true) {
          std::function<void()> next;
          {
            std::unique_lock wait(mutex);
            wake.wait(wait, [] { return !queue.empty(); });
            next = std::move(queue.front());
            queue.pop_front();
          }
          next();
        }
      }).detach();
    }
    wake.notify_one();
  }

  FileIcon::Peek peekKind(const std::string& name) {
    const std::string ext = extensionOf(name);
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "heic" || ext == "gif" || ext == "mov"
        || ext == "mp4" || ext == "mkv" || ext == "avif" || ext == "jxl") {
      return FileIcon::Peek::Image;
    }
    if (ext == "ods" || ext == "xlsx" || ext == "csv") {
      return FileIcon::Peek::Sheet;
    }
    return ext == "pdf" ? FileIcon::Peek::Pdf : FileIcon::Peek::Page;
  }

  std::vector<FileIcon::Peek> peekFolder(const std::string& path, std::int64_t mtimeNs) {
    {
      std::scoped_lock lock(g_peekMutex);
      if (auto it = g_peeks.find(path); it != g_peeks.end() && it->second.mtimeNs == mtimeNs) {
        return it->second.peeks;
      }
      g_peeks[path] = PeekEntry{.mtimeNs = mtimeNs};
    }
    enqueuePeek([path, mtimeNs] {
      std::vector<FileIcon::Peek> peeks;
      if (DIR* dir = ::opendir(path.c_str())) {
        // Bounded: a huge or slow folder costs at most this many records.
        constexpr int kScanLimit = 64;
        int scanned = 0;
        while (const dirent* e = ::readdir(dir)) {
          if (++scanned > kScanLimit || peeks.size() == 3) {
            break;
          }
          if (e->d_name[0] == '.' || e->d_type == DT_DIR) {
            continue;
          }
          peeks.push_back(peekKind(e->d_name));
        }
        ::closedir(dir);
      }
      bool changed = false;
      {
        std::scoped_lock lock(g_peekMutex);
        PeekEntry& entry = g_peeks[path];
        if (entry.mtimeNs == mtimeNs) {
          changed = !peeks.empty();
          entry.peeks = std::move(peeks);
          entry.ready = true;
        }
      }
      if (changed) {
        DeferredCall::callLater([] {
          const auto listeners = g_peekListeners;
          for (const auto& [id, fn] : listeners) {
            fn();
          }
        });
      }
    });
    return {};
  }
} // namespace

int FileIcon::addPeekListener(std::function<void()> changed) {
  const int id = g_nextPeekListener++;
  g_peekListeners.emplace(id, std::move(changed));
  return id;
}

void FileIcon::removePeekListener(int id) { g_peekListeners.erase(id); }

FileIcon::FileIcon(float size) : m_size(size) {
  m_back = static_cast<Image*>(addChild(std::make_unique<Image>()));
  for (int i = 0; i < 3; ++i) {
    Box* mini = static_cast<Box*>(addChild(ui::box({})));
    m_minis.push_back(mini);
    std::array<Box*, 6> lines{};
    for (Box*& line : lines) {
      line = static_cast<Box*>(mini->addChild(ui::box({})));
    }
    m_miniLines.push_back(lines);
  }
  m_front = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_emblem = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_page = static_cast<Box*>(addChild(ui::box({})));
  for (int i = 0; i < 10; ++i) {
    m_lines.push_back(static_cast<Box*>(m_page->addChild(ui::box({}))));
  }
  for (int i = 0; i < 12; ++i) {
    m_art.push_back(static_cast<Box*>(m_page->addChild(ui::box({}))));
  }
  m_badge = static_cast<Box*>(m_page->addChild(ui::box({})));
  m_badgeText = static_cast<Label*>(m_badge->addChild(ui::label({.fontWeight = FontWeight::Bold, .maxLines = 1})));
  m_thumb = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_play = static_cast<Box*>(addChild(ui::box({})));
  m_playGlyph = static_cast<Glyph*>(m_play->addChild(std::make_unique<Glyph>()));
  m_playGlyph->setGlyph("media-play");
  m_playGlyph->setColor(rgbHex(0xffffff));
  setSize(size, size);
}

FileIcon::~FileIcon() { releaseThumb(); }

void FileIcon::releaseThumb() {
  if (m_thumbs != nullptr && !m_thumbPath.empty()) {
    m_thumbs->release(m_thumbPath, m_thumbPx);
  }
  m_thumbPath.clear();
}

bool FileIcon::bindThumb(Renderer& renderer, const fs::path& path, ThumbnailService* thumbnails, bool video) {
  if (thumbnails == nullptr) {
    return false;
  }
  const std::string key = path.string();
  const int px = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  if (key != m_thumbPath || thumbnails != m_thumbs || px != m_thumbPx) {
    releaseThumb();
    m_thumbs = thumbnails;
    m_thumbPath = key;
    m_thumbPx = px;
    (void)thumbnails->acquire(key, px);
  }
  const TextureHandle handle = thumbnails->peek(key, px);
  if (handle.id == 0 || handle.width <= 0 || handle.height <= 0) {
    return false;
  }
  // mockup .thumb: the picture at its own aspect inside a 64 x 52 box (a square photo no heavier than its
  // neighbours), r 4, hairline.
  const float u = m_size / kGrid;
  const float boxH = grid(52, u);
  const float aspect = static_cast<float>(handle.width) / static_cast<float>(handle.height);
  float w = m_size;
  float h = std::round(m_size / aspect);
  if (h > boxH) {
    h = boxH;
    w = std::round(boxH * aspect);
  }
  m_thumb->setExternalTexture(renderer, handle);
  m_thumb->setSize(w, h);
  m_thumb->setPosition(std::round((m_size - w) / 2.0F), std::round(m_size - h - grid(2, u)));
  m_thumb->setRadius(grid(4, u));
  m_thumb->setBorder(rgbaHex(0x00000033), grid(1, u) / 2.0F);
  m_thumb->setVisible(true);
  if (video) {
    const float d = grid(20, u);
    m_play->setSize(d, d);
    m_play->setRadius(d / 2.0F);
    layoutPlay(d);
    m_play->setFill(rgbaHex(0x00000080));
    m_play->setPosition(std::round((m_size - d) / 2.0F), std::round(m_size - h / 2.0F - d / 2.0F - grid(2, u)));
    m_play->setVisible(true);
  }
  return true;
}

void FileIcon::hideAll() {
  for (Node* n : std::initializer_list<Node*>{m_back, m_front, m_emblem, m_page, m_thumb, m_play}) {
    n->setVisible(false);
  }
  for (Box* b : m_minis) {
    b->setVisible(false);
  }
}

void FileIcon::doLayout(Renderer& renderer) {
  if (m_boundInfo.has_value() && renderer.renderScale() != m_boundScale) {
    const FileInfo info = *m_boundInfo;
    bind(renderer, info, m_boundPath, m_boundThumbs);
  }
  Node::doLayout(renderer);
}

void FileIcon::bind(Renderer& renderer, const FileInfo& info, const fs::path& path, ThumbnailService* thumbnails) {
  m_boundInfo = info;
  m_boundPath = path;
  m_boundThumbs = thumbnails;
  m_boundScale = renderer.renderScale();
  setSize(m_size, m_size);
  hideAll();
  if (info.isDir) {
    releaseThumb();
    bindFolder(renderer, path, info.mtimeNs);
    return;
  }
  const std::string& type = info.contentType;
  const bool image = type.starts_with("image/");
  const bool video = type.starts_with("video/");
  if ((image || video || type == "application/pdf") && bindThumb(renderer, path, thumbnails, video)) {
    return;
  }
  if (!image && !video && type != "application/pdf") {
    releaseThumb();
  }
  if (image) {
    bindArt(renderer, "pictures", 0.875F);
  } else if (video) {
    bindVideoCard();
  } else if (type.starts_with("audio/")) {
    bindArt(renderer, "music", 0.875F);
  } else {
    bindPage(renderer, info);
  }
}

void FileIcon::bindArt(Renderer& renderer, const char* asset, float fraction) {
  const float side = std::round(m_size * fraction);
  const int px = static_cast<int>(std::ceil(side * renderer.renderScale()));
  m_back->setExternalTexture(renderer, iconTexture(renderer, asset, px));
  m_back->setSize(side, side);
  m_back->setPosition(std::round((m_size - side) / 2.0F), std::round((m_size - side) / 2.0F));
  m_back->setVisible(true);
}

void FileIcon::layoutPlay(float d) {
  // mockup .play: an 8 px triangle in the 20 px circle.
  const float g = d * 0.4F;
  m_playGlyph->setGlyphSize(g);
  m_playGlyph->setSize(g, g);
  m_playGlyph->setPosition(std::round((d - g) / 2.0F), std::round((d - g) / 2.0F));
}

void FileIcon::bindVideoCard() {
  // mockup .thumb for a video with no frame yet: a dark 64 x 40 card, r 4, hairline, the play circle in its middle.
  const float u = m_size / kGrid;
  const float h = grid(40, u);
  const float top = std::round(m_size - h - grid(2, u));
  for (Box* b : m_lines) {
    b->setVisible(false);
  }
  for (Box* b : m_art) {
    b->setVisible(false);
  }
  m_badge->setVisible(false);
  m_page->setPosition(0.0F, top);
  m_page->setSize(m_size, h);
  m_page->setRadius(grid(4, u));
  m_page->setFill(rgbHex(0x2b2f3a));
  m_page->setBorder(rgbaHex(0x00000033), grid(1, u) / 2.0F);
  m_page->setVisible(true);
  const float d = grid(20, u);
  m_play->setSize(d, d);
  m_play->setRadius(d / 2.0F);
  layoutPlay(d);
  m_play->setFill(rgbaHex(0xffffff40));
  m_play->setPosition(std::round((m_size - d) / 2.0F), std::round(top + (h - d) / 2.0F));
  m_play->setVisible(true);
}

void FileIcon::bindFolder(Renderer& renderer, const fs::path& path, std::int64_t mtimeNs) {
  const float u = m_size / kGrid;
  const int px = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  const char* emblem = emblemFor(path);
  if (emblem != nullptr && m_size < kGrid / 2.0F) {
    // List and column rows (mockup §2-3): the place art alone, as in the sidebar.
    m_back->setExternalTexture(renderer, iconTexture(renderer, emblem, px));
    m_back->setPosition(0.0F, 0.0F);
    m_back->setSize(m_size, m_size);
    m_back->setVisible(true);
    return;
  }
  if (emblem != nullptr) {
    // place(): the plain folder and its emblem at the lower right (mockup .place).
    m_back->setExternalTexture(renderer, iconTexture(renderer, "folder", px));
    m_back->setPosition(0.0F, 0.0F);
    m_back->setSize(m_size, m_size);
    m_back->setVisible(true);
    const float e = std::round(m_size / 2.0F);
    m_emblem->setExternalTexture(renderer, iconTexture(renderer, emblem, static_cast<int>(std::ceil(e * renderer.renderScale()))));
    m_emblem->setSize(e, e);
    m_emblem->setPosition(m_size - e, m_size - e - std::round(4.0F * u));
    m_emblem->setVisible(true);
    return;
  }
  m_back->setExternalTexture(renderer, iconTexture(renderer, "folder-back", px));
  m_back->setPosition(0.0F, 0.0F);
  m_back->setSize(m_size, m_size);
  m_back->setVisible(true);
  // Contents ride inside: the rear item highest, only their tops show above the front flap (mockup fold()).
  const auto peeks = m_size >= kGrid / 2.0F ? peekFolder(path.string(), mtimeNs) : std::vector<Peek>{};
  static constexpr std::array<std::array<std::array<float, 3>, 3>, 4> kPos{{
      {},
      {{{10, 12, 0}}},
      {{{14, 10, 4}, {8, 14, -3}}},
      {{{14, 6, 5}, {6, 10, -4}, {10, 14, 0}}},
  }};
  const std::size_t n = std::min<std::size_t>(peeks.size(), 3);
  for (std::size_t i = 0; i < n; ++i) {
    Box* mini = m_minis[i];
    const bool image = peeks[i] == Peek::Image;
    const float w = (image ? 44.0F : 36.0F) * u;
    const float h = (image ? 32.0F : 44.0F) * u;
    const auto& [x, y, deg] = kPos[n][i];
    mini->setPosition(std::round((x + (image ? 0.0F : 4.0F)) * u), std::round(y * u));
    mini->setSize(w, h);
    mini->setRadius(grid(3, u));
    mini->setFill(image ? rgbHex(0x4a4ab0) : rgbHex(0xffffff));
    mini->setBorder(rgbaHex(0x00000033), 0.5F * u);
    // mockup mini(): a page = coloured head (red for a PDF, blue otherwise) over 5 grey lines; a sheet = green
    // header over 5 dashed rows; a picture shows its own colour.
    const bool sheet = peeks[i] == Peek::Sheet;
    for (std::size_t k = 0; k < m_miniLines[i].size(); ++k) {
      Box* line = m_miniLines[i][k];
      line->setVisible(!image);
      if (image) {
        continue;
      }
      const bool head = k == 0;
      if (sheet) {
        line->setPosition(head ? 0.0F : grid(4, u), head ? 0.0F : std::round((8.0F + 6.0F * static_cast<float>(k - 1)) * u));
        line->setSize(head ? w : w - grid(8, u), head ? grid(6, u) : grid(4, u));
        line->setRadii(head ? Radii(grid(3, u), grid(3, u), 0.0F, 0.0F) : Radii(0.0F));
        line->setFill(rgbHex(head ? 0x1a7f37 : 0xd5dae2));
      } else {
        line->setPosition(grid(6, u), std::round((head ? 4.0F : 9.0F + 4.0F * static_cast<float>(k - 1)) * u));
        line->setSize(head ? std::round(w * 0.6F - grid(6, u)) : w - grid(12, u), grid(2, u));
        line->setRadii(Radii(0.0F));
        line->setFill(rgbHex(head ? (peeks[i] == Peek::Pdf ? 0xd93025 : 0x2a6fd8) : 0xc3c8d2));
      }
    }
    mini->setTransformOrigin(w / 2.0F, h / 2.0F);
    mini->setRotation(deg * static_cast<float>(M_PI) / 180.0F);
    mini->setVisible(true);
  }
  m_front->setExternalTexture(renderer, iconTexture(renderer, "folder-front", px));
  m_front->setPosition(0.0F, 0.0F);
  m_front->setSize(m_size, m_size);
  m_front->setVisible(true);
}

void FileIcon::bindPage(Renderer& renderer, const FileInfo& info) {
  const float u = m_size / kGrid;
  const Kind kind = pageKind(info);
  const bool small = m_size < kGrid / 2.0F;
  for (Box* b : m_lines) {
    b->setVisible(false);
  }
  for (Box* b : m_art) {
    b->setVisible(false);
  }
  const auto place = [u](Box* b, float x, float y, float w, float h, std::uint32_t rgb, float radius = 0.0F) {
    b->setVisible(true);
    b->setPosition(x * u, y * u);
    b->setSize(w * u, h * u);
    b->setRadius(radius * u);
    b->setFill(rgbHex(rgb));
  };
  m_page->setVisible(true);
  m_page->setBorder(rgbaHex(0x00000033), 0.5F * u);

  if (kind.art == Art::Slide) {
    // mockup .slide: a 64 x 40 card (the pink to orange wash as its middle colour), two white title bars.
    m_page->setPosition(0.0F, std::round(m_size - grid(52, u)));
    m_page->setSize(m_size, grid(40, u));
    m_page->setRadius(grid(4, u));
    m_page->setFill(rgbHex(0xef725f));
    if (!small) {
      place(m_art[0], 6, 6, 31, 4, 0xffffff, 2);
      place(m_art[1], 6, 14, 21, 2, 0xf8d4cc, 1);
    }
    m_badge->setVisible(false);
    return;
  }

  // mockup .pg: a 48 x 60 page, hairline, badge bottom left.
  const float w = 48.0F;
  const float h = 60.0F;
  m_page->setPosition(std::round((m_size - w * u) / 2.0F), std::round(m_size - h * u - 2.0F * u));
  m_page->setSize(w * u, h * u);
  m_page->setRadius(grid(4, u));
  m_page->setFill(rgbHex(kind.art == Art::Zip ? 0xefc88a : 0xffffff));
  if (!small) {
    switch (kind.art) {
    case Art::Sheet: {
      // .pg.xls: a green header row over a 4 x 7 grid of 6 px cells with hairlines.
      place(m_art[0], 0, 0, w, 6, 0x1a7f37);
      m_art[0]->setRadii(Radii(grid(4, u), grid(4, u), 0.0F, 0.0F));
      for (int i = 0; i < 3; ++i) {
        place(m_art[1 + i], 12.0F * static_cast<float>(i + 1), 6, 0.5F, 42, 0xd5dae2);
      }
      for (int i = 0; i < 8; ++i) {
        place(m_art[4 + i], 0, 6.0F * static_cast<float>(i + 2) - 0.5F, w, 0.5F, 0xd5dae2);
      }
      break;
    }
    case Art::Zip: {
      // A zipper down the middle: a tan strip with dark teeth every 4 px.
      place(m_art[0], 21, 6, 6, 40, 0xc99b55);
      for (int i = 0; i < 10; ++i) {
        place(m_art[1 + i], 21, 6.0F + 4.0F * static_cast<float>(i), 6, 2, 0x7a5a2a);
      }
      break;
    }
    default: {
      // Header bar, then 2 px lines on a 4 px pitch; a PDF stops after 3 lines for its chart.
      place(m_lines[0], 6, 6, kind.art == Art::Text ? 18 : 25, 4, 0x3a3f4b, 1);
      const int lines = kind.art == Art::Pdf ? 3 : 9;
      for (int i = 0; i < lines && i + 1 < static_cast<int>(m_lines.size()); ++i) {
        static constexpr std::array<float, 8> kWidths{36, 32, 34, 25, 31, 22, 32, 27}; // mockup lines() 100 90 95 70 %
        place(m_lines[1 + i], 6, 14.0F + 4.0F * static_cast<float>(i), kWidths[static_cast<std::size_t>(i) % kWidths.size()], 2, 0xc3c8d2, 1);
      }
      if (kind.art == Art::Pdf) {
        const std::array<float, 4> bars{5, 9, 7, 12};
        for (std::size_t i = 0; i < bars.size(); ++i) {
          place(m_art[i], 6.0F + 6.0F * static_cast<float>(i), 40.0F - bars[i], 4, bars[i], 0x5147e5, 1);
        }
      }
      break;
    }
    }
  }
  if (small) {
    // mockup list rows (20 px): three grey lines and a band in the kind colour along the bottom.
    for (int i = 0; i < 3; ++i) {
      place(m_lines[i], 10, 12.0F + 10.0F * static_cast<float>(i), i == 2 ? 20 : 28, 4, 0x9a9faa, 2);
    }
    place(m_art[0], 0, h - 12, w, 12, kind.color);
    m_art[0]->setRadii(Radii(0.0F, 0.0F, grid(4, u), grid(4, u)));
  }
  const bool badge = kind.badge[0] != '\0' && !small;
  m_badge->setVisible(badge);
  if (badge) {
    m_badgeText->setText(kind.badge);
    m_badgeText->setFontSize(grid(8, u));
    m_badgeText->setColor(rgbHex(0xffffff));
    m_badgeText->layout(renderer);
    const float bw = m_badgeText->width() + 8.0F * u;
    const float bh = 12.0F * u;
    m_badge->setSize(bw, bh);
    m_badge->setPosition(4.0F * u, h * u - bh - 4.0F * u);
    m_badge->setRadius(grid(2, u));
    m_badge->setFill(rgbHex(kind.color));
    m_badgeText->setPosition(4.0F * u, (bh - m_badgeText->height()) / 2.0F);
  }
}

PlaceIcon::PlaceIcon(std::string asset, float size)
    : m_asset(std::move(asset)), m_size(size) {
  m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_image->setSize(size, size);
  setSize(size, size);
}

void PlaceIcon::doLayout(Renderer& renderer) {
  const int px = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  if (px != m_loadedPx) {
    m_image->setExternalTexture(renderer, iconTexture(renderer, m_asset, px));
    m_loadedPx = px;
  }
  m_image->setSize(m_size, m_size);
  m_image->layout(renderer);
}

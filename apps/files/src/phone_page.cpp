#include "phone_page.h"

#include "cursor-shape-v1-client-protocol.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "drives.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "core/process/process.h"
#include "render/scene/input_area.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sys/xattr.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-phone");
  constexpr std::uint16_t kRecent = 24;   // Recent Photos on the page
  constexpr std::uint16_t kImportAll = 64; // RecentPhotos takes 1 to 64
  constexpr float kTile = 88.0F;
  constexpr float kImportTile = 72.0F;
  constexpr float kStorageBar = 280.0F;

  std::unique_ptr<Label> text(std::string value, float size, ColorRole role, FontWeight weight = FontWeight::Medium,
                              float grow = 0.0F) {
    return ui::label({.text = std::move(value), .fontSize = size, .fontWeight = weight, .color = colorSpecFromRole(role),
                      .maxLines = 1, .ellipsize = TextEllipsize::End, .flexGrow = grow});
  }

  fs::path stateFile() { return fs::path(g_get_user_state_dir()) / "andromeda" / "files" / "phone-imports.json"; }

  std::string ago(std::int64_t when) {
    if (when <= 0) {
      return "Away";
    }
    const auto secs = std::max<std::int64_t>(0, std::time(nullptr) - when);
    if (secs < 3600) {
      return std::format("Away · last seen {} min ago", std::max<std::int64_t>(1, secs / 60));
    }
    if (secs < 86400) {
      return std::format("Away · last seen {} h ago", secs / 3600);
    }
    return std::format("Away · last seen {} days ago", secs / 86400);
  }

  std::string dayTitle(std::uint64_t time) {
    const std::time_t t = static_cast<std::time_t>(time);
    std::tm day{};
    localtime_r(&t, &day);
    const std::time_t now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    if (day.tm_year == today.tm_year && day.tm_yday == today.tm_yday) {
      return "Today";
    }
    if (day.tm_year == today.tm_year && day.tm_yday + 1 == today.tm_yday) {
      return "Yesterday";
    }
    char buf[32];
    std::strftime(buf, sizeof(buf), "%e %B %Y", &day);
    return std::string(buf[0] == ' ' ? buf + 1 : buf);
  }

  // A thumbnail from PNG bytes: the texture loads at the first layout, so the page builds without a renderer.
  class PngNode final : public Node {
  public:
    PngNode(std::shared_ptr<std::vector<std::uint8_t>> png, float size, float radius) : m_png(std::move(png)) {
      m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
      m_image->setFit(ImageFit::Cover);
      m_image->setRadius(radius);
      m_image->setSize(size, size);
      setSize(size, size);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (!m_loaded && m_png && !m_png->empty()) {
        m_loaded = m_image->setSourceBytes(renderer, m_png->data(), m_png->size());
      }
      m_image->setSize(width(), height());
      m_image->layout(renderer);
    }

  private:
    std::shared_ptr<std::vector<std::uint8_t>> m_png;
    Image* m_image = nullptr;
    bool m_loaded = false;
  };

  // A flex with an input area over all of it (the area stays out of layout): hover fill_1 when radius > 0.
  class Click final : public Flex {
  public:
    InputArea* area = nullptr;

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      place();
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      place();
    }

  private:
    void place() {
      if (area != nullptr) {
        area->setPosition(0.0F, 0.0F);
        area->setSize(width(), height());
      }
    }
  };

  std::unique_ptr<Click> clickable(std::unique_ptr<Node> child, std::function<void()> onClick, float radius = 0.0F) {
    auto c = std::make_unique<Click>();
    c->setRadius(radius);
    c->addChild(std::move(child));
    auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, .participatesInLayout = false});
    Click* body = c.get();
    if (radius > 0.0F) {
      area->setOnEnter([body](const InputArea::PointerData&) { body->setFill(colorSpecFromRole(ColorRole::Fill1)); });
      area->setOnLeave([body] { body->setFill(clearColorSpec()); });
    }
    area->setOnClick([fn = std::move(onClick)](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        fn();
      }
    });
    c->area = static_cast<InputArea*>(c->addChild(std::move(area)));
    return c;
  }

  std::unique_ptr<Node> badge(float s, ColorRole fill, float x, float y) {
    auto b = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(fill),
                      .radius = 8.0F * s, .border = colorSpecFromRole(ColorRole::OnAccent),
                      .borderWidth = 1.0F * s, .width = 16.0F * s, .height = 16.0F * s});
    b->addChild(ui::glyph({.glyph = std::string("check"), .glyphSize = 10.0F * s,
                           .color = colorSpecFromRole(ColorRole::OnAccent)}));
    b->setPosition(x, y);
    b->setParticipatesInLayout(false);
    return b;
  }
} // namespace

PhonePage::PhonePage(LinkShare& link, std::string deviceId, Actions actions)
    : m_link(link), m_deviceId(std::move(deviceId)), m_actions(std::move(actions)) {
  loadImported();
}

void PhonePage::later(std::function<void()> fn) {
  DeferredCall::callLater([weak = weak_from_this(), fn = std::move(fn)] {
    if (weak.lock()) {
      fn();
    }
  });
}

void PhonePage::loadImported() {
  std::ifstream in(stateFile());
  const auto doc = nlohmann::json::parse(in, nullptr, false);
  if (doc.is_object() && doc.contains("dest:" + m_deviceId) && doc["dest:" + m_deviceId].is_string()) {
    m_destination = doc["dest:" + m_deviceId].get<std::string>();
  }
  if (doc.is_object() && doc.contains(m_deviceId) && doc[m_deviceId].is_array()) {
    for (const auto& p : doc[m_deviceId]) {
      if (p.is_string()) {
        m_imported.insert(p.get<std::string>());
      }
    }
  }
}

void PhonePage::saveImported() const {
  const fs::path file = stateFile();
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  nlohmann::json doc = nlohmann::json::object();
  {
    std::ifstream in(file);
    auto old = nlohmann::json::parse(in, nullptr, false);
    if (old.is_object()) {
      doc = std::move(old);
    }
  }
  doc[m_deviceId] = m_imported;
  if (!m_destination.empty()) {
    doc["dest:" + m_deviceId] = m_destination.string();
  }
  const fs::path tmp = file.string() + ".tmp";
  std::ofstream(tmp) << doc.dump();
  fs::rename(tmp, file, ec);
}

void PhonePage::load() {
  const LinkShare::Device* d = device();
  if (d == nullptr || !d->connected || m_loading) {
    return;
  }
  m_loading = true;
  m_link.recentPhotos(m_deviceId, kImportAll, [weak = weak_from_this()](std::vector<LinkShare::Photo> photos,
                                                                         const std::string& error) {
    auto self = weak.lock();
    if (self == nullptr) {
      return;
    }
    self->m_loading = false;
    self->m_loaded = true;
    self->m_photoError = error;
    std::map<std::string, std::shared_ptr<std::vector<std::uint8_t>>> old;
    for (Shot& shot : self->m_photos) {
      old.emplace(shot.photo.path, shot.png);
    }
    self->m_photos.clear();
    self->m_picked.clear();
    std::ranges::sort(photos, [](const auto& a, const auto& b) { return a.time > b.time; });
    for (LinkShare::Photo& photo : photos) {
      auto png = old.contains(photo.path) ? old[photo.path] : std::make_shared<std::vector<std::uint8_t>>();
      self->m_photos.push_back({.photo = std::move(photo), .png = std::move(png)});
    }
    kLog.info("phone {} photos {}{}", self->m_deviceId, self->m_photos.size(), error.empty() ? "" : " error " + error);
    for (Shot& shot : self->m_photos) {
      if (!shot.png->empty()) {
        continue;
      }
      // 2x tiles: the strip shows 88 pt.
      self->m_link.thumbnail(self->m_deviceId, shot.photo.path, static_cast<std::uint16_t>(kTile * 2),
                             [weak, png = shot.png](std::vector<std::uint8_t> bytes) {
                               auto live = weak.lock();
                               if (live == nullptr || bytes.empty()) {
                                 return;
                               }
                               *png = std::move(bytes);
                               live->m_actions.rebuild();
                             });
    }
    self->m_actions.rebuild();
  });
}

fs::path PhonePage::browseRoot() const {
  const LinkShare::Device* d = device();
  if (d == nullptr) {
    return {};
  }
  std::error_code ec;
  const fs::path root = fs::path(g_get_home_dir()) / "Phone" / d->name;
  return fs::is_directory(root, ec) ? root : fs::path();
}

std::vector<fs::path> PhonePage::received(const std::string& deviceId, std::size_t limit) {
  const char* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  const fs::path dir = downloads != nullptr ? fs::path(downloads) : fs::path(g_get_home_dir()) / "Downloads";
  const std::string want = "link://" + deviceId;
  std::vector<std::pair<fs::file_time_type, fs::path>> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    char buf[256];
    const ssize_t n = ::getxattr(entry.path().c_str(), "user.xdg.origin.url", buf, sizeof(buf));
    if (n > 0 && std::string_view(buf, static_cast<std::size_t>(n)) == want) {
      found.emplace_back(entry.last_write_time(ec), entry.path());
    }
  }
  std::ranges::sort(found, std::greater{});
  std::vector<fs::path> out;
  for (auto& [time, path] : found) {
    if (out.size() == limit) {
      break;
    }
    out.push_back(std::move(path));
  }
  return out;
}

std::size_t PhonePage::pendingCount() const {
  return static_cast<std::size_t>(std::ranges::count_if(m_photos, [this](const Shot& s) { return !imported(s.photo.path); }));
}

void PhonePage::openPhoto(const LinkShare::Photo& photo) {
  // One folder per phone path: the daemon cleans names and never replaces a file, so the name is no key.
  const fs::path cache = fs::path(g_get_user_cache_dir()) / "andromeda" / "files" / "phone" / m_deviceId /
                         std::format("{:016x}", std::hash<std::string>{}(photo.path));
  std::error_code ec;
  fs::create_directories(cache, ec);
  for (const auto& entry : fs::directory_iterator(cache, ec)) {
    if (entry.is_regular_file(ec)) {
      m_actions.quickLook(entry.path());
      return;
    }
  }
  m_link.savePhoto(m_deviceId, photo.path, cache, [weak = weak_from_this()](const fs::path& saved, const std::string& error) {
    if (auto self = weak.lock()) {
      if (error.empty()) {
        self->m_actions.quickLook(saved);
      } else {
        self->m_actions.toast(error);
      }
    }
  });
}

void PhonePage::pick(std::size_t index) {
  if (index >= m_photos.size()) {
    return;
  }
  if (!m_picked.erase(index)) {
    m_picked.insert(index);
  }
  kLog.debug("phone picked {}", m_picked.size());
  m_actions.rebuild();
}

void PhonePage::import(bool all) {
  if (m_importing) {
    return;
  }
  std::vector<std::string> paths;
  for (std::size_t i = 0; i < m_photos.size(); ++i) {
    const std::string& path = m_photos[i].photo.path;
    if (all ? !imported(path) : m_picked.contains(i)) {
      paths.push_back(path);
    }
  }
  if (paths.empty()) {
    return;
  }
  const fs::path folder = importFolder();
  std::error_code ec;
  fs::create_directories(folder, ec);
  m_importing = true;
  kLog.info("phone import {} photos to {}", paths.size(), folder.string());
  importNext(std::move(paths), folder, 0);
  m_actions.rebuild();
}

fs::path PhonePage::importFolder() const {
  std::error_code ec;
  if (!m_destination.empty() && fs::is_directory(m_destination, ec)) {
    return m_destination;
  }
  const LinkShare::Device* d = device();
  return PhotoImport::destination().parent_path() / (d ? d->name : m_deviceId) / PhotoImport::destination().filename();
}

void PhonePage::importNext(std::vector<std::string> paths, fs::path folder, std::size_t done) {
  if (done == paths.size()) {
    m_importing = false;
    m_picked.clear();
    saveImported();
    kLog.info("phone imported {} photos", done);
    m_actions.toast(std::format("Imported {} {} to {}", done, done == 1 ? "photo" : "photos", folder.filename().string()));
    m_actions.rebuild();
    return;
  }
  const std::string path = paths[done];
  m_link.savePhoto(m_deviceId, path, folder,
                   [weak = weak_from_this(), paths = std::move(paths), folder, done](const fs::path&,
                                                                                    const std::string& error) mutable {
                     auto self = weak.lock();
                     if (self == nullptr) {
                       return;
                     }
                     if (!error.empty()) {
                       self->m_importing = false;
                       self->saveImported();
                       kLog.warn("phone import stopped: {}", error);
                       self->m_actions.toast(error);
                       self->m_actions.rebuild();
                       return;
                     }
                     self->m_imported.insert(paths[done]);
                     self->importNext(std::move(paths), std::move(folder), done + 1);
                   });
}

std::unique_ptr<Node> PhonePage::buildHero(float s) const {
  const LinkShare::Device* d = device();
  auto card = ui::row({.align = FlexAlign::Center, .gap = 16.0F * s, .padding = 16.0F * s,
                       .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Style::scaledRadius(Tokens::radiusR8, s),
                       .border = colorSpecFromRole(ColorRole::GlassCardEdge), .borderWidth = 0.5F * s});
  card->addChild(std::make_unique<PlaceIcon>("phone", 64.0F * s));
  auto info = ui::column({.align = FlexAlign::Start, .gap = 4.0F * s, .flexGrow = 1.0F});
  info->addChild(text(d->name, 17.0F * s, ColorRole::Label1, FontWeight::SemiBold));
  std::string status;
  if (d->connected) {
    status = d->transport == "bluetooth" ? "Connected over Bluetooth" : "Connected over Wi-Fi";
    if (d->battery >= 0) {
      status += std::format(" · {} %", d->battery);
    }
  } else {
    status = ago(d->lastSeen);
  }
  info->addChild(text(std::move(status), 12.0F * s, ColorRole::Label2));
  if (d->storageTotal > 0) {
    const float used = static_cast<float>(static_cast<double>(d->storageTotal - std::min(d->storageFree, d->storageTotal)) /
                                          static_cast<double>(d->storageTotal));
    auto bar = ui::row({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Fill2),
                        .radius = 2.0F * s, .clipChildren = true, .width = kStorageBar * s, .height = 4.0F * s});
    bar->addChild(ui::box({.fill = colorSpecFromRole(used > 0.9F ? ColorRole::Danger : ColorRole::Accent),
                           .width = kStorageBar * s * used, .height = 4.0F * s}));
    info->addChild(std::move(bar));
    info->addChild(text(std::format("{} free of {}", formatFileSize(d->storageFree), formatFileSize(d->storageTotal)),
                        12.0F * s, ColorRole::Label2));
  }
  card->addChild(std::move(info));
  const bool on = d->connected;
  card->addChild(ui::button({.text = std::string("Mirror Screen"), .enabled = on, .variant = ButtonVariant::Default,
                             .onClick = [this] { m_link.mirror(m_deviceId); }}));
  card->addChild(ui::button({.text = std::string("Send Files…"), .enabled = on, .variant = ButtonVariant::Primary,
                             .onClick = [this] { m_actions.sendFiles(); }}));
  return card;
}

std::unique_ptr<Node> PhonePage::buildSection(float s, std::string title, std::string count,
                                              std::vector<std::pair<std::string, std::function<void()>>> links) const {
  auto row = ui::row({.align = FlexAlign::Center, .gap = 16.0F * s, .paddingH = 16.0F * s, .height = 40.0F * s});
  row->addChild(text(std::move(title), 13.0F * s, ColorRole::Label1, FontWeight::SemiBold, 1.0F));
  if (!count.empty()) {
    row->addChild(text(std::move(count), 13.0F * s, ColorRole::Label2));
  }
  for (auto& [label, onClick] : links) {
    row->addChild(clickable(text(label, 13.0F * s, ColorRole::AccentText), std::move(onClick)));
  }
  return row;
}

std::unique_ptr<Node> PhonePage::buildState(float s, std::string title, std::string body, std::string button,
                                            std::function<void()> onClick) const {
  auto card = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 8.0F * s,
                          .padding = 16.0F * s, .fill = colorSpecFromRole(ColorRole::GlassCard),
                          .radius = Style::scaledRadius(Tokens::radiusR8, s),
                          .border = colorSpecFromRole(ColorRole::GlassCardEdge), .borderWidth = 0.5F * s,
                          .minHeight = 184.0F * s});
  card->addChild(text(std::move(title), 15.0F * s, ColorRole::Label1, FontWeight::SemiBold));
  card->addChild(ui::label({.text = std::move(body), .fontSize = 13.0F * s, .color = colorSpecFromRole(ColorRole::Label2),
                            .maxWidth = 360.0F * s, .maxLines = 3, .textAlign = TextAlign::Center}));
  if (!button.empty()) {
    card->addChild(ui::button({.text = std::move(button), .variant = ButtonVariant::Default, .onClick = std::move(onClick)}));
  }
  return card;
}

std::unique_ptr<Node> PhonePage::buildTile(float s, std::size_t index, float size, bool importView) {
  const Shot& shot = m_photos[index];
  const float px = size * s;
  const bool done = imported(shot.photo.path);
  const bool picked = m_picked.contains(index);
  auto area = ui::inputArea({.width = px, .height = px,
                             .opacity = importView && done && !picked ? 0.4F : 1.0F,
                             .onClick = [this, index, importView](const InputArea::PointerData&) {
                               later([this, index, importView] {
                                 if (importView) {
                                   pick(index);
                                 } else if (index < m_photos.size()) {
                                   openPhoto(m_photos[index].photo);
                                 }
                               });
                             }});
  const float radius = Style::scaledRadius(Tokens::radiusR4, s);
  if (shot.png->empty()) {
    area->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .radius = radius, .width = px, .height = px}));
  } else {
    area->addChild(std::make_unique<PngNode>(shot.png, px, radius));
  }
  if (importView && picked) {
    auto ring = ui::box({.border = colorSpecFromRole(ColorRole::Accent), .borderWidth = 2.0F * s, .radius = radius,
                         .width = px, .height = px});
    ring->setPosition(0.0F, 0.0F);
    ring->setParticipatesInLayout(false);
    area->addChild(std::move(ring));
    area->addChild(badge(s, ColorRole::Accent, px - 20.0F * s, 4.0F * s));
  } else if (importView && done) {
    area->addChild(badge(s, ColorRole::Success, px - 20.0F * s, px - 20.0F * s));
  }
  return area;
}

std::unique_ptr<Node> PhonePage::build(float scale) {
  const float s = scale;
  auto scroll = ui::scrollView({.contentScale = s, .scrollbarAutoHide = true, .flexGrow = 1.0F});
  auto page = ui::column({.align = FlexAlign::Stretch, .gap = 8.0F * s, .paddingV = 16.0F * s});
  const LinkShare::Device* d = device();
  if (d == nullptr) {
    auto box = ui::column({.align = FlexAlign::Stretch, .paddingH = 16.0F * s});
    box->addChild(buildState(s, "Use your phone with Files", "Pair a phone with Andromeda Link to see its photos and folders here.",
                             "Set Up Link…", [] { (void)process::runAsync(std::vector<std::string>{"andromeda-settings", "link"}); }));
    page->addChild(std::move(box));
    scroll->content()->addChild(std::move(page));
    return scroll;
  }
  auto hero = ui::column({.align = FlexAlign::Stretch, .paddingH = 16.0F * s});
  hero->addChild(buildHero(s));
  page->addChild(std::move(hero));
  const bool away = !d->connected;
  if (away) {
    m_loaded = false;
  } else if (!m_loaded && !m_loading) {
    later([this] { load(); }); // connected since the last answer
  }

  // Recent Photos.
  const std::size_t pending = pendingCount();
  std::vector<std::pair<std::string, std::function<void()>>> links;
  if (!away && !m_photos.empty()) {
    links.emplace_back("Import…", [this] { later([this] { m_actions.showImport(shared_from_this()); }); });
  }
  auto photos = ui::column({.align = FlexAlign::Stretch, .opacity = away ? 0.5F : 1.0F});
  photos->addChild(buildSection(s, "Recent Photos", pending > 0 && !away ? std::format("{} not imported", pending) : "",
                                std::move(links)));
  if (m_photoError == "denied") {
    auto box = ui::column({.align = FlexAlign::Stretch, .paddingH = 16.0F * s});
    box->addChild(buildState(s, std::format("Photos Are Off on {}", d->name),
                             "Allow photo access in Andromeda Link on the phone, then try again.", "Try Again",
                             [this] { later([this] { load(); }); }));
    photos->addChild(std::move(box));
  } else if (m_photos.empty()) {
    photos->addChild(ui::row({.paddingH = 16.0F * s},
                             text(away ? std::string("The phone is away.")
                                  : m_loading ? std::string("Loading photos…")
                                  : !m_photoError.empty() ? m_photoError
                                                         : std::string("No photos on the phone."),
                                  13.0F * s, m_photoError.empty() ? ColorRole::Label2 : ColorRole::DangerText)));
  } else {
    auto strip = ui::row({.align = FlexAlign::Start, .gap = 8.0F * s, .paddingH = 16.0F * s, .clipChildren = true});
    for (std::size_t i = 0; i < std::min<std::size_t>(m_photos.size(), kRecent); ++i) {
      strip->addChild(buildTile(s, i, kTile, false));
    }
    photos->addChild(std::move(strip));
  }
  page->addChild(std::move(photos));

  // On the Phone: the browse mount's top folders.
  const fs::path root = browseRoot();
  std::error_code ec0;
  auto folders = ui::column({.align = FlexAlign::Stretch, .opacity = away ? 0.5F : 1.0F});
  folders->addChild(buildSection(s, "On the Phone", "",
                                 root.empty() ? std::vector<std::pair<std::string, std::function<void()>>>{}
                                              : std::vector<std::pair<std::string, std::function<void()>>>{
                                                    {"Show All", [this, root] { later([this, root] { m_actions.open(root); }); }}}));
  if (root.empty() && !away) {
    auto box = ui::column({.align = FlexAlign::Stretch, .paddingH = 16.0F * s});
    box->addChild(buildState(s, "Browsing Is Off",
                             std::format("Turn on Browse in Andromeda Link on {} to see its folders here.", d->name),
                             "Send Files…", [this] { later([this] { m_actions.sendFiles(); }); }));
    folders->addChild(std::move(box));
  } else if (!root.empty()) {
    auto grid = ui::row({.align = FlexAlign::Start, .wrap = true, .gap = 8.0F * s, .paddingH = 16.0F * s});
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
      if (entry.is_directory(ec) && !entry.path().filename().string().starts_with(".")) {
        dirs.push_back(entry.path());
      }
    }
    std::ranges::sort(dirs);
    for (const fs::path& dir : dirs) {
      const std::string name = dir.filename() == "DCIM" ? "Camera" : dir.filename().string();
      auto col = ui::column({.align = FlexAlign::Center, .gap = 4.0F * s, .padding = 8.0F * s, .width = 120.0F * s});
      col->addChild(std::make_unique<PlaceIcon>("folder", 64.0F * s));
      col->addChild(text(name, 12.0F * s, ColorRole::Label1));
      grid->addChild(clickable(std::move(col), [this, dir] { later([this, dir] { m_actions.open(dir); }); },
                               Style::scaledRadius(Tokens::radiusR8, s)));
    }
    folders->addChild(std::move(grid));
  }
  page->addChild(std::move(folders));

  // Received from the phone.
  const auto files = received(m_deviceId, 8);
  kLog.debug("phone page {} strip {} folders {} received {}{}", m_deviceId, std::min<std::size_t>(m_photos.size(), kRecent),
             root.empty() ? 0 : std::distance(fs::directory_iterator(root, ec0), fs::directory_iterator()), files.size(),
             m_photoError == "denied" ? " photos-off" : "");
  if (!files.empty()) {
    const char* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    const fs::path downloads = dl != nullptr ? fs::path(dl) : files.front().parent_path();
    page->addChild(buildSection(s, std::format("Received from {}", d->name), "",
                                {{"Show in Downloads", [this, downloads] { later([this, downloads] { m_actions.open(downloads); }); }}}));
    auto list = ui::column({.align = FlexAlign::Stretch, .paddingH = 16.0F * s});
    std::error_code ec;
    for (const fs::path& file : files) {
      auto r = ui::row({.align = FlexAlign::Center, .gap = 12.0F * s, .paddingH = 8.0F * s, .height = 32.0F * s,
                        .flexGrow = 1.0F});
      r->addChild(text(file.filename().string(), 13.0F * s, ColorRole::Label1, FontWeight::Normal, 1.0F));
      r->addChild(text(formatFileSize(fs::file_size(file, ec)), 12.0F * s, ColorRole::Label2));
      auto row = clickable(std::move(r), [this, file] { later([this, file] { m_actions.quickLook(file); }); },
                           Style::scaledRadius(Tokens::radiusR4, s));
      row->setAlign(FlexAlign::Stretch);
      list->addChild(std::move(row));
    }
    page->addChild(std::move(list));
  }
  scroll->content()->addChild(std::move(page));
  return scroll;
}

std::unique_ptr<Node> PhonePage::buildImport(float scale) {
  const float s = scale;
  const LinkShare::Device* d = device();
  auto root = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  auto top = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .paddingV = 12.0F * s});
  top->addChild(ui::segmented({.options = std::vector<ui::SegmentedOption>{{.label = "Not Imported"}, {.label = "All"}},
                               .selectedIndex = m_showAll ? 1U : 0U, .equalSegmentWidths = true, .width = 260.0F * s,
                               .onChange = [this](std::size_t i) { later([this, i] { m_showAll = i == 1; m_actions.rebuild(); }); }}));
  root->addChild(std::move(top));

  auto scroll = ui::scrollView({.contentScale = s, .scrollbarAutoHide = true, .flexGrow = 1.0F});
  auto body = ui::column({.align = FlexAlign::Stretch, .gap = 8.0F * s, .paddingV = 8.0F * s});
  std::string day;
  Flex* grid = nullptr;
  std::size_t shown = 0;
  for (std::size_t i = 0; i < m_photos.size(); ++i) {
    if (!m_showAll && imported(m_photos[i].photo.path)) {
      continue;
    }
    const std::string title = dayTitle(m_photos[i].photo.time);
    if (title != day || grid == nullptr) {
      day = title;
      std::size_t fresh = 0;
      for (const Shot& shot : m_photos) {
        fresh += dayTitle(shot.photo.time) == title && !imported(shot.photo.path) ? 1 : 0;
      }
      body->addChild(ui::row({.paddingV = 4.0F * s, .paddingH = 16.0F * s},
                             text(fresh > 0 ? std::format("{} · {} new", title, fresh) : title, 13.0F * s,
                                  ColorRole::Label1, FontWeight::SemiBold)));
      auto g = ui::row({.align = FlexAlign::Start, .wrap = true, .gap = 6.0F * s, .paddingH = 16.0F * s});
      grid = g.get();
      body->addChild(std::move(g));
    }
    grid->addChild(buildTile(s, i, kImportTile, true));
    ++shown;
  }
  if (shown == 0) {
    body->addChild(ui::row({.justify = FlexJustify::Center, .paddingV = 48.0F * s},
                           text(m_loading ? std::string("Loading photos…") : std::string("Every photo is imported."),
                                13.0F * s, ColorRole::Label2)));
  }
  scroll->content()->addChild(std::move(body));
  root->addChild(std::move(scroll));

  // Bottom bar.
  const std::size_t pending = pendingCount();
  auto bar = ui::row({.align = FlexAlign::Center, .gap = 8.0F * s, .paddingH = 16.0F * s,
                      .fill = colorSpecFromRole(ColorRole::TintThick), .border = colorSpecFromRole(ColorRole::Separator),
                      .borderWidth = 0.5F * s, .height = 56.0F * s});
  bar->addChild(text(m_importing ? std::string("Importing…") : std::format("{} selected", m_picked.size()), 13.0F * s,
                     ColorRole::Label2, FontWeight::Medium, 1.0F));
  const std::string where = m_destination.empty() ? std::format("To Pictures › {}", d ? d->name : m_deviceId)
                                                 : std::format("To {}", m_destination.filename().string());
  bar->addChild(ui::button({.text = where, .glyph = std::string("folder"), .variant = ButtonVariant::Ghost,
                            .onClick = [this] {
                              m_actions.chooseFolder([weak = weak_from_this()](const fs::path& folder) {
                                if (auto self = weak.lock()) {
                                  self->m_destination = folder;
                                  self->saveImported();
                                  kLog.info("phone import folder {}", folder.string());
                                  self->m_actions.rebuild();
                                }
                              });
                            }}));
  bar->addChild(ui::button({.text = std::format("Import All {} New", pending), .enabled = pending > 0 && !m_importing,
                            .variant = ButtonVariant::Default, .onClick = [this] { later([this] { import(true); }); }}));
  bar->addChild(ui::button({.text = std::format("Import {}", m_picked.size()), .enabled = !m_picked.empty() && !m_importing,
                            .variant = ButtonVariant::Primary, .onClick = [this] { later([this] { import(false); }); }}));
  root->addChild(std::move(bar));
  return root;
}

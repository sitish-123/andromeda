#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/node.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Box;
class Flex;
class Glyph;
class Image;
class InputArea;
class Label;
class ThumbnailService;

// The immersive Gallery view (DESIGN.md "Files v2 immersive Gallery", mockup section 15): the current photo on an
// ambient wash of its own colours, a floating title capsule, edge arrows and a filmstrip. Follows the light/dark scheme.
class FileGallery final : public Node {
public:
  // The Info card's content for one photo; the host reads it off the UI thread.
  struct PhotoInfo {
    std::string name;
    std::string kind;                // "JPEG · 4032 × 3024 · 4.1 MB"
    std::vector<std::string> chips;  // camera and exposure
    std::vector<std::uint8_t> mapBgra; // empty: no GPS tag, no map
    int mapWidth = 0;
    int mapHeight = 0;
    std::string place;
    std::vector<std::pair<std::string, std::string>> rows; // Taken, From, Folder, Tags
  };

  struct Host {
    std::function<void()> invalidate;
    std::function<void(std::size_t index)> select; // a filmstrip item or an arrow picked another photo
    // An action capsule button on the current photo: "favourite", "rotate", "markup", "phone", "share" or "more";
    // the scene point is where a menu opens.
    std::function<void(std::string_view action, float sceneX, float sceneY)> action;
    std::function<bool(const std::filesystem::path&)> starred;
    std::function<std::string()> phoneName; // the paired phone; empty: no Send button
    // Reads a photo's Info card content (map in device pixels); `done` runs on the UI thread.
    std::function<void(const std::filesystem::path&, int mapWidth, int mapHeight, std::function<void(PhotoInfo)> done)>
        info;
    // Video in place (mockup section 15 middle): `rect` = surface-local x, y, width, height; motion photos play muted.
    std::function<void(const std::filesystem::path& video, bool sound, std::array<float, 4> rect)> playVideo;
    std::function<void(std::array<float, 4> rect)> placeVideo;
    std::function<void()> toggleVideo;
    std::function<void()> stopVideo;
    // A motion photo whose video sits inside the JPEG: the host cuts it out off the UI thread; empty = none.
    std::function<void(const std::filesystem::path& photo, std::function<void(std::filesystem::path)> done)>
        motionVideo;
  };

  // What outlives a rebuild of the window's nodes (the browser owns it): the photo, its zoom, the Info card, a playing
  // video and the slideshow carry over to the new Gallery node.
  struct State {
    std::filesystem::path item;
    float zoom = 1.0F;
    float panX = 0.0F;
    float panY = 0.0F;
    bool infoOpen = false;
    std::filesystem::path infoFor;
    PhotoInfo info;
    bool videoActive = false;
    bool scrubPlaying = false;
    float scrubProgress = 0.0F;
    std::string scrubTime = "0:00";
    bool slideshowOn = false;
    std::string washPath;
  };

  FileGallery(float scale, ThumbnailService* thumbnails, AnimationManager& animations, Host host,
              std::shared_ptr<State> state);
  ~FileGallery() override;
  FileGallery(const FileGallery&) = delete;
  FileGallery& operator=(const FileGallery&) = delete;

  // Per item: the local calendar day (a filmstrip divider marks a new one) and its motion video, if any.
  struct ItemInfo {
    std::int32_t day = 0;
    bool video = false;
    bool motion = false;                 // a motion photo: a dot in the filmstrip, plays once on open
    std::filesystem::path motionVideo;   // a paired video file (Live Photo); empty: the video is inside the JPEG
  };
  // Images and videos of the folder, in view order; `current` indexes them. `info` runs parallel to `items`.
  void setItems(std::vector<std::filesystem::path> items, std::size_t current, std::vector<ItemInfo> info = {});
  [[nodiscard]] std::size_t current() const noexcept { return m_current; }
  [[nodiscard]] std::size_t count() const noexcept { return m_items.size(); }
  // F: hides the chrome at once, or shows it again (DESIGN.md: chrome fades 2 s after the last pointer move).
  void toggleChrome();
  // I: the Info card (name, type, size, EXIF chips, map, From, Folder, Tags).
  void toggleInfo();
  // The current photo's state changed outside (starred, rotated): read it again.
  void refresh();
  // E2E geometry (DESK_FILES_GEOMETRY): "gallery-action <id> x y" per shown action button, surface-local centres.
  void appendGeometry(std::string& out) const;
  // Space: plays or pauses the current video, else starts or stops the slideshow.
  void togglePlay();
  // The browser left the Gallery: this node no longer writes its state back when it goes.
  void detachState() { m_state.reset(); }
  // The browser came back to the Gallery on this node: a fresh start (fit, no card, no video, no slideshow).
  void enter(std::shared_ptr<State> state);
  // From the host's player.
  void setVideoProgress(bool playing, float progress, const std::string& time);
  void videoEnded();

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  struct Held {
    std::string path;
    int px = 0;
  };
  void hold(const std::string& path, int px, std::vector<Held>& next);
  void releaseAll(std::vector<Held>& held);
  void rebuildFilm();
  void showChrome();
  void driveChrome(float target);
  void requestInfo();
  void startMotion();
  void stopVideo();
  [[nodiscard]] bool currentIsVideo() const { return !m_itemInfo.empty() && m_itemInfo[m_current].video; }
  // Zoom (mockup section 15 right): 1 = fit. Keeps the photo point under (x, y) still.
  void zoomAt(float x, float y, float zoom);
  void resetZoom();
  void clampPan();
  void flingPan(float vx, float vy);
  [[nodiscard]] float zoom100() const; // the zoom that shows one photo pixel per surface pixel
  void buildInfoCard();

  float m_scale;
  ThumbnailService* m_thumbs;
  Host m_host;
  std::vector<std::filesystem::path> m_items;
  std::vector<ItemInfo> m_itemInfo;
  std::size_t m_current = 0;
  std::vector<Held> m_held; // acquired from m_thumbs, released on the next layout or at the end

  Box* m_bg = nullptr;
  Image* m_ambient = nullptr;
  Image* m_ambientOld = nullptr;        // the previous photo's wash, fading out (300 ms cross-fade)
  std::string m_washPath;               // whose wash m_ambient shows
  std::string m_oldWashPath;            // held until its fade ends
  float m_washFade = 1.0F;              // 0 -> 1: the new wash over the old
  AnimationManager::Id m_washAnim = 0;
  std::chrono::steady_clock::time_point m_shownAt{}; // when the current photo was asked for (speed budget logs)
  bool m_loggedThumb = true; // true until a photo is asked for: a rebuilt node never times its carried-over photo
  bool m_loggedFull = true;
  Image* m_photo = nullptr;
  Flex* m_title = nullptr;
  Label* m_name = nullptr;
  Label* m_position = nullptr;
  Flex* m_prev = nullptr;
  Flex* m_next = nullptr;
  Flex* m_actions = nullptr;
  Glyph* m_starGlyph = nullptr;
  Flex* m_rotateButton = nullptr;
  Flex* m_phoneButton = nullptr;
  Flex* m_infoButton = nullptr;
  std::vector<std::pair<const Node*, const char*>> m_actionIds;
  bool m_infoOpen = false;
  bool m_videoActive = false;
  std::array<float, 4> m_videoRect{};
  std::uint64_t m_motionGeneration = 0;
  Flex* m_scrub = nullptr;
  Glyph* m_scrubGlyph = nullptr;
  Flex* m_scrubTrack = nullptr;
  Box* m_scrubLevel = nullptr;
  Label* m_scrubTime = nullptr;
  float m_scrubProgress = 0.0F;
  bool m_scrubPlaying = false;
  Timer m_slideshow;
  bool m_slideshowOn = false;
  float m_zoom = 1.0F;
  float m_panX = 0.0F; // photo centre offset from the stage centre, surface px
  float m_panY = 0.0F;
  float m_fitW = 0.0F; // the photo at zoom 1, surface px
  float m_fitH = 0.0F;
  float m_stageCx = 0.0F;
  float m_stageCy = 0.0F;
  int m_photoTexW = 0;
  float m_pinchStartZoom = 1.0F;
  bool m_wantHundred = false; // a double-click asked for 100 % before the full-size decode told the photo's size
  bool m_dragging = false;
  float m_dragX = 0.0F;
  float m_dragY = 0.0F;
  float m_velX = 0.0F; // px per ms
  float m_velY = 0.0F;
  std::chrono::steady_clock::time_point m_dragAt{};
  std::chrono::steady_clock::time_point m_lastClickAt{};
  AnimationManager::Id m_panAnimX = 0;
  AnimationManager::Id m_panAnimY = 0;
  AnimationManager::Id m_zoomAnim = 0;
  Image* m_navigator = nullptr;
  Box* m_navigatorFrame = nullptr;
  Flex* m_zoomChip = nullptr;
  Label* m_zoomLabel = nullptr;
  Flex* m_infoCard = nullptr;
  Image* m_map = nullptr;
  std::filesystem::path m_infoFor; // the photo m_info belongs to, or is being read for
  PhotoInfo m_info;
  bool m_mapDirty = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  std::shared_ptr<State> m_state;
  Flex* m_film = nullptr;
  std::vector<Image*> m_filmImages;
  std::vector<std::size_t> m_filmIndex;
  Box* m_filmRing = nullptr;
  bool m_filmDirty = true;
  float m_filmCurrentW = 64.0F; // the current photo at its aspect, 4-grid units (mockup .film i.on)

  AnimationManager& m_animations;
  AnimationManager::Id m_chromeAnim = 0;
  float m_chrome = 1.0F;       // 1 = capsules shown; 0 = hidden, the photo fills the freed space
  float m_chromeTarget = 1.0F;
  Timer m_hideTimer;
  InputArea* m_hover = nullptr; // the whole view: a pointer move brings the chrome back
};

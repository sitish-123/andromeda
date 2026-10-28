#pragma once

#include <memory>

struct wl_global;
struct wlr_image_description_v1_data;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_surface;

namespace andromeda {

  class Server;

  class ColorManager {
  public:
    explicit ColorManager(Server& server);
    ~ColorManager();

    ColorManager(const ColorManager&) = delete;
    ColorManager& operator=(const ColorManager&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] wl_global* global() const;
    [[nodiscard]] const wlr_image_description_v1_data* surfaceDescription(wlr_surface* surface) const;
    [[nodiscard]] bool surfaceRequiresHdrOutput(wlr_surface* surface) const;
    // Register before adding surfaces. Scene destruction removes the registration.
    [[nodiscard]] bool registerScene(wlr_scene* scene);
    void applySurfaceDescriptionToBuffer(wlr_surface* surface, wlr_scene_buffer* buffer) const;
    void updatePreferredDescriptions();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace andromeda

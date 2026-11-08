#pragma once

#include "core/animation.h"
#include "scene/ssd_frame_geometry.h"
#include "scene/surface_blur.h"
#include "view/chrome_attachment.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_blur;

namespace andromeda {
  class Server;
  class TabLabelCache;

  // Ticks the title-bar hover fades (rulebook P3) of every frame that has one running; one per server.
  class TitleBarMotion final : public Animatable {
  public:
    explicit TitleBarMotion(Server& server);
    ~TitleBarMotion() override;
    TitleBarMotion(const TitleBarMotion&) = delete;
    TitleBarMotion& operator=(const TitleBarMotion&) = delete;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override;
    void scheduleFrames();

  private:
    Server& m_server;
  };


  class SsdFrame final : public ViewChromeAttachment {
  public:
    SsdFrame(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent = nullptr);
    ~SsdFrame() override;
    void layout(const ViewChromeGeometry& geometry) override;
    void setFocused(bool focused) override;
    void setAlpha(float alpha) override;
    // Like surface blur, backdrop blur follows lifecycle fades independently of the chrome's drag opacity.
    void setAlphas(float chromeAlpha, float blurAlpha);
    void reloadConfig() override;
    void setTitle(std::string_view title);
    void setResizable(bool resizable);
    void setMaximizeEnabled(bool enabled);
    // The buttons to show, from the title outward.
    void setButtons(std::span<const TitleBarButton> buttons);
    void setMaximized(bool maximized);
    // `fade` false snaps the wash: the window moved under a still pointer, so nothing reads as a hover.
    void setHovered(SsdFramePart part, bool fade = true);
    void setPressed(SsdFramePart part);
    void setBlurOptions(const SurfaceBlurOptions& options);
    void setBlurLive(bool live);
    [[nodiscard]] bool maximizeEnabled() const { return m_maximizeEnabled; }
    [[nodiscard]] int titleBarHeight() const { return m_geometry.titleBarHeight; }
    [[nodiscard]] bool contains(const wlr_scene_node* node) const;
    [[nodiscard]] SsdFramePoint pointAt(double x, double y) const;
    // A button's cell relative to the client content (the title bar lies above y = 0); empty when not shown.
    [[nodiscard]] std::optional<wlr_box> buttonCell(TitleBarButton button) const;
    void snapshot(wlr_scene_tree* parent) const;
    [[nodiscard]] std::unique_ptr<ViewChromeAttachment> makePreview(wlr_scene_tree* parent) const override;
    void syncPreview(ViewChromeAttachment& preview) const override;

  private:
    void redraw();
    void updateColors();
    void updateBlur();
    friend class TitleBarMotion;
    // Advances the hover fade; false once it rests.
    bool tickHover(double nowMsec);
    [[nodiscard]] int zoomed(int value) const;
    wlr_scene_tree* m_tree = nullptr;
    // Invisible resize bands stay outside the window's animation capture bounds.
    wlr_scene_tree* m_gripTree = nullptr;
    wlr_scene_rect* m_background = nullptr;
    wlr_scene_rect* m_separator = nullptr;
    wlr_scene_blur* m_blur = nullptr;
    SurfaceBlurOptions m_blurOptions;
    bool m_blurLive = false;
    wlr_scene_buffer* m_label = nullptr;
    wlr_scene_rect* m_close = nullptr;
    wlr_scene_rect* m_maximize = nullptr;
    wlr_scene_rect* m_minimize = nullptr;
    // Windows style: the hovered button's cell fill, under the marks.
    wlr_scene_rect* m_hoverCell = nullptr;
    // Line glyphs over the buttons (Windows always, macOS on hover); indexed by TitleBarButton.
    std::array<wlr_scene_buffer*, kTitleBarButtonCount> m_glyphs{};
    struct GlyphKey {
      int px = 0;
      bool mac = false;
      std::array<float, 4> ink{};
      double scale = 0.0;
      bool operator==(const GlyphKey&) const = default;
    };
    std::array<GlyphKey, kTitleBarButtonCount> m_glyphKeys{};
    TitleBarLayout m_parts;
    std::vector<TitleBarButton> m_buttons;
    std::array<wlr_scene_rect*, 4> m_grips{};
    std::shared_ptr<TabLabelCache> m_labels;
    ViewChromeGeometry m_geometry;
    std::string m_title;
    bool m_focused = false;
    bool m_resizable = false;
    bool m_maximizeEnabled = true;
    bool m_maximized = false;
    bool m_preview = false;
    float m_alpha = 1.0F;
    float m_blurAlpha = 1.0F;
    SsdFramePart m_hovered = SsdFramePart::None;
    // The part the hover wash is drawn on, kept while it fades out; m_hoverLevel is the wash strength 0..1.
    SsdFramePart m_shownHover = SsdFramePart::None;
    AnimatedValue m_hoverLevel{0.0};
    SsdFramePart m_pressed = SsdFramePart::None;
  };
} // namespace andromeda

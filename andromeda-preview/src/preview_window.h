#pragma once

#include "file_kind.h"
#include "helper_client.h"
#include "image_loader.h"
#include "video_view.h"
#include "render/scene/input_dispatcher.h"
#include "render/animation/animation_manager.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class AppLoop;
class Flex;
class Image;
class Node;
class Renderer;
class ScrollView;
class ToplevelSurface;
struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;

namespace desk {
  class ForeignParent;
}

namespace preview {

  // The Quick Look window: one file at a time, decoded off the window process (helper) or by glycin.
  class PreviewWindow {
  public:
    struct Callbacks {
      std::function<void()> closed;
      std::function<void(std::int32_t direction)> selection;
    };

    PreviewWindow(AppLoop& loop, desk::ForeignParent& parent, Callbacks callbacks);
    ~PreviewWindow();
    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    // Shows `path` (a local file). A second ShowFile for the shown file with closeIfShown closes the window.
    void show(const std::string& path, const std::string& windowHandle, bool closeIfShown);
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }

  private:
    struct Page {
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      std::uint32_t stride = 0;
      std::shared_ptr<MappedFd> pixels;
      bool requested = false;
    };

    void load(const std::string& path);
    void loadPdf();
    void loadVideo(HelperKind kind);
    void dropHelper();
    void requestVisiblePages();
    void onPage(std::size_t index, const HelperReply& reply);
    void uploadPendingPages(Renderer& renderer);
    void helperFailed();
    void requestPage(std::size_t index);
    void openSurface(const std::string& windowHandle);
    void prepareFrame(bool needsLayout);
    void buildScene(Renderer& renderer, float width, float height);
    [[nodiscard]] std::unique_ptr<Node> buildBody(Renderer& renderer, float width);
    void invalidate();
    void openInDefaultApp();
    void onKeyboard(const KeyboardEvent& event);
    void onPointer(const PointerEvent& event);
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> windowSizeFor(const WaylandOutput* output) const;

    AppLoop& m_loop;
    desk::ForeignParent& m_parent;
    Callbacks m_callbacks;
    AnimationManager m_animations;
    InputDispatcher m_input;
    std::unique_ptr<ToplevelSurface> m_surface;
    std::unique_ptr<Node> m_root;
    ScrollView* m_scroll = nullptr;
    std::vector<Image*> m_pageImages;
    std::vector<std::size_t> m_pendingUploads;
    bool m_dirty = true;
    bool m_pointerInside = false;

    std::string m_path;
    std::string m_contentType;
    FileKind m_kind = FileKind::Other;
    std::uint64_t m_generation = 0;
    std::unique_ptr<HelperClient> m_helper;
    // Declared after m_helper: it uses the helper, so it goes first.
    std::unique_ptr<VideoView> m_video;
    Node* m_videoBox = nullptr;
    ImageDecoder m_decoder;
    Pixels m_image;
    std::vector<Page> m_pages;
    float m_pageAspect = 1.414F;
    std::string m_text;
    bool m_textTruncated = false;
    bool m_loading = false;
    bool m_failed = false;
    std::string m_shownNote;
  };

} // namespace preview

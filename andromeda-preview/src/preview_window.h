#pragma once

#include <atomic>

#include "file_kind.h"
#include "helper_client.h"
#include "audio_out.h"
#include "image_loader.h"
#include "video_view.h"
#include "render/scene/input_dispatcher.h"
#include "render/animation/animation_manager.h"
#include "core/timer_manager.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AppLoop;
class FileOpsClient;
class Flex;
class Image;
class Label;
class Node;
class ProgressBar;
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

  class CodeLines;

  // The Quick Look window: one file at a time, decoded off the window process (helper) or by glycin.
  class PreviewWindow {
  public:
    struct Callbacks {
      std::function<void()> closed;
      std::function<void(std::int32_t direction)> selection;
    };

    // fileOps lists archives (FileOps1 ListArchive): the daemon parses them, the window only shows the names.
    PreviewWindow(AppLoop& loop, desk::ForeignParent& parent, FileOpsClient& fileOps, Callbacks callbacks);
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
    void loadPdf(const std::string& path);
    void loadOffice();
    void loadMedia(HelperKind kind);
    void startAudio(bool withVideo);
    void togglePlayback();
    [[nodiscard]] std::unique_ptr<Node> buildAudio(Renderer& renderer, float width);
    [[nodiscard]] std::unique_ptr<Node> buildMediaBar();
    void startProgress();
    void requestPeaks();
    void requestFont(float width);
    void updateProgress();
    void dropHelper();
    void requestVisiblePages();
    void markCurrentPage();
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
    FileOpsClient& m_fileOps;
    Callbacks m_callbacks;
    AnimationManager m_animations;
    InputDispatcher m_input;
    std::unique_ptr<ToplevelSurface> m_surface;
    std::unique_ptr<Node> m_root;
    ScrollView* m_scroll = nullptr;
    std::vector<Image*> m_pageImages;
    // PDF page strip (mockup §15): one scaled page per document page; the shown one has an accent ring.
    std::vector<Flex*> m_stripFrames;
    std::vector<Image*> m_stripImages;
    Label* m_pageLabel = nullptr;
    float m_pageStep = 0.0F; // one page plus its gap in the main scroll, logical px
    std::size_t m_currentPage = 0;
    std::vector<std::size_t> m_pendingUploads;
    bool m_dirty = true;
    bool m_pointerInside = false;

    std::string m_path;
    std::string m_contentType;
    std::string m_openWith; // the default app's name for this type; empty = none
    FileKind m_kind = FileKind::Other;
    std::uint64_t m_generation = 0;
    // m_generation for worker threads: they read it without touching the window (0 once the window is gone).
    std::shared_ptr<std::atomic<std::uint64_t>> m_live = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::unique_ptr<HelperClient> m_helper;
    // Declared after m_helper: it uses the helper, so it goes first.
    std::unique_ptr<VideoView> m_video;
    Node* m_videoBox = nullptr;
    // The picture's rounded corners: concave masks in the window colour over the subsurface edge.
    std::array<Node*, 4> m_videoCorners{};
    // The letterbox around the picture (top, bottom, left, right), in the window colour over the hole.
    std::array<Node*, 4> m_letterbox{};
    std::unique_ptr<AudioOut> m_audio;
    bool m_audioOnly = false;
    bool m_paused = false;
    std::uint32_t m_durationMs = 0;
    std::string m_title;
    std::string m_artist;
    Pixels m_cover;
    // The memfd glycin reads the cover from (as /proc/self/fd/N); -1 when none.
    int m_coverFd = -1;
    ProgressBar* m_progress = nullptr;
    Label* m_time = nullptr;
    // Waveform (mockup §15): a second helper decodes the whole file for it, so playback never waits.
    std::unique_ptr<HelperClient> m_peaksHelper;
    std::vector<std::uint8_t> m_peaks;
    Timer m_progressTimer;
    ImageDecoder m_decoder;
    Pixels m_image;
    std::vector<Page> m_pages;
    float m_pageAspect = 1.414F;
    std::string m_text;
    bool m_textTruncated = false;
    // Code with line numbers and colours; the list view it feeds holds a raw pointer, so it outlives m_root.
    std::unique_ptr<CodeLines> m_code;
    struct ArchiveRow {
      std::string path;
      std::int64_t size = 0;
      bool isDir = false;
    };
    std::vector<ArchiveRow> m_archive;
    // Font specimen pixels from the helper, drawn at the window's scale once its width is known.
    Page m_fontPage;
    bool m_fontRequested = false;
    // Folder card: item count, total size (a worker walks the tree) and the newest items.
    std::size_t m_folderItems = 0;
    std::uint64_t m_folderBytes = 0;
    bool m_folderCut = false; // the walk stopped at its cap: the size is "at least"
    std::vector<std::string> m_folderPeek;
    std::size_t m_archiveTotal = 0;
    bool m_loading = false;
    bool m_spinner = false; // a load still running after 200 ms shows the spinner
    Timer m_spinnerTimer;
    bool m_failed = false;
    std::string m_shownNote;
  };

} // namespace preview

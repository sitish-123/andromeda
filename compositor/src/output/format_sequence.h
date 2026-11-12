#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

struct wlr_output_mode;

namespace andromeda {
  struct OutputMode;

  enum class FormatTier : uint8_t {
    Hdr,
    Sdr10,
    Sdr8,
  };

  enum class FormatCommitResult : uint8_t {
    Committed,
    Rejected,
    Retry,
  };

  // Whether the commits of one pass over the candidates may modeset.
  enum class ModesetPolicy : uint8_t {
    Allowed,
    Forbidden,
  };

  enum class StageResult : uint8_t {
    Staged,
    Unsupported,
    NeedsModeset, // under ModesetPolicy::Forbidden only; nothing was staged
  };

  struct FormatSequenceParams {
    bool hdrRequested;
    bool imageDescAvailable;
    bool hdrWasActive;
    uint32_t currentRenderFormat;
    int bitDepth;
    bool tryVrrOn;
    const OutputMode* configuredModeSpec; // nullptr = No mode configured
    std::string_view automaticMode;       // the mode picked without a configured one, as text; empty = none staged
    wlr_output_mode* preferredMode;       // nullptr = No fallback available
    wlr_output_mode* keptLiveMode;        // nullptr = No live mode was kept
    bool refreshOnlyChange;               // the staged mode changes only the refresh of the lit output
    bool modeFallbackAlreadyWarned;
    std::optional<FormatTier> vrrDroppedTier;
    std::string_view earlyHdrFail;
  };

  struct FormatSequenceOps {
    std::function<StageResult(uint32_t format, bool vrr, ModesetPolicy policy)> stageSdr;
    std::function<StageResult(uint32_t format, bool vrr, ModesetPolicy policy)> stageHdr;
    std::function<bool()> test;
    std::function<FormatCommitResult()> commit;
    std::function<void()> clearImageDescription;
    std::function<void(wlr_output_mode*)> stageMode;
    std::function<void(std::string_view requested, const wlr_output_mode& fallback)> warnModeFallback;
    std::function<void(FormatTier tier)> warnVrrDropped;
  };

  // The no-modeset pass of a refresh-only change. Rejected and NoLiveCandidate retry with a modeset.
  enum class RefreshPass : uint8_t {
    NotRun,
    Committed, // in the live colour state
    Busy,
    Rejected,
    NoLiveCandidate, // every candidate changes the colour state
  };

  struct FormatSequenceResult {
    bool committed = false;
    bool usedLiveModeRetry = false;
    RefreshPass refreshPass = RefreshPass::NotRun;
    bool usedModeFallback = false;
    bool modeFallbackWarnedNow = false;
    bool retry = false;
    FormatTier committedTier = FormatTier::Sdr8;
    uint32_t committedFormat = 0;
    bool vrrDropped = false;
    std::string_view hdrFail;
    std::string_view sdr10Fail;
  };

  [[nodiscard]] FormatSequenceResult
  runFormatSequence(const FormatSequenceParams& params, const FormatSequenceOps& ops);
} // namespace andromeda

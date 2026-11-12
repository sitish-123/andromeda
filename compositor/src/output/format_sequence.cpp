#include "output/format_sequence.h"

#include "config/value_parse.h"
#include "output/hdr_format.h"
#include "output/sdr_format.h"

#include <drm_fourcc.h>
#include <format>
#include <span>

namespace andromeda {

  namespace {

    std::span<const bool> vrrPasses(bool tryVrrOn) {
      static constexpr bool kBothPasses[] = {true, false};
      static constexpr bool kNoVrrPass[] = {false};
      return tryVrrOn ? std::span<const bool>(kBothPasses) : std::span<const bool>(kNoVrrPass);
    }

    // Runs the HDR -> SDR10 -> SDR8 x VRR sequence for whatever mode is currently staged.
    // Returns true if a commit succeeded. Sets hdrFail/sdr10Fail accordingly. Under ModesetPolicy::Forbidden only the
    // live colour state is staged, so the reasons of a committed pass describe skipped tiers, not rejected ones.
    bool runSequenceForCurrentMode(
        const FormatSequenceParams& params, const FormatSequenceOps& ops, ModesetPolicy policy,
        FormatSequenceResult& result, std::string_view& hdrFail, std::string_view& sdr10Fail, bool& anyStaged
    ) {
      using StageOp = std::function<StageResult(uint32_t, bool, ModesetPolicy)>;
      const auto stage = [&](const StageOp& op, uint32_t fmt, bool vrr) -> bool {
        const bool staged = op(fmt, vrr, policy) == StageResult::Staged;
        anyStaged = anyStaged || staged;
        return staged;
      };

      const auto commitStaged = [&](FormatTier tier, uint32_t fmt, bool vrr) -> bool {
        const FormatCommitResult commit = ops.commit();
        if (commit == FormatCommitResult::Retry) {
          result.retry = true;
          return false;
        }
        if (commit == FormatCommitResult::Rejected) {
          return false;
        }
        result.committedTier = tier;
        result.committedFormat = fmt;
        result.vrrDropped = params.tryVrrOn && !vrr;
        if (result.vrrDropped && params.vrrDroppedTier != tier) {
          ops.warnVrrDropped(tier);
        }
        return true;
      };

      const auto commitTenBit = [&](FormatTier tier, uint32_t fmt, bool vrr,
                                    const StageOp& op) -> bool {
        if (commitStaged(tier, fmt, vrr)) {
          return true;
        }
        if (result.retry) {
          return false;
        }
        return vrr && stage(op, fmt, false) && commitStaged(tier, fmt, false);
      };

      // HDR.
      const auto tryHdrFormats = [&]() -> bool {
        if (!(params.hdrRequested && params.imageDescAvailable)) {
          if (params.hdrWasActive) {
            ops.clearImageDescription();
          }
          return false;
        }

        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          const auto accepted = selectHdrRenderFormat(params.currentRenderFormat, [&](uint32_t fmt) {
            return stage(ops.stageHdr, fmt, vrr) && ops.test();
          });
          if (!accepted) {
            continue;
          }
          if (commitTenBit(FormatTier::Hdr, *accepted, vrr, ops.stageHdr)) {
            return true;
          }
          if (result.retry) {
            return false;
          }
          hdrFail = "HDR commit rejected by backend";
          ops.clearImageDescription();
          return false;
        }

        hdrFail = "backend rejected all 10-bit HDR render formats";
        ops.clearImageDescription();
        return false;
      };

      // SDR10.
      const auto trySdr10Formats = [&]() -> bool {
        if (params.bitDepth != 10) {
          return false;
        }

        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          const auto accepted = selectSdr10RenderFormat(params.currentRenderFormat, [&](uint32_t fmt) {
            return stage(ops.stageSdr, fmt, vrr) && ops.test();
          });
          if (!accepted) {
            continue;
          }
          if (commitTenBit(FormatTier::Sdr10, *accepted, vrr, ops.stageSdr)) {
            return true;
          }
          if (result.retry) {
            return false;
          }
          sdr10Fail = "10-bit SDR commit rejected by backend";
          return false;
        }

        sdr10Fail = "backend rejected all 10-bit SDR render formats";
        return false;
      };

      // Test SDR8 too, so a rejected swapchain reaches capability fallbacks
      // instead of becoming a retryable scene-build failure.
      const auto trySdr8Formats = [&]() -> bool {
        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          if (!stage(ops.stageSdr, DRM_FORMAT_XRGB8888, vrr) || !ops.test()) {
            continue;
          }
          if (commitStaged(FormatTier::Sdr8, DRM_FORMAT_XRGB8888, vrr)) {
            return true;
          }
          if (result.retry) {
            return false;
          }
        }
        return false;
      };

      if (tryHdrFormats()) {
        return true;
      }
      if (result.retry) {
        return false;
      }
      if (trySdr10Formats()) {
        return true;
      }
      return !result.retry && trySdr8Formats();
    }

  } // namespace

  FormatSequenceResult runFormatSequence(const FormatSequenceParams& params, const FormatSequenceOps& ops) {
    FormatSequenceResult result;
    std::string_view hdrFail = params.earlyHdrFail;
    std::string_view sdr10Fail;

    const auto rerun = [&] {
      hdrFail = params.earlyHdrFail;
      sdr10Fail = {};
      bool staged = false;
      result.committed =
          runSequenceForCurrentMode(params, ops, ModesetPolicy::Allowed, result, hdrFail, sdr10Fail, staged);
    };
    const auto rerunOnMode = [&](wlr_output_mode* mode) {
      ops.stageMode(mode);
      rerun();
    };

    // A modeset blocks the main thread (eDP link training, panel power cycle); the kernel can often change only the
    // refresh without one.
    if (params.refreshOnlyChange) {
      bool liveStaged = false;
      result.committed = runSequenceForCurrentMode(
          params, ops, ModesetPolicy::Forbidden, result, hdrFail, sdr10Fail, liveStaged
      );
      result.refreshPass = result.committed ? RefreshPass::Committed
          : result.retry                    ? RefreshPass::Busy
          : liveStaged                      ? RefreshPass::Rejected
                                            : RefreshPass::NoLiveCandidate;
      if (!result.committed && !result.retry) {
        rerun();
      }
    } else {
      rerun();
    }

    if (!result.committed && !result.retry && params.keptLiveMode != nullptr) {
      result.usedLiveModeRetry = true;
      rerunOnMode(params.keptLiveMode);
    }

    const bool modeStaged = params.configuredModeSpec != nullptr || !params.automaticMode.empty();
    if (!result.committed && !result.retry && modeStaged && params.preferredMode != nullptr) {
      result.usedModeFallback = true;
      if (!params.modeFallbackAlreadyWarned) {
        result.modeFallbackWarnedNow = true;
        const OutputMode* configured = params.configuredModeSpec;
        const std::string requested = configured == nullptr
            ? std::format("automatic {}", params.automaticMode)
            : configured->refreshMHz != 0
            ? std::format("configured {}x{}@{}mHz", configured->width, configured->height, configured->refreshMHz)
            : std::format("configured {}x{}", configured->width, configured->height);
        ops.warnModeFallback(requested, *params.preferredMode);
      }

      rerunOnMode(params.preferredMode);
    }

    result.hdrFail = hdrFail;
    result.sdr10Fail = sdr10Fail;
    return result;
  }

} // namespace andromeda

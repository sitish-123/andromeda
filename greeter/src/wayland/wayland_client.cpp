#include "wayland/wayland_client.h"

#include "config/output_identity.h"
#include "core/log.h"
#include "fractional-scale-v1-client-protocol.h"
#include "greeter/greeter_preferences.h"
#include "viewporter-client-protocol.h"
#include "wayland/output_span_layout.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  constexpr Logger kLog("wayland");

  static_assert(WL_OUTPUT_TRANSFORM_90 == 1);
  static_assert(WL_OUTPUT_TRANSFORM_270 == 3);
  static_assert(WL_OUTPUT_TRANSFORM_FLIPPED_90 == 5);
  static_assert(WL_OUTPUT_TRANSFORM_FLIPPED_270 == 7);

  [[nodiscard]] float outputScaleFactor(const WaylandOutputInfo& out) noexcept {
    if (out.preferredScale > 0.0f) {
      return out.preferredScale;
    }
    return static_cast<float>(std::max(1, out.scale));
  }

  std::optional<std::pair<std::uint32_t, std::uint32_t>> logicalSizeForOutputInfo(const WaylandOutputInfo& out) {
    if (!out.done || out.pixelWidth <= 0 || out.pixelHeight <= 0) {
      return std::nullopt;
    }

    const auto [pixelWidth, pixelHeight] =
        greeter::orientedOutputPixelSize(out.pixelWidth, out.pixelHeight, out.transform);

    const float scale = outputScaleFactor(out);
    if (scale <= 1.01f) {
      return std::pair{static_cast<std::uint32_t>(pixelWidth), static_cast<std::uint32_t>(pixelHeight)};
    }

    const auto logicalWidth =
        static_cast<std::uint32_t>(std::max(1, static_cast<std::int32_t>(static_cast<float>(pixelWidth) / scale)));
    const auto logicalHeight =
        static_cast<std::uint32_t>(std::max(1, static_cast<std::int32_t>(static_cast<float>(pixelHeight) / scale)));
    return std::pair{logicalWidth, logicalHeight};
  }

  [[nodiscard]] bool allReadyOutputsShareOrigin(const std::vector<WaylandOutputInfo>& outputs) {
    std::size_t readyCount = 0;
    for (const auto& out : outputs) {
      if (!out.done || out.pixelWidth <= 0 || out.pixelHeight <= 0) {
        continue;
      }
      ++readyCount;
      if (out.x != 0 || out.y != 0) {
        return false;
      }
    }
    return readyCount > 1;
  }

  [[nodiscard]] int
  outputLayoutIndex(const std::vector<greeter::GreeterOutputPlacement>& layout, const WaylandOutputInfo& output) {
    for (std::size_t i = 0; i < layout.size(); ++i) {
      if (output.matchesIdentifier(layout[i].name)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  [[nodiscard]] std::optional<WaylandOutputLayout> configuredLayoutForOutput(
      const WaylandOutputInfo& output, const std::vector<greeter::GreeterOutputPlacement>& layout
  ) {
    if (layout.empty()) {
      return std::nullopt;
    }

    for (const auto& placement : layout) {
      if (!output.matchesIdentifier(placement.name)) {
        continue;
      }
      const auto logical = logicalSizeForOutputInfo(output);
      if (!logical) {
        return std::nullopt;
      }
      return WaylandOutputLayout{
          .x = placement.x,
          .y = placement.y,
          .width = logical->first,
          .height = logical->second,
      };
    }

    return std::nullopt;
  }

  [[nodiscard]] std::size_t readyOutputCount(const std::vector<WaylandOutputInfo>& outputs) {
    std::size_t count = 0;
    for (const auto& out : outputs) {
      if (out.done && out.pixelWidth > 0 && out.pixelHeight > 0) {
        ++count;
      }
    }
    return count;
  }

  void logWaylandConnectDiagnostics() {
    const char* wlDisplay = std::getenv("WAYLAND_DISPLAY");
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      kLog.warn("XDG_RUNTIME_DIR is unset");
      return;
    }

    struct stat runtimeStat{};
    if (stat(runtimeDir, &runtimeStat) != 0) {
      kLog.warn("XDG_RUNTIME_DIR missing or inaccessible: {} (errno={} '{}')", runtimeDir, errno, std::strerror(errno));
    } else if (!S_ISDIR(runtimeStat.st_mode)) {
      kLog.warn("XDG_RUNTIME_DIR is not a directory: {}", runtimeDir);
    }

    if (wlDisplay == nullptr || wlDisplay[0] == '\0') {
      kLog.warn("WAYLAND_DISPLAY is unset");
      return;
    }

    const std::string socketPath = std::string(runtimeDir) + "/" + wlDisplay;
    struct stat socketStat{};
    if (stat(socketPath.c_str(), &socketStat) == 0) {
      kLog.info("Wayland socket present: {}", socketPath);
      return;
    }

    kLog.warn("Wayland socket missing: {} (errno={} '{}')", socketPath, errno, std::strerror(errno));

    DIR* dir = opendir(runtimeDir);
    if (dir == nullptr) {
      kLog.warn("cannot list {} (errno={} '{}')", runtimeDir, errno, std::strerror(errno));
      return;
    }

    std::string entries;
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') {
        continue;
      }
      if (!entries.empty()) {
        entries += ", ";
      }
      entries += entry->d_name;
    }
    closedir(dir);
    kLog.warn("XDG_RUNTIME_DIR contents: {}", entries.empty() ? "(empty)" : entries);
  }
} // namespace

std::string WaylandOutputInfo::stableIdentifier() const {
  char identifier[512];
  if (greeter_output_stable_identifier(
          name.c_str(), make.c_str(), model.c_str(), nullptr, description.c_str(), identifier, sizeof(identifier)
      )) {
    return identifier;
  }
  return {};
}

bool WaylandOutputInfo::matchesIdentifier(const std::string_view identifier) const {
  const std::string value(identifier);
  return greeter_output_identifier_matches(
      name.c_str(), make.c_str(), model.c_str(), nullptr, description.c_str(), value.c_str()
  );
}

WaylandClient::WaylandClient() = default;

WaylandClient::~WaylandClient() { disconnect(); }



void WaylandClient::setOutputsChangedCallback(std::function<void()> callback) {
  m_outputsChangedCallback = std::move(callback);
}

void WaylandClient::setPreferredOutputName(std::optional<std::string> name) {
  if (name.has_value() && name->empty()) {
    m_preferredOutputName = std::nullopt;
    return;
  }
  m_preferredOutputName = std::move(name);
}

const WaylandOutputInfo* WaylandClient::findOutputByName(const std::string_view name) const noexcept {
  if (name.empty()) {
    return nullptr;
  }
  for (const auto& out : m_outputs) {
    if (out.done && out.matchesIdentifier(name)) {
      return &out;
    }
  }
  return nullptr;
}

const WaylandOutputInfo* WaylandClient::preferredOutput() const noexcept {
  if (!hasResolvedPreferredOutput()) {
    return nullptr;
  }
  return findOutputByName(*m_preferredOutputName);
}

const WaylandOutputInfo* WaylandClient::primaryOutput() const noexcept {
  const WaylandOutputInfo* best = nullptr;
  std::int64_t bestArea = 0;
  for (const auto& out : m_outputs) {
    if (!out.done || out.pixelWidth <= 0 || out.pixelHeight <= 0) {
      continue;
    }
    const std::int64_t area = static_cast<std::int64_t>(out.pixelWidth) * static_cast<std::int64_t>(out.pixelHeight);
    if (area > bestArea) {
      bestArea = area;
      best = &out;
    }
  }
  return best;
}

int32_t WaylandClient::effectiveBufferScale() const noexcept {
  int32_t maxScale = 1;
  for (const auto& out : m_outputs) {
    if (!out.done || out.pixelWidth <= 0 || out.pixelHeight <= 0) {
      continue;
    }

    const float scale = outputScaleFactor(out);
    if (scale <= 1.01f) {
      continue;
    }

    maxScale = std::max(maxScale, static_cast<int32_t>(std::lround(std::ceil(scale))));
  }

  return maxScale;
}

int32_t WaylandClient::preferredBufferScale() const noexcept {
  const WaylandOutputInfo* out = preferredOutput();
  if (out == nullptr) {
    return effectiveBufferScale();
  }
  return outputBufferScale(out->output);
}

int32_t WaylandClient::outputBufferScale(const wl_output* output) const noexcept {
  if (output == nullptr) {
    return effectiveBufferScale();
  }

  for (const auto& out : m_outputs) {
    if (out.output != output || !out.done || out.pixelWidth <= 0 || out.pixelHeight <= 0) {
      continue;
    }

    const float scale = outputScaleFactor(out);
    if (scale <= 1.01f) {
      return 1;
    }
    return static_cast<int32_t>(std::lround(std::ceil(scale)));
  }

  return effectiveBufferScale();
}

void WaylandClient::setOutputPreferredScale(wl_output* output, float scale) noexcept {
  if (output == nullptr || scale <= 0.0f) {
    return;
  }
  for (auto& out : m_outputs) {
    if (out.output != output) {
      continue;
    }
    if (std::fabs(out.preferredScale - scale) < 0.01f) {
      return;
    }
    out.preferredScale = scale;
    if (out.done) {
      notifyOutputsChanged();
    }
    break;
  }
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> WaylandClient::primaryLogicalSize() const noexcept {
  const WaylandOutputInfo* out = primaryOutput();
  if (out == nullptr) {
    return std::nullopt;
  }
  return logicalSizeForOutputInfo(*out);
}

std::optional<std::string> WaylandClient::primaryConnectorName() const noexcept {
  const WaylandOutputInfo* out = primaryOutput();
  if (out == nullptr || out->name.empty()) {
    return std::nullopt;
  }
  return out->name;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> WaylandClient::preferredLogicalSize() const noexcept {
  const WaylandOutputInfo* out = preferredOutput();
  if (out == nullptr) {
    return std::nullopt;
  }
  return logicalSizeForOutputInfo(*out);
}

bool WaylandClient::hasPreferredOutputName() const noexcept {
  return m_preferredOutputName.has_value() && !m_preferredOutputName->empty();
}

bool WaylandClient::hasReadyOutputs() const noexcept { return readyOutputCount(m_outputs) > 0; }

std::vector<const WaylandOutputInfo*> WaylandClient::readyOutputsSorted() const noexcept {
  std::vector<const WaylandOutputInfo*> ready;
  ready.reserve(m_outputs.size());
  for (const auto& out : m_outputs) {
    if (out.done && out.pixelWidth > 0 && out.pixelHeight > 0) {
      ready.push_back(&out);
    }
  }
  std::sort(ready.begin(), ready.end(), [this](const WaylandOutputInfo* lhs, const WaylandOutputInfo* rhs) {
    if (!m_outputLayout.empty()) {
      const int leftIndex = outputLayoutIndex(m_outputLayout, *lhs);
      const int rightIndex = outputLayoutIndex(m_outputLayout, *rhs);
      if (leftIndex >= 0 && rightIndex >= 0) {
        return leftIndex < rightIndex;
      }
      if (leftIndex >= 0) {
        return true;
      }
      if (rightIndex >= 0) {
        return false;
      }
    }
    return lhs->name < rhs->name;
  });
  return ready;
}

std::vector<const WaylandOutputInfo*> WaylandClient::greeterTargetOutputs() const noexcept {
  if (hasResolvedPreferredOutput()) {
    if (const WaylandOutputInfo* out = preferredOutput()) {
      return {out};
    }
    return {};
  }
  return readyOutputsSorted();
}

bool WaylandClient::hasResolvedPreferredOutput() const noexcept {
  return hasPreferredOutputName() && findOutputByName(*m_preferredOutputName) != nullptr;
}

void WaylandClient::forgetPreferredOutput() noexcept { m_preferredOutputName = std::nullopt; }

std::optional<WaylandOutputLayout> WaylandClient::layoutForOutput(const WaylandOutputInfo& output) const noexcept {
  const auto logical = logicalSizeForOutputInfo(output);
  if (!logical) {
    return std::nullopt;
  }

  if (const auto configured = configuredLayoutForOutput(output, m_outputLayout)) {
    kLog.info(
        "output '{}' configured layout at ({},{}) {}x{}", output.name.empty() ? "?" : output.name.c_str(),
        configured->x, configured->y, configured->width, configured->height
    );
    return configured;
  }

  const bool synthesizeLayout = !m_outputLayout.empty() || allReadyOutputsShareOrigin(m_outputs);
  if (synthesizeLayout) {
    const std::vector<const WaylandOutputInfo*> ordered = readyOutputsSorted();
    std::int64_t syntheticX = 0;

    // Match the compositor's placement of connectors omitted from a partial
    // configured layout: start them after the rightmost configured output,
    // then chain the omitted connectors by name.
    if (!m_outputLayout.empty()) {
      for (const WaylandOutputInfo* candidate : ordered) {
        const int placementIndex = outputLayoutIndex(m_outputLayout, *candidate);
        if (placementIndex < 0) {
          continue;
        }
        if (const auto candidateLogical = logicalSizeForOutputInfo(*candidate)) {
          syntheticX = std::max(
              syntheticX,
              static_cast<std::int64_t>(m_outputLayout[static_cast<std::size_t>(placementIndex)].x)
                  + static_cast<std::int64_t>(candidateLogical->first)
          );
        }
      }
    }

    for (const WaylandOutputInfo* candidate : ordered) {
      if (outputLayoutIndex(m_outputLayout, *candidate) >= 0) {
        continue;
      }
      if (candidate->output == output.output) {
        break;
      }
      if (const auto candidateLogical = logicalSizeForOutputInfo(*candidate)) {
        syntheticX += static_cast<std::int64_t>(candidateLogical->first);
      }
    }
    if (syntheticX < std::numeric_limits<std::int32_t>::min()
        || syntheticX > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    const auto x = static_cast<std::int32_t>(syntheticX);
    kLog.info(
        "output '{}' synthetic layout at ({},0) {}x{}", output.name.empty() ? "?" : output.name.c_str(), x,
        logical->first, logical->second
    );
    return WaylandOutputLayout{
        .x = x,
        .y = 0,
        .width = logical->first,
        .height = logical->second,
    };
  }

  return WaylandOutputLayout{
      .x = output.x,
      .y = output.y,
      .width = logical->first,
      .height = logical->second,
  };
}

std::optional<WaylandOutputLayout> WaylandClient::preferredOutputLayout() const noexcept {
  const WaylandOutputInfo* out = preferredOutput();
  if (out == nullptr) {
    return std::nullopt;
  }
  return layoutForOutput(*out);
}

std::optional<WaylandOutputLayout> WaylandClient::greeterOutputLayout() const noexcept {
  if (hasResolvedPreferredOutput()) {
    return preferredOutputLayout();
  }
  if (readyOutputCount(m_outputs) <= 1) {
    return std::nullopt;
  }
  const WaylandOutputInfo* out = primaryOutput();
  if (out == nullptr) {
    return std::nullopt;
  }
  return layoutForOutput(*out);
}

bool WaylandClient::needsOutputViewport() const noexcept {
  if (readyOutputCount(m_outputs) <= 1 || !hasPreferredOutputName()) {
    return false;
  }
  const auto layout = greeterOutputLayout();
  const auto combined = combinedLogicalSize();
  if (!layout || !combined) {
    return false;
  }
  return combined->first != layout->width || combined->second != layout->height;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> WaylandClient::combinedLogicalSize() const noexcept {
  std::size_t readyCount = 0;
  bool hasNonOriginPlacement = false;
  for (const auto& out : m_outputs) {
    if (!out.done || out.pixelWidth <= 0 || out.pixelHeight <= 0) {
      continue;
    }
    ++readyCount;
    if (out.x != 0 || out.y != 0) {
      hasNonOriginPlacement = true;
    }
  }

  if (readyCount == 0) {
    return std::nullopt;
  }

  if (readyCount == 1) {
    for (const auto& out : m_outputs) {
      if (out.done && out.pixelWidth > 0 && out.pixelHeight > 0) {
        return logicalSizeForOutputInfo(out);
      }
    }
    return std::nullopt;
  }

  if (!hasNonOriginPlacement) {
    std::uint32_t totalWidth = 0;
    std::uint32_t maxHeight = 0;
    for (const auto& out : m_outputs) {
      const auto logical = logicalSizeForOutputInfo(out);
      if (!logical) {
        continue;
      }
      totalWidth += logical->first;
      maxHeight = std::max(maxHeight, logical->second);
    }
    if (totalWidth > 0 && maxHeight > 0) {
      return std::pair{totalWidth, maxHeight};
    }
  }

  bool any = false;
  std::int64_t minX = 0;
  std::int64_t minY = 0;
  std::int64_t maxX = 0;
  std::int64_t maxY = 0;
  for (const auto& out : m_outputs) {
    const auto layout = layoutForOutput(out);
    if (!layout) {
      continue;
    }

    const std::int64_t x0 = layout->x;
    const std::int64_t y0 = layout->y;
    const std::int64_t x1 = x0 + static_cast<std::int64_t>(layout->width);
    const std::int64_t y1 = y0 + static_cast<std::int64_t>(layout->height);
    if (!any) {
      minX = x0;
      minY = y0;
      maxX = x1;
      maxY = y1;
      any = true;
    } else {
      minX = std::min(minX, x0);
      minY = std::min(minY, y0);
      maxX = std::max(maxX, x1);
      maxY = std::max(maxY, y1);
    }
  }

  if (!any || maxX <= minX || maxY <= minY) {
    return std::nullopt;
  }

  return std::pair{static_cast<std::uint32_t>(maxX - minX), static_cast<std::uint32_t>(maxY - minY)};
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> WaylandClient::targetLogicalSize() const noexcept {
  if (needsOutputViewport()) {
    return combinedLogicalSize();
  }
  if (hasResolvedPreferredOutput()) {
    return preferredLogicalSize();
  }
  return primaryLogicalSize();
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
WaylandClient::logicalSizeForOutput(const wl_output* output) const noexcept {
  if (output == nullptr) {
    return primaryLogicalSize();
  }

  for (const auto& out : m_outputs) {
    if (out.output == output) {
      return logicalSizeForOutputInfo(out);
    }
  }
  return std::nullopt;
}

std::optional<WaylandOutputLayout> WaylandClient::logicalLayoutForOutput(const wl_output* output) const noexcept {
  if (output == nullptr) {
    return std::nullopt;
  }

  for (const auto& out : m_outputs) {
    if (out.output == output) {
      return layoutForOutput(out);
    }
  }
  return std::nullopt;
}

void WaylandClient::notifyOutputsChanged() {
  refreshOutputs();
  if (m_outputsChangedCallback) {
    m_outputsChangedCallback();
  }
}

bool WaylandClient::connect() {
  if (m_connection != nullptr) {
    return true;
  }

  m_outputLayout = greeter::loadGreeterOutputLayout();
  auto connection = std::make_unique<WaylandConnection>();
  connection->setOutputChangeCallback([this]() { notifyOutputsChanged(); });
  connection->setOutputLifecycleCallbacks(nullptr, [this](wl_output* /*output*/) { notifyOutputsChanged(); });
  try {
    connection->connect();
  } catch (const std::exception& e) {
    kLog.error("Wayland connect failed: {}", e.what());
    logWaylandConnectDiagnostics();
    return false;
  }
  if (connection->compositor() == nullptr || connection->seat() == nullptr || connection->xdgWmBase() == nullptr) {
    kLog.error("missing required Wayland globals (compositor/seat/xdg-shell)");
    return false;
  }

  m_connection = std::move(connection);
  refreshOutputs();
  kLog.info("connected to Wayland compositor");
  return true;
}

void WaylandClient::disconnect() {
  m_outputs.clear();
  m_connection.reset();
}

int WaylandClient::dispatch() const { return display() != nullptr ? wl_display_dispatch(display()) : -1; }

int WaylandClient::dispatchPending() const {
  return display() != nullptr ? wl_display_dispatch_pending(display()) : -1;
}

int WaylandClient::flush() const { return display() != nullptr ? wl_display_flush(display()) : -1; }

void WaylandClient::setPointerEventCallback(WaylandSeat::PointerEventCallback callback) {
  if (m_connection != nullptr) {
    m_connection->setPointerEventCallback(std::move(callback));
  }
}

void WaylandClient::setKeyboardEventCallback(WaylandSeat::KeyboardEventCallback callback) {
  if (m_connection != nullptr) {
    m_connection->setKeyboardEventCallback(std::move(callback));
  }
}

int WaylandClient::repeatPollTimeoutMs() const { return m_connection ? m_connection->repeatPollTimeoutMs() : -1; }

void WaylandClient::repeatTick() {
  if (m_connection != nullptr) {
    m_connection->repeatTick();
  }
}

void WaylandClient::refreshOutputs() {
  std::vector<WaylandOutputInfo> next;
  if (m_connection != nullptr) {
    for (const WaylandOutput& out : m_connection->outputs()) {
      WaylandOutputInfo info{
          .output = out.output,
          .registryName = out.name,
          .name = out.connectorName,
          .make = out.make,
          .model = out.model,
          .description = out.description,
          .x = out.logicalX,
          .y = out.logicalY,
          .pixelWidth = out.width,
          .pixelHeight = out.height,
          .scale = out.scale,
          .transform = out.transform,
          .done = out.done,
      };
      const auto previous = std::ranges::find(m_outputs, out.output, &WaylandOutputInfo::output);
      if (previous != m_outputs.end()) {
        info.preferredScale = previous->preferredScale;
      }
      next.push_back(std::move(info));
    }
  }
  m_outputs = std::move(next);
}

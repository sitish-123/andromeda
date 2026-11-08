#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class Flex;
class Label;
class Node;

// Compress sheet (mockup files-v2-live.html §9, rulebook 9.10 dialog): name, format (ZIP, 7z, tar.zst), an
// optional password for ZIP and 7z, split size and a live size estimate. Compress starts one FileOps1 job next to
// the first item.
class CompressSheet {
public:
  // destination = the archive file; format = "zip", "7z" or "tar.zst"; password empty = none; volumeSize 0 = one file.
  using Compress = std::function<void(const std::filesystem::path& destination, const std::string& format,
                                      const std::string& password, std::uint64_t volumeSize)>;
  // Source bytes by how well they pack, summed on a worker.
  struct Totals {
    std::uint64_t packed = 0; // already compressed: photos, video, archives
    std::uint64_t text = 0;
    std::uint64_t other = 0;
  };

  CompressSheet(std::vector<std::filesystem::path> sources, Compress compress, std::function<void()> close,
                std::function<void()> rebuild);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void submit();
  // E2E: name, format (zip, 7z, tar.zst), password or split (none, 10m, 25m, 100m, 700m, 4g); the sheet rebuilds.
  void set(const std::string& field, const std::string& value);
  // The archive the sheet would write now.
  [[nodiscard]] std::filesystem::path destination() const;
  // "About 940 MB", "About 940 MB in 10 parts", or "Estimating…" while the worker sums the sources.
  [[nodiscard]] std::string estimate() const;

private:
  void update();

  std::vector<std::filesystem::path> m_sources;
  Compress m_compress;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  std::string m_name;
  std::size_t m_format = 0; // index into the format list
  std::string m_password;
  std::size_t m_split = 0; // index into the split sizes
  std::optional<Totals> m_totals;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

  Label* m_note = nullptr;
  Label* m_estimate = nullptr;
  Button* m_ok = nullptr;
};

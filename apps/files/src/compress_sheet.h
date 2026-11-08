#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Button;
class Flex;
class Label;
class Node;

// Compress sheet (mockup files-v2-live.html §9, rulebook 9.10 dialog): name, format (ZIP, 7z, tar.zst) and an
// optional password for ZIP and 7z. Compress starts one FileOps1 job next to the first item.
class CompressSheet {
public:
  // destination = the archive file; format = "zip", "7z" or "tar.zst"; password empty = none.
  using Compress = std::function<void(const std::filesystem::path& destination, const std::string& format,
                                      const std::string& password)>;

  CompressSheet(std::vector<std::filesystem::path> sources, Compress compress, std::function<void()> close,
                std::function<void()> rebuild);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void submit();
  // The archive the sheet would write now.
  [[nodiscard]] std::filesystem::path destination() const;

private:
  void update();

  std::vector<std::filesystem::path> m_sources;
  Compress m_compress;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  std::string m_name;
  std::size_t m_format = 0; // index into the format list
  std::string m_password;

  Label* m_note = nullptr;
  Button* m_ok = nullptr;
};

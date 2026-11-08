#pragma once

#include "smart_folders.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Button;
class Label;
class Node;

// Smart Folder sheet (mockup files-v2-live.html §16 "Smart Folder", FILES-UI-SPEC §18): the criteria of a saved
// search as rows, the plain sentence they make, and Save to Sidebar.
class SmartFolderSheet {
public:
  // `folder` = the starting criteria; `here` = the folder offered as "Where" besides This PC; `original` = the name
  // being edited ("" = a new smart folder).
  SmartFolderSheet(SmartFolder folder, std::filesystem::path here, std::string original,
                   std::function<void(const SmartFolder&, const std::string& original)> save,
                   std::function<void()> close, std::function<void()> rebuild);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void set(const std::string& field, const std::string& value); // E2E: name, text, kind, days, where
  void submit();

private:
  void update();
  [[nodiscard]] std::string error() const;

  SmartFolder m_folder;
  std::filesystem::path m_here;
  std::string m_original;
  std::function<void(const SmartFolder&, const std::string&)> m_save;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  Label* m_sentence = nullptr;
  Button* m_ok = nullptr;
};

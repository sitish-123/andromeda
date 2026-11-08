#pragma once

#include "tags.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Label;
class Node;

// Tags sheet (mockup files-v2-live.html §17, FILES-UI-SPEC §17): every tag with its colour and item count; the
// selected one gets a name field, colour swatches, order and delete. Each change saves tags.json at once (Finder).
class TagManagerSheet {
public:
  struct Host {
    std::function<std::size_t(const std::string&)> count;
    std::function<std::vector<std::string>(const std::vector<TagDef>&)> unlisted;
    // Rewrites the tag on every file that has it (one undo step each).
    std::function<void(const std::string& old, const std::string& now)> renameOnFiles;
    std::function<void(const std::string& tag)> removeFromFiles;
    std::function<void()> configChanged; // the sidebars follow tags.json
    std::function<void()> close;
    std::function<void()> rebuild;
    std::function<void(class Input&)> focus;
  };

  explicit TagManagerSheet(Host host);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  // After a tag walk: counts change in place; tags found on files for the first time get rows.
  void indexChanged();

  // The sheet's edits, also driven by E2E actions. False = refused (unknown tag, bad or taken name).
  bool select(const std::string& name);
  bool rename(const std::string& old, const std::string& now);
  bool move(const std::string& name, int delta);
  bool setSidebar(const std::string& name, bool shown);
  bool setColor(const std::string& name, std::uint32_t rgb);
  bool remove(const std::string& name);
  void addTag();
  void done();

  // Finder's colours, then grey.
  static constexpr std::uint32_t kColors[] = {0xd93025, 0xe46b2c, 0xe6c229, 0x1a7f37, 0x5147e5, 0x8a4fd8, 0x8e8e93};

private:
  [[nodiscard]] std::unique_ptr<Node> buildRow(std::size_t index, float s);
  [[nodiscard]] std::unique_ptr<Node> buildEditor(float s);
  [[nodiscard]] std::size_t find(const std::string& name) const;
  void save();
  void commitDraft();
  void rebuildLater();

  Host m_host;
  std::vector<TagDef> m_tags;
  std::size_t m_selected = 0;
  std::string m_draft; // the name field of the selected tag
  std::string m_error;
  bool m_confirmDelete = false;
  bool m_focusName = false;
  std::map<std::string, Label*> m_counts;
  // Old names of renamed and deleted tags: files keep them until their job lands, so a walk must not add them back.
  std::vector<std::string> m_retired;
};

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Button;
class Node;

// Extract sheet (mockup files-v2-live.html §9): destination with a smart-folder note, "Show the folder when done",
// "Move the archive to Trash after extracting". The same sheet asks for the password when an Extract job ends with
// "password needed" (State::PasswordNeeded) or "wrong password" (State::WrongPassword).
class ExtractSheet {
public:
  enum class State : std::uint8_t { Normal, PasswordNeeded, WrongPassword };
  struct Request {
    std::filesystem::path destination;
    std::string password;
    bool showWhenDone = true;
    bool trashArchive = false;
  };
  using Extract = std::function<void(const Request& request)>;
  // Opens a folder picker; the sheet takes the picked folder.
  using ChooseFolder = std::function<void(std::function<void(const std::filesystem::path&)> picked)>;

  // topNames: ArchiveListing::topNames(), for the smart note; empty when not known.
  ExtractSheet(std::filesystem::path archive, std::vector<std::string> topNames, Extract extract, ChooseFolder choose,
               std::function<void()> close, std::function<void()> rebuild);

  void setState(State state) { m_state = state; }
  void setDestination(std::filesystem::path folder) { m_destination = std::move(folder); }
  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void submit();
  // "The archive has one top folder, …": the note under the destination.
  [[nodiscard]] std::string smartNote() const;

private:
  std::filesystem::path m_archive;
  std::vector<std::string> m_topNames;
  Extract m_extract;
  ChooseFolder m_choose;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  State m_state = State::Normal;
  Request m_request;
  std::filesystem::path m_destination;
  Button* m_ok = nullptr;
};

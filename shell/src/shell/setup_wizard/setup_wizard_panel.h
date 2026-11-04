#pragma once

#include "core/timer_manager.h"
#include "shell/panel/panel.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class Button;
class CountdownRingNode;
class Flex;
class Glyph;
class Image;
class Label;
class LinkService;
class Node;
class Select;
class SystemBus;
class Toggle;
class WaylandConnection;
enum class PaletteSource : std::uint8_t;

namespace sdbus {
  class IProxy;
}

class SetupWizardPanel : public Panel {
public:
  SetupWizardPanel(ConfigService* config, WaylandConnection* wayland, LinkService* link, SystemBus* systemBus);
  ~SetupWizardPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override { return scaled(640.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(560.0F); }
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] bool padsContent() const noexcept override { return false; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }

  static bool isFirstRun(const ConfigService& config);

  static constexpr int kPageCount = 7;

private:
  // A node drawn over `host` (selection ring, hit area), outset by `outset` on every side.
  struct Overlay {
    Node* node = nullptr;
    Node* host = nullptr;
    float outset = 0.0F;
  };
  // Mode illustration parts, placed from the art box size in doLayout.
  struct ModeArt {
    Flex* box = nullptr;
    Image* wallpaper = nullptr;
    std::vector<Flex*> parts;
    int mode = 0;
  };

  enum class PhoneState : std::uint8_t { NotRunning, Waiting, Paired, Failed };
  enum class SearchState : std::uint8_t { NotRunning, NoProgress };
  enum class UpdateState : std::uint8_t { Idle, Checking, UpToDate, Available, Failed };
  enum class Method : std::uint8_t { None, Face, Finger };

  // One unlock method row: hardware found, prints enrolled, last error.
  struct UnlockMethod {
    bool present = false;
    int enrolled = 0;
    bool failed = false;
    Flex* row = nullptr;
    Label* sub = nullptr;
    Button* setUp = nullptr;
    Flex* on = nullptr;
  };

  void doLayout(Renderer& renderer, float width, float height) override;
  std::unique_ptr<Flex> buildWelcome(float scale);
  std::unique_ptr<Flex> buildAppearance(float scale);
  std::unique_ptr<Flex> buildPhone(float scale);
  std::unique_ptr<Flex> buildUnlock(float scale);
  std::unique_ptr<Flex> buildSearch(float scale);
  std::unique_ptr<Flex> buildUpdate(float scale);
  std::unique_ptr<Flex> buildDone(float scale);
  std::unique_ptr<Flex> makePage(int page, float scale, std::string_view titleKey, std::string_view bodyKey);
  std::unique_ptr<Flex> makeAlertRow(float scale, Label** textOut, std::function<void()> onRetry);
  std::unique_ptr<Flex> makeMethodRow(float scale, Method method);
  std::unique_ptr<Flex> makeThumb(float scale, std::string_view label, std::function<void()> onClick, Flex** frameOut,
                                  Flex** ringOut, Label** labelOut);
  void addOverlay(Node* node, Node* host, float outset);
  void layoutModeArt(const ModeArt& art);
  [[nodiscard]] bool pageShown(int page) const;
  void setStep(int step);
  void advance(int direction);
  void syncSelection();
  void syncPages();
  void refreshUi();
  void chooseWallpaper();
  void commit();

  void enterPage(int page);
  void leavePage(int page);
  void syncPhone();
  void renderQr(Renderer& renderer);
  void startPairing();
  void probeUnlock();
  void probeFace();
  void probeFinger();
  void startEnrol(Method method);
  void stopEnrol();
  void finishEnrol(bool ok);
  void releaseFace();
  void releaseFinger();
  void probeSearch();
  void checkUpdates();

  ConfigService* m_config = nullptr;
  LinkService* m_link = nullptr;
  SystemBus* m_systemBus = nullptr;
  std::string m_user;
  Flex* m_root = nullptr;
  Image* m_logo = nullptr;
  int m_step = 0;
  bool m_preview = false;
  std::array<Flex*, kPageCount> m_pages{};
  std::array<Flex*, kPageCount> m_dots{};
  Button* m_leadButton = nullptr;
  Button* m_laterButton = nullptr;
  Button* m_primaryButton = nullptr;
  Toggle* m_telemetryToggle = nullptr;

  std::vector<Overlay> m_overlays;
  std::vector<ModeArt> m_modeArts;
  std::array<Flex*, 3> m_modeRings{};
  std::array<Label*, 3> m_modeLabels{};
  int m_mode = 0;

  Flex* m_colourRow = nullptr;
  Node* m_matchSeparator = nullptr;
  std::vector<Flex*> m_swatchRings;
  std::vector<std::string> m_swatchNames;
  Toggle* m_matchToggle = nullptr;
  Select* m_schemeSelect = nullptr;
  PaletteSource m_paletteSource{};
  std::string m_builtinPalette;

  std::array<Flex*, 2> m_wallpaperRings{};
  std::array<Label*, 2> m_wallpaperLabels{};
  Image* m_bundledImage = nullptr;
  Image* m_pickedImage = nullptr;
  Node* m_addGlyph = nullptr;
  std::string m_pickedPath;
  bool m_pickedLoaded = false;
  int m_wallpaper = 0;
  std::string m_wallpaperDir;

  PhoneState m_phoneState = PhoneState::NotRunning;
  std::string m_phoneCode;
  std::string m_phoneDetail;
  bool m_pairingRequested = false;
  Timer m_phoneTimer;
  Flex* m_phoneCard = nullptr;
  Flex* m_phoneAlert = nullptr;
  Label* m_phoneAlertText = nullptr;
  Node* m_qrGlyph = nullptr;
  Flex* m_qrBox = nullptr;
  Image* m_qrImage = nullptr;
  std::string m_phoneUri;
  std::string m_qrUri;
  Node* m_pairedGlyph = nullptr;
  Label* m_phoneHeadline = nullptr;
  Label* m_phoneBody = nullptr;
  Label* m_phoneCodeLabel = nullptr;
  Flex* m_phoneStatus = nullptr;

  UnlockMethod m_face;
  UnlockMethod m_finger;
  Method m_enrolling = Method::None;
  float m_enrolProgress = 0.0F;
  std::string m_enrolPrompt;
  int m_fingerStages = 0;
  int m_fingerPassed = 0;
  std::string m_fingerDevice;
  std::vector<std::string> m_fingerNames;
  std::unique_ptr<sdbus::IProxy> m_gaze;
  std::unique_ptr<sdbus::IProxy> m_fprintDevice;
  bool m_faceClaimed = false;
  bool m_fingerClaimed = false;
  Flex* m_methodCard = nullptr;
  Node* m_unlockSeparator = nullptr;
  Flex* m_enrolCard = nullptr;
  Node* m_enrolFaceGlyph = nullptr;
  Node* m_enrolFingerGlyph = nullptr;
  CountdownRingNode* m_enrolRing = nullptr;
  Label* m_enrolHeadline = nullptr;
  Label* m_enrolPromptLabel = nullptr;

  SearchState m_searchState = SearchState::NotRunning;
  Flex* m_searchAlert = nullptr;
  Flex* m_searchInfo = nullptr;

  UpdateState m_updateState = UpdateState::Idle;
  std::string m_updateTitle;
  std::string m_updateSub;
  bool m_updateChecker = false;
  Flex* m_updateChecking = nullptr;
  Flex* m_updateResult = nullptr;
  Glyph* m_updateGlyph = nullptr;
  Label* m_updateTitleLabel = nullptr;
  Label* m_updateSubLabel = nullptr;
  Button* m_updateRetry = nullptr;
};

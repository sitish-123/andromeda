#pragma once

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
class Flex;
class Image;
class Label;
class Node;
class Select;
class Toggle;
class WaylandConnection;
enum class PaletteSource : std::uint8_t;

class SetupWizardPanel : public Panel {
public:
  SetupWizardPanel(ConfigService* config, WaylandConnection* /*wayland*/) : m_config(config) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(640.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(560.0F); }
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] bool padsContent() const noexcept override { return false; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }

  static bool isFirstRun(const ConfigService& config);

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

  void doLayout(Renderer& renderer, float width, float height) override;
  std::unique_ptr<Flex> buildWelcome(float scale);
  std::unique_ptr<Flex> buildAppearance(float scale);
  std::unique_ptr<Flex> buildDone(float scale);
  std::unique_ptr<Flex> makeThumb(float scale, std::string_view label, std::function<void()> onClick, Flex** frameOut,
                                  Flex** ringOut, Label** labelOut);
  void addOverlay(Node* node, Node* host, float outset);
  void layoutModeArt(const ModeArt& art);
  void setStep(int step);
  void syncSelection();
  void chooseWallpaper();
  void commit();

  ConfigService* m_config = nullptr;
  Flex* m_root = nullptr;
  Image* m_logo = nullptr;
  int m_step = 0;
  std::array<Flex*, 3> m_pages{};
  std::array<Flex*, 3> m_dots{};
  Button* m_leadButton = nullptr;
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
};

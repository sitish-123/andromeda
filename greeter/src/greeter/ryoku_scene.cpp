#include "greeter/ryoku_scene.h"

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <numbers>

namespace {
  constexpr char kFont[] = "Outfit Black";
  constexpr float kPi = std::numbers::pi_v<float>;

  constexpr Color kWhite = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  constexpr Color kDim = rgba(0.40f, 0.40f, 0.40f, 1.0f);
  constexpr Color kSub = rgba(0.333f, 0.333f, 0.333f, 1.0f);
  constexpr Color kWait = rgba(0.20f, 0.20f, 0.20f, 1.0f);
  constexpr Color kError = rgba(1.0f, 0.267f, 0.267f, 1.0f);

  // Ryoku tracks its capitals wide; Pango has no letter spacing here, so a space goes between the letters.
  std::string tracked(std::string_view text, std::string_view gap) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
      const auto lead = static_cast<unsigned char>(text[i]);
      const std::size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      if (!out.empty()) {
        out += gap;
      }
      out += text.substr(i, len);
      i += len;
    }
    return out;
  }

  std::string upper(std::string text) {
    for (char& c : text) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    return text;
  }

  constexpr std::string_view kThin = " ";
  constexpr std::string_view kHair = " ";

  Label* addLabel(Node& root, float size, const Color& color, int z) {
    auto label = std::make_unique<Label>();
    label->setFontFamily(kFont);
    label->setFontSize(size);
    label->setColor(color);
    label->setZIndex(z);
    return static_cast<Label*>(root.addChild(std::move(label)));
  }

  RectNode* addRect(Node& parent, const RoundedRectStyle& style, int z) {
    auto rect = std::make_unique<RectNode>();
    rect->setStyle(style);
    rect->setZIndex(z);
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

  float msOfDay() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&t, &local);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    return static_cast<float>((local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) * 1000 + ms);
  }
} // namespace

void RyokuScene::build(Node& root) {
  m_root = &root;
  m_background = addRect(root, RoundedRectStyle{.fill = rgba(0.0f, 0.0f, 0.0f, 1.0f)}, 0);
  buildRing(m_minutes, 18.0f, 10.0f, 22.0f);
  buildRing(m_seconds, 13.0f, 8.0f, 16.0f);
  // Below the rings, so the lit minute and second show inside the pill.
  m_pill = addRect(root, {}, 1);
  m_pillLine = addRect(root, {}, 1);
  m_hour = addLabel(root, 110.0f, kWhite, 5);
  m_date = addLabel(root, 13.0f, kSub, 5);
  m_weekday = addLabel(root, 18.0f, kWhite, 5);
  m_user = addLabel(root, 18.0f, kDim, 8);
  m_hint = addLabel(root, 10.0f, kDim, 8);
  m_mask = addLabel(root, 14.0f, kDim, 8);
  m_waiting = addLabel(root, 10.0f, kWait, 8);
  m_waiting->setText(tracked("WAITING FOR KEY", kHair));
  m_needle = addRect(root, RoundedRectStyle{.fill = kWhite}, 8);
  m_session = addLabel(root, 12.0f, kDim, 8);
  m_reboot = addLabel(root, 12.0f, kDim, 8);
  m_reboot->setText(tracked("REBOOT", kHair));
  m_shutdown = addLabel(root, 12.0f, kDim, 8);
  m_shutdown->setText(tracked("SHUTDOWN", kHair));
  for (Label* action : {m_session, m_reboot, m_shutdown}) {
    action->setHitTestVisible(true);
    action->setOnEnter([action](const auto&) { action->setColor(kWhite); });
    action->setOnLeave([action]() { action->setColor(kDim); });
  }
}

void RyokuScene::buildRing(Ring& ring, float tickLong, float tickShort, float numberSize) {
  ring.tickLong = tickLong;
  ring.tickShort = tickShort;
  ring.numberSize = numberSize;
  auto container = std::make_unique<Node>();
  container->setZIndex(2);
  container->setHitTestVisible(false);
  ring.container = m_root->addChild(std::move(container));
  for (RectNode*& tick : ring.ticks) {
    tick = addRect(*ring.container, RoundedRectStyle{.fill = kWhite}, 0);
  }
  for (std::size_t i = 0; i < ring.numbers.size(); ++i) {
    auto label = std::make_unique<Label>();
    label->setFontFamily(kFont);
    label->setFontSize(numberSize);
    label->setColor(kWhite);
    const std::size_t minute = i * 5;
    label->setText((minute < 10 ? "0" : "") + std::to_string(minute));
    ring.numbers[i] = static_cast<Label*>(ring.container->addChild(std::move(label)));
  }
}

void RyokuScene::layout(Renderer& renderer, float ox, float oy, float sw, float sh) {
  m_ox = ox;
  m_oy = oy;
  m_sw = sw;
  m_sh = sh;
  m_s = sh / 768.0f;
  const float s = m_s;

  m_background->setPosition(ox, oy);
  m_background->setSize(sw, sh);

  const float cx = ox + 40.0f * s;
  const float cy = oy + sh * 0.5f;
  m_minutes.radius = 320.0f * s;
  m_seconds.radius = 480.0f * s;
  layoutRing(renderer, m_minutes, cx, cy, 35.0f * s);
  layoutRing(renderer, m_seconds, cx, cy, 30.0f * s);

  const float pillX = cx + 230.0f * s;
  const float pillW = 330.0f * s;
  const float pillH = 90.0f * s;
  m_pill->setStyle(
      RoundedRectStyle{
          .fill = rgba(0.031f, 0.031f, 0.031f, 1.0f),
          .border = rgba(0.102f, 0.102f, 0.102f, 1.0f),
          .radius = pillH * 0.5f,
          .borderWidth = std::max(1.0f, s),
      }
  );
  m_pill->setPosition(pillX, cy - pillH * 0.5f);
  m_pill->setSize(pillW, pillH);
  m_pillLine->setStyle(RoundedRectStyle{.fill = rgba(0.133f, 0.133f, 0.133f, 1.0f)});
  m_pillLine->setPosition(pillX + 170.0f * s, cy - 17.5f * s);
  m_pillLine->setSize(std::max(1.0f, s), 35.0f * s);

  (void)tick();
  m_hour->setFontSize(110.0f * s);
  m_hour->setText(m_hourText);
  m_hour->measure(renderer);
  m_hour->setPosition(pillX - 40.0f * s - m_hour->width(), cy - m_hour->height() * 0.5f);

  m_date->setFontSize(13.0f * s);
  m_weekday->setFontSize(18.0f * s);
  m_date->measure(renderer);
  m_weekday->measure(renderer);
  const float dateX = pillX + pillW + 110.0f * s;
  const float dateBlock = m_date->height() + 5.0f * s + m_weekday->height();
  m_date->setPosition(dateX, cy - dateBlock * 0.5f);
  m_weekday->setPosition(dateX, cy - dateBlock * 0.5f + m_date->height() + 5.0f * s);

  const float right = ox + sw - 80.0f * s;
  float hudX = right;
  for (Label* action : {m_shutdown, m_reboot, m_session}) {
    action->setFontSize(12.0f * s);
    action->measure(renderer);
    hudX -= action->width();
    action->setPosition(hudX, oy + 50.0f * s);
    hudX -= 25.0f * s;
  }

  const float columnBottom = oy + sh - 80.0f * s;
  m_mask->setFontSize(14.0f * s);
  std::string stars;
  for (std::size_t i = 0; i < m_passwordLength; ++i) {
    stars += "✦";
  }
  m_mask->setText(tracked(stars, kThin));
  m_mask->measure(renderer);
  m_waiting->setFontSize(10.0f * s);
  m_waiting->measure(renderer);
  m_passwordRow = Rect{right - 350.0f * s, columnBottom - 70.0f * s, 350.0f * s, 30.0f * s};
  const float rowMid = m_passwordRow.y + m_passwordRow.h * 0.5f;
  m_mask->setPosition(right - m_mask->width(), rowMid - m_mask->height() * 0.5f);
  m_mask->setVisible(m_passwordLength > 0);
  m_waiting->setPosition(right - m_waiting->width(), rowMid - m_waiting->height() * 0.5f);
  m_waiting->setOpacity(0.4f);
  m_waiting->setVisible(m_passwordLength == 0);
  m_needle->setSize(std::max(1.5f, 1.5f * s), 12.0f * s);
  m_needle->setPosition(right + 4.0f * s, rowMid - 6.0f * s);
  m_needle->setVisible(m_passwordLength > 0);

  m_hint->setFontSize(10.0f * s);
  m_hint->measure(renderer);
  m_hint->setPosition(right - m_hint->width(), m_passwordRow.y - 8.0f * s - m_hint->height());

  m_user->setFontSize(18.0f * s);
  m_user->measure(renderer);
  m_user->setPosition(right - m_user->width(), m_hint->y() - 10.0f * s - m_user->height());
}

void RyokuScene::layoutRing(Renderer& renderer, Ring& ring, float cx, float cy, float numberInset) {
  const float r = ring.radius;
  ring.container->setPosition(cx - r, cy - r);
  ring.container->setSize(2.0f * r, 2.0f * r);
  for (std::size_t i = 0; i < ring.ticks.size(); ++i) {
    const float a = static_cast<float>(i) * 6.0f * kPi / 180.0f;
    RectNode* tick = ring.ticks[i];
    const bool major = i % 5 == 0;
    const float h = (major ? ring.tickLong : ring.tickShort) * m_s;
    const float w = (major ? 2.0f : 1.0f) * std::max(1.0f, m_s);
    tick->setSize(w, h);
    tick->setPosition(r + r * std::cos(a) - w * 0.5f, r + r * std::sin(a) - h * 0.5f);
    tick->setRotation(a + kPi * 0.5f);
  }
  for (std::size_t i = 0; i < ring.numbers.size(); ++i) {
    Label* number = ring.numbers[i];
    const float a = static_cast<float>(i) * 30.0f * kPi / 180.0f;
    number->setFontSize(ring.numberSize * m_s);
    number->measure(renderer);
    const float nr = r - numberInset;
    number->setPosition(r + nr * std::cos(a) - number->width() * 0.5f, r + nr * std::sin(a) - number->height() * 0.5f);
    number->setRotation(a);
  }
}

bool RyokuScene::tick() {
  const float ms = msOfDay();
  const float secAngle = -std::fmod(ms, 60000.0f) / 60000.0f * 2.0f * kPi;
  const float minAngle = -std::fmod(ms, 3600000.0f) / 3600000.0f * 2.0f * kPi;
  m_seconds.container->setRotation(secAngle);
  m_minutes.container->setRotation(minAngle);
  spotlight(m_seconds, secAngle);
  spotlight(m_minutes, minAngle);

  const std::time_t t = std::time(nullptr);
  std::tm local{};
  localtime_r(&t, &local);
  char hour[3];
  std::strftime(hour, sizeof(hour), "%H", &local);
  char date[32];
  std::strftime(date, sizeof(date), "%d %b %Y", &local);
  char weekday[16];
  std::strftime(weekday, sizeof(weekday), "%A", &local);
  const std::string dateText = std::string(date) + weekday;
  if (hour == m_hourText && dateText == m_dateText) {
    return false;
  }
  m_hourText = hour;
  m_dateText = dateText;
  m_date->setText(tracked(upper(date), kThin));
  m_weekday->setText(tracked(upper(weekday), kThin));
  return true;
}

// The tick facing the pill (display angle 0) is lit; neighbours fade over 4 degrees, as in Ryoku.
void RyokuScene::spotlight(Ring& ring, float ringAngle) {
  for (std::size_t i = 0; i < ring.ticks.size(); ++i) {
    const float rel = std::remainder(static_cast<float>(i) * 6.0f + ringAngle * 180.0f / kPi, 360.0f);
    const float light = std::max(0.0f, 1.0f - std::abs(rel) / 4.0f);
    const bool major = i % 5 == 0;
    ring.ticks[i]->setOpacity(light > 0.0f ? 1.0f : (major ? 0.3f : 0.15f));
    if (major) {
      ring.numbers[i / 5]->setOpacity(light > 0.0f ? 0.4f + light * 0.6f : 0.25f);
    }
  }
}

void RyokuScene::setUserName(const std::string& name) { m_user->setText(tracked(upper(name), kThin)); }

void RyokuScene::setHint(const std::string& text, bool isError) {
  m_hint->setText(tracked(upper(text), kHair));
  m_hint->setColor(isError ? kError : kDim);
}

void RyokuScene::setPasswordLength(std::size_t length) { m_passwordLength = length; }

void RyokuScene::setSessionName(const std::string& name) { m_session->setText(tracked(upper(name), kHair)); }

void RyokuScene::setOnSession(std::function<void()> callback) {
  m_session->setOnClick([cb = std::move(callback)](const auto&) { cb(); });
}

void RyokuScene::setOnReboot(std::function<void()> callback) {
  m_reboot->setOnClick([cb = std::move(callback)](const auto&) { cb(); });
}

void RyokuScene::setOnShutdown(std::function<void()> callback) {
  m_shutdown->setOnClick([cb = std::move(callback)](const auto&) { cb(); });
}

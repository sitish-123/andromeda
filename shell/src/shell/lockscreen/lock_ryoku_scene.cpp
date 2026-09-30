#include "shell/lockscreen/lock_ryoku_scene.h"

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <numbers>

namespace {
  constexpr char kFont[] = "Outfit Black";
  constexpr float kPi = std::numbers::pi_v<float>;

  constexpr Color kWhite = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  constexpr Color kDim = rgba(0.40F, 0.40F, 0.40F, 1.0F);
  constexpr Color kSub = rgba(0.333F, 0.333F, 0.333F, 1.0F);
  constexpr Color kWait = rgba(0.20F, 0.20F, 0.20F, 1.0F);
  constexpr Color kError = rgba(1.0F, 0.267F, 0.267F, 1.0F);

  constexpr std::string_view kThin = " ";
  constexpr std::string_view kHair = " ";
  constexpr std::string_view kStar = "✦";

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

  Color mix(const Color& a, const Color& b, float t) {
    return rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
  }

  Node* addGroup(Node& parent, int z) {
    auto group = std::make_unique<Node>();
    group->setZIndex(z);
    return parent.addChild(std::move(group));
  }

  Label* addLabel(Node& parent, float size, const Color& color, int z) {
    auto label = std::make_unique<Label>();
    label->setFontFamily(kFont);
    label->setFontSize(size);
    label->setColor(color);
    label->setZIndex(z);
    return static_cast<Label*>(parent.addChild(std::move(label)));
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

  float steadySeconds() {
    return std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
} // namespace

void LockRyokuScene::build(Node& root, AnimationManager& animations) {
  m_animations = &animations;
  m_background = addRect(root, RoundedRectStyle{.fill = rgba(0.0F, 0.0F, 0.0F, 1.0F)}, 0);

  m_blast = addGroup(root, 1);
  buildRing(m_minutes, 18.0F, 10.0F, 22.0F);
  buildRing(m_seconds, 13.0F, 8.0F, 16.0F);
  // Below the rings, so the lit minute and second show inside the pill.
  m_pill = addRect(*m_blast, {}, 1);
  m_pillLine = addRect(*m_blast, {}, 1);
  m_hour = addLabel(*m_blast, 110.0F, kWhite, 5);
  m_date = addLabel(*m_blast, 13.0F, kSub, 5);
  m_weekday = addLabel(*m_blast, 18.0F, kWhite, 5);

  m_hud = addGroup(root, 8);
  m_column = addGroup(*m_hud, 1);
  m_user = addLabel(*m_column, 18.0F, kDim, 1);
  m_hint = addLabel(*m_column, 10.0F, kDim, 1);
  m_mask = addLabel(*m_column, 14.0F, kDim, 1);
  m_waiting = addLabel(*m_column, 10.0F, kWait, 1);
  m_waiting->setText(tracked("WAITING FOR KEY", kHair));
  m_waiting->setOpacity(m_waitingOpacity);
  m_needle = addRect(*m_column, RoundedRectStyle{.fill = kWhite}, 1);

  m_flash = addRect(root, RoundedRectStyle{.fill = kWhite}, 50);
  m_flash->setOpacity(0.0F);

  // Entry: everything fades up while the dial winds back onto the time.
  m_blast->setOpacity(0.0F);
  m_hud->setOpacity(0.0F);
  m_animations->animate(0.0F, 1.0F, 350.0F, Easing::EaseOutCubic, [this](float v) {
    m_blast->setOpacity(v);
    m_hud->setOpacity(v);
  });
  m_animations->animate(0.9F, 0.0F, 1100.0F, Easing::EaseOutCubic, [this](float v) { m_introOffset = v; });
}

void LockRyokuScene::buildRing(Ring& ring, float tickLong, float tickShort, float numberSize) {
  ring.tickLong = tickLong;
  ring.tickShort = tickShort;
  ring.numberSize = numberSize;
  ring.container = addGroup(*m_blast, 2);
  ring.container->setHitTestVisible(false);
  for (RectNode*& tick : ring.ticks) {
    tick = addRect(*ring.container, RoundedRectStyle{.fill = kWhite}, 0);
  }
  for (std::size_t i = 0; i < ring.numbers.size(); ++i) {
    const std::size_t minute = i * 5;
    ring.numbers[i] = addLabel(*ring.container, numberSize, kWhite, 0);
    ring.numbers[i]->setText((minute < 10 ? "0" : "") + std::to_string(minute));
  }
}

void LockRyokuScene::layout(Renderer& renderer, float sw, float sh) {
  m_s = sh / 768.0F;
  const float s = m_s;

  m_background->setPosition(0.0F, 0.0F);
  m_background->setSize(sw, sh);
  m_flash->setPosition(0.0F, 0.0F);
  m_flash->setSize(sw, sh);

  // The blast group is centred where Ryoku scales it from: 400 px right of the edge, halfway down.
  m_blast->setPosition(0.0F, 0.0F);
  m_blast->setSize(800.0F * s, sh);
  const float cx = 40.0F * s;
  const float cy = sh * 0.5F;
  m_ringCx = cx;
  m_ringCy = cy;
  m_viewH = sh;
  m_minutes.radius = 320.0F * s;
  m_seconds.radius = 480.0F * s;
  layoutRing(renderer, m_minutes, cx, cy, 35.0F * s);
  layoutRing(renderer, m_seconds, cx, cy, 30.0F * s);

  const float pillX = cx + 230.0F * s;
  const float pillW = 330.0F * s;
  const float pillH = 90.0F * s;
  m_pill->setStyle(
      RoundedRectStyle{
          .fill = rgba(0.031F, 0.031F, 0.031F, 1.0F),
          .border = rgba(0.102F, 0.102F, 0.102F, 1.0F),
          .radius = pillH * 0.5F,
          .borderWidth = std::max(1.0F, s),
      }
  );
  m_pill->setPosition(pillX, cy - pillH * 0.5F);
  m_pill->setSize(pillW, pillH);
  m_pillLine->setStyle(RoundedRectStyle{.fill = rgba(0.133F, 0.133F, 0.133F, 1.0F)});
  m_pillLine->setPosition(pillX + 170.0F * s, cy - 17.5F * s);
  m_pillLine->setSize(std::max(1.0F, s), 35.0F * s);

  (void)tick(true);
  m_hour->setFontSize(110.0F * s);
  m_hour->setText(m_hourText);
  m_hour->measure(renderer);
  m_hour->setPosition(pillX - 40.0F * s - m_hour->width(), cy - m_hour->height() * 0.5F);
  m_date->setFontSize(13.0F * s);
  m_weekday->setFontSize(18.0F * s);
  m_date->measure(renderer);
  m_weekday->measure(renderer);
  const float dateX = pillX + pillW + 110.0F * s;
  const float dateBlock = m_date->height() + 5.0F * s + m_weekday->height();
  m_date->setPosition(dateX, cy - dateBlock * 0.5F);
  m_weekday->setPosition(dateX, cy - dateBlock * 0.5F + m_date->height() + 5.0F * s);

  m_hud->setSize(sw, sh);
  const float right = sw - 80.0F * s;
  float hudX = right;
  for (auto it = m_actionLabels.rbegin(); it != m_actionLabels.rend(); ++it) {
    (*it)->setFontSize(12.0F * s);
    (*it)->measure(renderer);
    hudX -= (*it)->width();
    (*it)->setPosition(hudX, 50.0F * s);
    hudX -= 25.0F * s;
  }

  m_column->setSize(sw, sh);
  const float columnBottom = sh - 80.0F * s;
  std::string stars;
  for (std::size_t i = 0; i < m_passwordLength; ++i) {
    stars += kStar;
  }
  m_mask->setFontSize(14.0F * s);
  m_mask->setText(tracked(stars, kThin));
  m_mask->measure(renderer);
  m_waiting->setFontSize(10.0F * s);
  m_waiting->measure(renderer);
  m_passwordRow = Rect{right - 350.0F * s, columnBottom - 70.0F * s, 350.0F * s, 30.0F * s};
  const float rowMid = m_passwordRow.y + m_passwordRow.h * 0.5F;
  m_mask->setPosition(right - m_mask->width(), rowMid - m_mask->height() * 0.5F);
  m_mask->setVisible(m_passwordLength > 0);
  m_waiting->setPosition(right - m_waiting->width(), rowMid - m_waiting->height() * 0.5F);
  m_needle->setSize(std::max(1.5F, 1.5F * s), 12.0F * s);
  m_needle->setPosition(right + 4.0F * s, rowMid - 6.0F * s);
  m_needle->setVisible(m_passwordLength > 0);

  m_hint->setFontSize(10.0F * s);
  m_hint->measure(renderer);
  m_hint->setPosition(right - m_hint->width(), m_passwordRow.y - 8.0F * s - m_hint->height());
  m_user->setFontSize(18.0F * s);
  m_user->measure(renderer);
  m_user->setPosition(right - m_user->width(), m_hint->y() - 10.0F * s - m_user->height());
}

void LockRyokuScene::layoutRing(Renderer& renderer, Ring& ring, float cx, float cy, float numberInset) {
  const float r = ring.radius;
  ring.container->setPosition(cx - r, cy - r);
  ring.container->setSize(2.0F * r, 2.0F * r);
  for (std::size_t i = 0; i < ring.ticks.size(); ++i) {
    const float a = static_cast<float>(i) * 6.0F * kPi / 180.0F;
    const bool major = i % 5 == 0;
    const float h = (major ? ring.tickLong : ring.tickShort) * m_s;
    const float w = (major ? 2.0F : 1.0F) * std::max(1.0F, m_s);
    RectNode* tick = ring.ticks[i];
    tick->setSize(w, h);
    tick->setPosition(r + r * std::cos(a) - w * 0.5F, r + r * std::sin(a) - h * 0.5F);
    tick->setRotation(a + kPi * 0.5F);
  }
  for (std::size_t i = 0; i < ring.numbers.size(); ++i) {
    Label* number = ring.numbers[i];
    const float a = static_cast<float>(i) * 30.0F * kPi / 180.0F;
    number->setFontSize(ring.numberSize * m_s);
    number->measure(renderer);
    const float nr = r - numberInset;
    number->setPosition(r + nr * std::cos(a) - number->width() * 0.5F, r + nr * std::sin(a) - number->height() * 0.5F);
    number->setRotation(a);
  }
}

bool LockRyokuScene::tick(bool smooth) {
  const float ms = smooth ? msOfDay() : std::floor(msOfDay() / 1000.0F) * 1000.0F;
  const float secAngle = -std::fmod(ms, 60000.0F) / 60000.0F * 2.0F * kPi + m_introOffset * 1.6F;
  const float minAngle = -std::fmod(ms, 3600000.0F) / 3600000.0F * 2.0F * kPi + m_introOffset;
  m_seconds.container->setRotation(secAngle);
  m_minutes.container->setRotation(minAngle);
  spotlight(m_seconds, secAngle);
  spotlight(m_minutes, minAngle);

  // The looping effects need every frame; the idle clock draws once a second and holds them still.
  const float t = smooth ? steadySeconds() : 0.0F;
  // Ryoku's sensor pulse: 1.0 to 0.45 and back over 1.2 s while the face check listens.
  m_hint->setOpacity(m_scanning ? 0.725F + 0.275F * std::cos(t * 2.0F * kPi / 1.2F) : 1.0F);
  m_needle->setOpacity(0.55F + 0.45F * std::cos(t * 2.0F * kPi / 0.9F));
  m_column->setPosition(m_shake * 10.0F * m_s * std::sin(t * 60.0F), 0.0F);

  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
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
void LockRyokuScene::spotlight(Ring& ring, float ringAngle) {
  // The dial is centred off the left edge, so about half of it is off screen; hidden nodes are not drawn.
  const float margin = 30.0F * m_s;
  const auto onScreen = [&](float angle, float radius) {
    const float x = m_ringCx + radius * std::cos(angle);
    const float y = m_ringCy + radius * std::sin(angle);
    return x > -margin && y > -margin && y < m_viewH + margin;
  };
  for (std::size_t i = 0; i < ring.ticks.size(); ++i) {
    const float angle = static_cast<float>(i) * 6.0F * kPi / 180.0F + ringAngle;
    const float rel = std::remainder(angle * 180.0F / kPi, 360.0F);
    const float light = std::max(0.0F, 1.0F - std::abs(rel) / 4.0F);
    const bool major = i % 5 == 0;
    ring.ticks[i]->setVisible(onScreen(angle, ring.radius));
    ring.ticks[i]->setOpacity(light > 0.0F ? 1.0F : (major ? 0.3F : 0.15F));
    if (major) {
      Label* number = ring.numbers[i / 5];
      number->setVisible(onScreen(angle, ring.radius - 35.0F * m_s));
      number->setOpacity(light > 0.0F ? 0.4F + light * 0.6F : 0.25F);
    }
  }
}

void LockRyokuScene::fadeColor(Label* label, bool lit) {
  m_animations->cancelForOwner(label);
  const Color from = label->color();
  const Color to = lit ? kWhite : kDim;
  m_animations->animate(
      0.0F, 1.0F, 200.0F, Easing::EaseOutCubic, [label, from, to](float v) { label->setColor(mix(from, to, v)); }, {},
      label
  );
}

void LockRyokuScene::setUser(const std::string& name) { m_user->setText(tracked(upper(name), kThin)); }

void LockRyokuScene::setHint(const std::string& text, bool isError, bool scanning) {
  m_scanning = scanning && !isError;
  m_hint->setText(tracked(upper(text), kHair));
  m_hint->setColor(isError ? kError : (m_scanning ? kWhite : kDim));
}

void LockRyokuScene::setPasswordLength(std::size_t length) {
  const bool wasEmpty = m_passwordLength == 0;
  m_passwordLength = length;
  if (wasEmpty == (length == 0)) {
    return;
  }
  m_animations->cancel(m_waitingAnim);
  m_waitingAnim = m_animations->animate(
      m_waitingOpacity, length == 0 ? 0.4F : 0.0F, 400.0F, Easing::EaseInOutCubic, [this](float v) {
        m_waitingOpacity = v;
        m_waiting->setOpacity(v);
      }
  );
}

void LockRyokuScene::setActions(std::vector<Action> actions) {
  std::vector<std::string> names;
  names.reserve(actions.size());
  for (const Action& action : actions) {
    names.push_back(action.label);
  }
  if (names == m_actionNames) {
    return;
  }
  m_actionNames = std::move(names);
  for (Label* label : m_actionLabels) {
    m_animations->cancelForOwner(label);
    (void)m_hud->removeChild(label);
  }
  m_actionLabels.clear();
  for (Action& action : actions) {
    Label* label = addLabel(*m_hud, 12.0F, kDim, 1);
    label->setText(tracked(upper(action.label), kHair));
    label->setHitTestVisible(true);
    label->setOnEnter([this, label](const auto&) { fadeColor(label, true); });
    label->setOnLeave([this, label]() { fadeColor(label, false); });
    label->setOnClick([run = std::move(action.run)](const auto&) { run(); });
    m_actionLabels.push_back(label);
  }
}

void LockRyokuScene::playRejected() {
  m_animations->cancelForOwner(&m_shake);
  m_animations->animate(1.0F, 0.0F, 420.0F, Easing::Linear, [this](float v) { m_shake = v; }, {}, &m_shake);
}

void LockRyokuScene::playGranted(std::function<void()> onDone) {
  m_scanning = false;
  m_hint->setText(tracked("ACCESS GRANTED " + std::string(kStar), kHair));
  m_hint->setColor(kWhite);
  m_animations->animateTimer(1.0F, 35.0F, 190.0F, Easing::EaseInQuad, [this](float v) { m_blast->setScale(v); });
  m_animations->animateTimer(1.0F, 0.0F, 120.0F, Easing::EaseInQuad, [this](float v) { m_hud->setOpacity(v); });
  m_animations->animateTimer(
      0.0F, 1.0F, 130.0F, Easing::EaseInQuad, [this](float v) { m_flash->setOpacity(v); },
      [done = std::move(onDone)]() {
        if (done) {
          done();
        }
      }
  );
}

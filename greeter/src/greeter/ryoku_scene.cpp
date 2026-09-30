#include "greeter/ryoku_scene.h"

#include "render/animation/animation_manager.h"
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

  constexpr Color kWhite = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  constexpr Color kDim = rgba(0.40f, 0.40f, 0.40f, 1.0f);
  constexpr Color kInactive = rgba(0.267f, 0.267f, 0.267f, 1.0f);
  constexpr Color kSub = rgba(0.333f, 0.333f, 0.333f, 1.0f);
  constexpr Color kWait = rgba(0.20f, 0.20f, 0.20f, 1.0f);
  constexpr Color kError = rgba(1.0f, 0.267f, 0.267f, 1.0f);

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

void RyokuScene::build(Node& root, AnimationManager& animations) {
  m_root = &root;
  m_animations = &animations;
  m_background = addRect(root, RoundedRectStyle{.fill = rgba(0.0f, 0.0f, 0.0f, 1.0f)}, 0);

  m_blast = addGroup(root, 1);
  buildRing(m_minutes, 18.0f, 10.0f, 22.0f);
  buildRing(m_seconds, 13.0f, 8.0f, 16.0f);
  // Below the rings, so the lit minute and second show inside the pill.
  m_pill = addRect(*m_blast, {}, 1);
  m_pillLine = addRect(*m_blast, {}, 1);
  m_hour = addLabel(*m_blast, 110.0f, kWhite, 5);
  m_date = addLabel(*m_blast, 13.0f, kSub, 5);
  m_weekday = addLabel(*m_blast, 18.0f, kWhite, 5);

  m_hud = addGroup(root, 8);
  m_session = addLabel(*m_hud, 12.0f, kDim, 1);
  m_reboot = addLabel(*m_hud, 12.0f, kDim, 1);
  m_reboot->setText(tracked("REBOOT", kHair));
  m_shutdown = addLabel(*m_hud, 12.0f, kDim, 1);
  m_shutdown->setText(tracked("SHUTDOWN", kHair));
  for (Label* action : {m_session, m_reboot, m_shutdown}) {
    action->setHitTestVisible(true);
    action->setOnEnter([this, action](const auto&) { fadeColor(action, true); });
    action->setOnLeave([this, action]() { fadeColor(action, false); });
  }

  m_column = addGroup(*m_hud, 1);
  m_userMenu = addGroup(*m_column, 2);
  m_user = addLabel(*m_column, 18.0f, kDim, 1);
  m_user->setOnEnter([this](const auto&) { fadeColor(m_user, true); });
  m_user->setOnLeave([this]() {
    if (!m_userMenuOpen) {
      fadeColor(m_user, false);
    }
  });
  m_user->setOnClick([this](const auto&) { toggleUserMenu(); });
  m_userMark = addLabel(*m_column, 12.0f, kWhite, 1);
  m_userMark->setText(std::string(kStar));
  m_userMark->setOpacity(0.0f);
  m_hint = addLabel(*m_column, 10.0f, kDim, 1);
  m_mask = addLabel(*m_column, 14.0f, kDim, 1);
  m_waiting = addLabel(*m_column, 10.0f, kWait, 1);
  m_waiting->setText(tracked("WAITING FOR KEY", kHair));
  m_waiting->setOpacity(m_waitingOpacity);
  m_needle = addRect(*m_column, RoundedRectStyle{.fill = kWhite}, 1);

  m_flash = addRect(root, RoundedRectStyle{.fill = kWhite}, 50);
  m_flash->setOpacity(0.0f);

  // Entry: everything fades up while the dial winds back onto the time.
  m_blast->setOpacity(0.0f);
  m_hud->setOpacity(0.0f);
  m_animations->animate(0.0f, 1.0f, 350.0f, Easing::EaseOutCubic, [this](float v) {
    m_blast->setOpacity(v);
    m_hud->setOpacity(v);
  });
  m_animations->animate(0.9f, 0.0f, 1100.0f, Easing::EaseOutCubic, [this](float v) { m_introOffset = v; });
}

void RyokuScene::buildRing(Ring& ring, float tickLong, float tickShort, float numberSize) {
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

void RyokuScene::layout(Renderer& renderer, float ox, float oy, float sw, float sh) {
  m_s = sh / 768.0f;
  const float s = m_s;

  m_background->setPosition(ox, oy);
  m_background->setSize(sw, sh);
  m_flash->setPosition(ox, oy);
  m_flash->setSize(sw, sh);

  // The blast group is centred where Ryoku scales it from: 400 px right of the edge, halfway down.
  m_blast->setPosition(ox, oy);
  m_blast->setSize(800.0f * s, sh);
  const float cx = 40.0f * s;
  const float cy = sh * 0.5f;
  m_ringCx = cx;
  m_ringCy = cy;
  m_viewH = sh;
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

  (void)tick(true);
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

  m_hud->setPosition(ox, oy);
  m_hud->setSize(sw, sh);
  const float right = sw - 80.0f * s;
  float hudX = right;
  for (Label* action : {m_shutdown, m_reboot, m_session}) {
    action->setFontSize(12.0f * s);
    action->measure(renderer);
    hudX -= action->width();
    action->setPosition(hudX, 50.0f * s);
    hudX -= 25.0f * s;
  }

  m_column->setSize(sw, sh);
  const float columnBottom = sh - 80.0f * s;
  std::string stars;
  for (std::size_t i = 0; i < m_passwordLength; ++i) {
    stars += kStar;
  }
  m_mask->setFontSize(14.0f * s);
  m_mask->setText(tracked(stars, kThin));
  m_mask->measure(renderer);
  m_waiting->setFontSize(10.0f * s);
  m_waiting->measure(renderer);
  const Rect row{right - 350.0f * s, columnBottom - 70.0f * s, 350.0f * s, 30.0f * s};
  m_passwordRow = Rect{row.x + ox, row.y + oy, row.w, row.h};
  const float rowMid = row.y + row.h * 0.5f;
  m_mask->setPosition(right - m_mask->width(), rowMid - m_mask->height() * 0.5f);
  m_mask->setVisible(m_passwordLength > 0);
  m_waiting->setPosition(right - m_waiting->width(), rowMid - m_waiting->height() * 0.5f);
  m_needle->setSize(std::max(1.5f, 1.5f * s), 12.0f * s);
  m_needle->setPosition(right + 4.0f * s, rowMid - 6.0f * s);
  m_needle->setVisible(m_passwordLength > 0);

  m_hint->setFontSize(10.0f * s);
  m_hint->measure(renderer);
  m_hint->setPosition(right - m_hint->width(), row.y - 8.0f * s - m_hint->height());

  m_user->setFontSize(18.0f * s);
  m_user->measure(renderer);
  m_user->setPosition(right - m_user->width(), m_hint->y() - 10.0f * s - m_user->height());
  m_userMark->setFontSize(12.0f * s);
  m_userMark->measure(renderer);
  m_userMark->setPosition(right + 8.0f * s, m_user->y() + (m_user->height() - m_userMark->height()) * 0.5f);
  layoutUserMenu(renderer, right, m_user->y() - 15.0f * s);
}

void RyokuScene::layoutRing(Renderer& renderer, Ring& ring, float cx, float cy, float numberInset) {
  const float r = ring.radius;
  ring.container->setPosition(cx - r, cy - r);
  ring.container->setSize(2.0f * r, 2.0f * r);
  for (std::size_t i = 0; i < ring.ticks.size(); ++i) {
    const float a = static_cast<float>(i) * 6.0f * kPi / 180.0f;
    const bool major = i % 5 == 0;
    const float h = (major ? ring.tickLong : ring.tickShort) * m_s;
    const float w = (major ? 2.0f : 1.0f) * std::max(1.0f, m_s);
    RectNode* tick = ring.ticks[i];
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

void RyokuScene::layoutUserMenu(Renderer& renderer, float right, float bottom) {
  const bool several = m_users.size() > 1;
  m_user->setHitTestVisible(several);
  float y = bottom;
  for (std::size_t i = m_userItems.size(); i-- > 0;) {
    Label* item = m_userItems[i];
    item->setFontSize(13.0f * m_s);
    item->measure(renderer);
    y -= item->height() + 6.0f * m_s;
    item->setPosition(right - 10.0f * m_s - item->width(), y);
  }
  m_userMenu->setVisible(several && m_userMenuReveal > 0.0f);
}

bool RyokuScene::tick(bool smooth) {
  const float ms = smooth ? msOfDay() : std::floor(msOfDay() / 1000.0f) * 1000.0f;
  const float secAngle = -std::fmod(ms, 60000.0f) / 60000.0f * 2.0f * kPi + m_introOffset * 1.6f;
  const float minAngle = -std::fmod(ms, 3600000.0f) / 3600000.0f * 2.0f * kPi + m_introOffset;
  m_seconds.container->setRotation(secAngle);
  m_minutes.container->setRotation(minAngle);
  spotlight(m_seconds, secAngle);
  spotlight(m_minutes, minAngle);

  // The looping effects need every frame; the idle clock draws once a second and holds them still.
  const float t = smooth ? steadySeconds() : 0.0f;
  // Ryoku's sensor pulse: 1.0 to 0.45 and back over 1.2 s while the face check listens.
  m_hint->setOpacity(m_scanning ? 0.725f + 0.275f * std::cos(t * 2.0f * kPi / 1.2f) : 1.0f);
  m_needle->setOpacity(0.55f + 0.45f * std::cos(t * 2.0f * kPi / 0.9f));
  m_column->setPosition(m_shake * 10.0f * m_s * std::sin(t * 60.0f), 0.0f);
  m_userMenu->setOpacity(m_userMenuReveal);

  m_userMenu->setPosition(0.0f, (1.0f - m_userMenuReveal) * 12.0f * m_s);

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
void RyokuScene::spotlight(Ring& ring, float ringAngle) {
  // The dial is centred off the left edge, so about half of it is off screen; hidden nodes are not drawn.
  const float margin = 30.0f * m_s;
  const auto onScreen = [&](float angle, float radius) {
    const float x = m_ringCx + radius * std::cos(angle);
    const float y = m_ringCy + radius * std::sin(angle);
    return x > -margin && y > -margin && y < m_viewH + margin;
  };
  for (std::size_t i = 0; i < ring.ticks.size(); ++i) {
    const float angle = static_cast<float>(i) * 6.0f * kPi / 180.0f + ringAngle;
    const float rel = std::remainder(angle * 180.0f / kPi, 360.0f);
    const float light = std::max(0.0f, 1.0f - std::abs(rel) / 4.0f);
    const bool major = i % 5 == 0;
    ring.ticks[i]->setVisible(onScreen(angle, ring.radius));
    ring.ticks[i]->setOpacity(light > 0.0f ? 1.0f : (major ? 0.3f : 0.15f));
    if (major) {
      Label* number = ring.numbers[i / 5];
      number->setVisible(onScreen(angle, ring.radius - 35.0f * m_s));
      number->setOpacity(light > 0.0f ? 0.4f + light * 0.6f : 0.25f);
    }
  }
}

void RyokuScene::fadeColor(Label* label, bool lit) {
  m_animations->cancelForOwner(label);
  const Color from = label->color();
  const Color to = lit ? kWhite : kDim;
  m_animations->animate(
      0.0f, 1.0f, 200.0f, Easing::EaseOutCubic, [label, from, to](float v) { label->setColor(mix(from, to, v)); },
      nullptr, label
  );
  if (label == m_user) {
    m_userMark->setOpacity(lit ? 1.0f : 0.0f);
  }
}

void RyokuScene::toggleUserMenu() {
  if (m_users.size() < 2) {
    return;
  }
  m_userMenuOpen = !m_userMenuOpen;
  m_userMenu->setVisible(true);
  m_animations->cancelForOwner(&m_userMenuReveal);
  m_animations->animate(
      m_userMenuReveal, m_userMenuOpen ? 1.0f : 0.0f, 400.0f, Easing::EaseOutCubic,
      [this](float v) { m_userMenuReveal = v; }, [this]() { m_userMenu->setVisible(m_userMenuReveal > 0.0f); },
      &m_userMenuReveal
  );
  fadeColor(m_user, m_userMenuOpen);
}

void RyokuScene::setUsers(const std::vector<std::string>& users, std::size_t selected) {
  m_selectedUser = selected;
  if (selected < users.size()) {
    m_user->setText(tracked(upper(users[selected]), kThin));
  }
  if (users != m_users) {
    m_users = users;
    for (Label* item : m_userItems) {
      m_animations->cancelForOwner(item);
      (void)m_userMenu->removeChild(item);
    }
    m_userItems.clear();
    for (std::size_t i = 0; i < m_users.size(); ++i) {
      Label* item = addLabel(*m_userMenu, 13.0f, kInactive, 0);
      item->setText(tracked(upper(m_users[i]), kHair));
      item->setHitTestVisible(true);
      item->setOnEnter([this, item](const auto&) { fadeColor(item, true); });
      item->setOnLeave([this, item, i]() {
        m_animations->cancelForOwner(item);
        item->setColor(i == m_selectedUser ? kWhite : kInactive);
      });
      item->setOnClick([this, i](const auto&) {
        toggleUserMenu();
        if (m_onUserPicked && i != m_selectedUser) {
          m_onUserPicked(i);
        }
      });
      m_userItems.push_back(item);
    }
  }
  for (std::size_t i = 0; i < m_userItems.size(); ++i) {
    m_userItems[i]->setColor(i == m_selectedUser ? kWhite : kInactive);
  }
}

void RyokuScene::setHint(const std::string& text, bool isError) {
  m_hint->setText(tracked(upper(text), kHair));
  m_hint->setColor(isError ? kError : (m_scanning ? kWhite : kDim));
}

void RyokuScene::setScanning(bool scanning) { m_scanning = scanning; }

void RyokuScene::setPasswordLength(std::size_t length) {
  const bool wasEmpty = m_passwordLength == 0;
  m_passwordLength = length;
  if (wasEmpty == (length == 0)) {
    return;
  }
  m_animations->cancel(m_waitingAnim);
  m_waitingAnim = m_animations->animate(
      m_waitingOpacity, length == 0 ? 0.4f : 0.0f, 400.0f, Easing::EaseInOutCubic, [this](float v) {
        m_waitingOpacity = v;
        m_waiting->setOpacity(v);
      }
  );
}

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

void RyokuScene::setOnUserPicked(std::function<void(std::size_t)> callback) { m_onUserPicked = std::move(callback); }

void RyokuScene::playRejected() {
  m_animations->cancelForOwner(&m_shake);
  m_animations->animate(1.0f, 0.0f, 420.0f, Easing::Linear, [this](float v) { m_shake = v; }, nullptr, &m_shake);
}

void RyokuScene::playGranted(std::function<void()> onDone) {
  m_scanning = false;
  m_hint->setText(tracked("ACCESS GRANTED " + std::string(kStar), kHair));
  m_hint->setColor(kWhite);
  m_animations->animate(1.0f, 35.0f, 190.0f, Easing::EaseInCubic, [this](float v) { m_blast->setScale(v); });
  m_animations->animate(1.0f, 0.0f, 120.0f, Easing::EaseInCubic, [this](float v) { m_hud->setOpacity(v); });
  m_animations->animate(
      0.0f, 1.0f, 130.0f, Easing::EaseInCubic, [this](float v) { m_flash->setOpacity(v); },
      [done = std::move(onDone)]() {
        if (done) {
          done();
        }
      }
  );
}

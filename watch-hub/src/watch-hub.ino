/* Family Hub on the M5Stack StopWatch (SKU C152).
 * Hardware setup, touch, audio and the round panel live in board.cpp.
 * Dice frames use dice::Renderer::renderFast — never the SDF renderer.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <stdarg.h>
#include <M5Unified.h>
extern "C" {
#include <cJSON.h>
}
#include "board.h"
#include "dice_sdf.h"
#include "roll_motion.h"
#include "zh_font.h"

#if __has_include("private-config.h")
#include "private-config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef WIFI_SSID2
#define WIFI_SSID2 ""
#endif
#ifndef WIFI_PASS2
#define WIFI_PASS2 ""
#endif
#ifndef DEFAULT_SERVER
#define DEFAULT_SERVER ""
#endif
#ifndef INBOX_SECRET
#define INBOX_SECRET ""
#endif

#ifndef DEVICE_ROLE
#define DEVICE_ROLE "mom"
#endif
#ifndef SEND_CONTACT
#define SEND_CONTACT "mom"
#endif
#ifndef LISTEN_CONTACT
#define LISTEN_CONTACT "dad"
#endif

static constexpr int SCR_W = board::kWidth;
static constexpr int SCR_H = board::kHeight;
static constexpr uint32_t kIdleSleepMs = 60000;
static constexpr uint32_t kPollMs = 1200;

#define ROLL_MS 960
#define SAMPLE_RATE 16000
#define MAX_REC_MS 15000
#define MIN_REC_MS 400
#define MAX_MESSAGES 3

static const uint16_t C_BG = 0xFF9B, C_CARD = 0xFFFF, C_LINE = 0xD651;
static const uint16_t C_WHITE = 0x2126, C_MUTED = 0x632C;
static const uint16_t C_TEAL = 0x76B5, C_GOLD = 0xFE0C;
static const uint16_t C_RED = 0xFC51;

static const int HOME_DICE_Y = 148, HOME_DICE_H = 104;
static const int HOME_VOICE_Y = 264, HOME_VOICE_H = 104;
static const int HOME_SET_Y = 388, HOME_SET_H = 42;
static const int SL_X = 78, SL_W = 310;

struct Contact { const char *key; const char *name; uint16_t color; };
static const Contact CONTACTS[] = {
  {"dad", "Dad", C_GOLD}, {"mom", "Mom", C_RED}, {"me", "Me", C_TEAL}
};
struct Message { char id[48]; uint32_t ts; uint16_t seconds; bool incoming; uint8_t sender; };
enum Page { PAGE_HOME, PAGE_DICE, PAGE_VOICE, PAGE_SETTINGS };
enum VoiceState { VOICE_READY, VOICE_RECORDING, VOICE_SENDING, VOICE_PLAYING, VOICE_ERROR };

static Preferences prefs;
static Page page = PAGE_HOME, settingsReturn = PAGE_HOME;
static VoiceState voiceState = VOICE_READY;
static char serverBase[112] = DEFAULT_SERVER;
static char banner[64] = "";
static uint32_t bannerUntil = 0, lastTouchAt = 0, lastListAt = 0, lastActivity = 0;
static uint8_t brightness = 160, volume = 70;
static int messageCount = 0;
static bool voiceContactChosen = false;
static Message messages[MAX_MESSAGES];
static uint32_t lastInboxPollAt = 0;
static bool inboxCheckRequested = true;
static bool screenLocked = false;
static bool inboxSeedTried = false;
static int16_t *pcm = nullptr;
static size_t pcmCap = SAMPLE_RATE * MAX_REC_MS / 1000;
static dice::Renderer diceRenderer;
static uint16_t *diceBig = nullptr;
static dice::Quat diceQ = {1, 0, 0, 0}, diceTargetQ = {1, 0, 0, 0};
static uint32_t rollAt = 0, diceNextAt = 0;
static bool rolling = false;
static int diceValue = 1, rollFromValue = 1, rollToValue = 1;
static float shakeScore = 0;
static uint16_t rollFrames = 0;
static volatile bool diceSoundActive = false, diceSoundCancel = false;

static WiFiManager wm;
static WiFiManagerParameter *serverParam = nullptr;
static bool wmReady = false;
static bool portalRunning = false;
static uint32_t portalUntil = 0;
static bool joinDone = false;
static bool loggedOnline = false;
static uint32_t joinUntil = 0;

struct WifiNet {
  const char *ssid;
  const char *pass;
};
static WifiNet wifiNets[2];
static int wifiNetCount = 0;
static int wifiNetAt = 0;

static void logf(const char *fmt, ...) {
  char buf[180];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (Serial.availableForWrite() >= (int)strlen(buf)) Serial.print(buf);
}

// Arduino-ESP32 leaves loopTask off the task watchdog (loopTaskWDTEnabled
// starts false). Only the idle tasks are subscribed. esp_task_wdt_delete on
// this task logs "task not found" and changes nothing, so it is not called.
// delay() blocks this task and lets the idle task run, which is what feeds
// the watchdog. HTTPClient already does that internally; feedWdt() is the
// same one-tick wait for our own record and playback loops.
static void feedWdt() { delay(1); }

static void noteActivity() { lastActivity = millis(); }

static void notice(const char *s, uint32_t ms = 2600) {
  strlcpy(banner, s, sizeof(banner));
  bannerUntil = millis() + ms;
}

static uint32_t utf8Next(const char *&p) {
  uint8_t a = (uint8_t)*p++;
  if (a < 0x80) return a;
  if ((a & 0xE0) == 0xC0) return ((a & 31) << 6) | (*p++ & 63);
  if ((a & 0xF0) == 0xE0) {
    uint32_t v = (a & 15) << 12;
    v |= ((uint8_t)*p++ & 63) << 6;
    return v | (*p++ & 63);
  }
  return '?';
}

struct UiCanvas {
  void drawPixel(int x, int y, uint16_t c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) diceBig[(size_t)y * SCR_W + x] = c;
  }
  void fillScreen(uint16_t c) {
    if (!diceBig) return;
    for (size_t i = 0; i < (size_t)SCR_W * SCR_H; ++i) diceBig[i] = c;
  }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    int x1 = max(0, x), y1 = max(0, y);
    int x2 = min(SCR_W, x + w), y2 = min(SCR_H, y + h);
    for (int yy = y1; yy < y2; ++yy) {
      uint16_t *row = diceBig + (size_t)yy * SCR_W;
      for (int xx = x1; xx < x2; ++xx) row[xx] = c;
    }
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    r = min(r, min(w, h) / 2);
    for (int yy = 0; yy < h; ++yy) {
      int inset = 0;
      if (yy < r) {
        float d = r - yy - 0.5f;
        inset = r - (int)sqrtf(max(0.0f, r * r - d * d));
      } else if (yy >= h - r) {
        float d = yy - (h - r) + 0.5f;
        inset = r - (int)sqrtf(max(0.0f, r * r - d * d));
      }
      fillRect(x + inset, y + yy, w - 2 * inset, 1, c);
    }
  }
  void fillCircle(int cx, int cy, int r, uint16_t c) {
    for (int yy = -r; yy <= r; ++yy) {
      int dx = (int)sqrtf(max(0.0f, (float)(r * r - yy * yy)));
      fillRect(cx - dx, cy + yy, dx * 2 + 1, 1, c);
    }
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int minx = max(0, min(x0, min(x1, x2)));
    int maxx = min(SCR_W - 1, max(x0, max(x1, x2)));
    int miny = max(0, min(y0, min(y1, y2)));
    int maxy = min(SCR_H - 1, max(y0, max(y1, y2)));
    int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (!area) return;
    for (int y = miny; y <= maxy; ++y) {
      for (int x = minx; x <= maxx; ++x) {
        int a = (x1 - x) * (y2 - y) - (y1 - y) * (x2 - x);
        int b = (x2 - x) * (y0 - y) - (y2 - y) * (x0 - x);
        int d = (x0 - x) * (y1 - y) - (y0 - y) * (x1 - x);
        if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) drawPixel(x, y, c);
      }
    }
  }
  void flush() { board::push(diceBig); }
} ui;

static void drawText(const char *s, int x, int y, uint16_t color, int scale = 1) {
  int cx = x;
  while (*s) {
    uint32_t cp = utf8Next(s);
    int lo = 0, hi = (int)(sizeof(ZH_CODES) / sizeof(ZH_CODES[0])) - 1, found = -1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (ZH_CODES[m] < cp) lo = m + 1;
      else if (ZH_CODES[m] > cp) hi = m - 1;
      else { found = m; break; }
    }
    if (found >= 0) {
      int cell = scale == 1 ? 32 : 40;
      const uint8_t *glyph = scale == 1 ? ZH_ALPHA_32[found] : ZH_ALPHA_40[found];
      for (int py = 0; py < cell; ++py) {
        for (int px = 0; px < cell; ++px) {
          int xx = cx + px, yy = y + py;
          if ((unsigned)xx >= SCR_W || (unsigned)yy >= SCR_H) continue;
          int bit = py * cell + px;
          uint8_t packed = pgm_read_byte(glyph + bit / 2);
          int a = (bit & 1) ? (packed & 15) : (packed >> 4);
          if (a < 5) continue;  // hard edge: AMOLED smears 4-bit alpha
          ui.drawPixel(xx, yy, color);
        }
      }
    }
    cx += cp >= 32 && cp < 127 ? (scale == 1 ? ASCII_ADV_32[cp - 32] : ASCII_ADV_40[cp - 32])
                               : (scale == 1 ? 32 : 40);
  }
}

static void drawCenteredText(const char *s, int y, uint16_t color, int scale = 1) {
  const char *p = s;
  int width = 0;
  while (*p) {
    uint32_t cp = utf8Next(p);
    width += cp >= 32 && cp < 127 ? (scale == 1 ? ASCII_ADV_32[cp - 32] : ASCII_ADV_40[cp - 32])
                                  : (scale == 1 ? 32 : 40);
  }
  drawText(s, max(8, (SCR_W - width) / 2), y, color, scale);
}

static void line(int x, int y, int w, uint16_t color = C_LINE) { ui.fillRect(x, y, w, 1, color); }

static void pill(int x, int y, int w, int h, uint16_t fill, uint16_t border = C_LINE) {
  ui.fillRoundRect(x, y, w, h, h / 3, border);
  ui.fillRoundRect(x + 2, y + 2, w - 4, h - 4, h / 3 - 2, fill);
}

static void drawBack(const char *title) {
  ui.fillRect(0, 0, SCR_W, 100, C_BG);
  ui.fillTriangle(124, 52, 146, 36, 146, 68, C_WHITE);
  drawText(title, 164, 36, C_WHITE);
  line(120, 92, SCR_W - 240);
}

static void drawHome() {
  ui.fillScreen(C_BG);
  ui.fillRoundRect(116, 56, 234, 64, 18, C_CARD);
  drawCenteredText("Family Hub", 72, C_WHITE);

  pill(84, HOME_DICE_Y, 298, HOME_DICE_H, 0xFE12, 0xFE12);
  ui.fillCircle(132, HOME_DICE_Y + 52, 28, C_RED);
  ui.fillCircle(124, HOME_DICE_Y + 44, 4, C_WHITE);
  ui.fillCircle(140, HOME_DICE_Y + 60, 4, C_WHITE);
  drawText("Roll dice", 176, HOME_DICE_Y + 28, C_WHITE, 2);

  pill(84, HOME_VOICE_Y, 298, HOME_VOICE_H, 0xAF19, 0xAF19);
  ui.fillCircle(132, HOME_VOICE_Y + 52, 28, C_TEAL);
  ui.fillRoundRect(124, HOME_VOICE_Y + 38, 14, 20, 7, C_BG);
  ui.fillRect(130, HOME_VOICE_Y + 58, 3, 9, C_BG);
  drawText("Voice notes", 176, HOME_VOICE_Y + 28, C_WHITE, 2);

  pill(150, HOME_SET_Y, 166, HOME_SET_H, C_CARD, C_LINE);
  drawText("Settings", 176, HOME_SET_Y + 4, C_WHITE);
  ui.flush();
}

// p.focal is in 480-canvas units. 1600 puts a tilted cube's corners ~327px
// from center at rest and ~400px at the toss peak; the round glass only
// keeps radius 231 (mask is W/2-2). 660 is sized from that projected
// bounding radius — not the face width — so the worst corner during the
// roll, including the 78px toss, stays ~17px inside the mask.
static constexpr float kDiceFocal = 660.0f;

static void renderDie(dice::Quat q, float yOff) {
  diceRenderer.buf = diceBig;
  diceRenderer.W = diceRenderer.H = SCR_W;
  diceRenderer.p.focal = kDiceFocal;
  diceRenderer.p.camZ = 4.2f;
  diceRenderer.renderFast(q, yOff);
}

static float liftToWorld(int liftPx) {
  float focal = kDiceFocal * (SCR_W / 480.0f);
  return liftPx * 4.2f / focal;
}

static void drawDiceChrome() {
  pill(118, 28, 112, 44, C_CARD);
  drawText("Back", 140, 34, C_WHITE);
}

static void drawDice() {
  if (!diceBig) return;
  renderDie(diceQ, 0);
  if (!rolling) drawDiceChrome();
  board::push(diceBig);
}

static void drawRollFrame(float u) {
  int frame = rollmotion::frameAt(rollFromValue, rollToValue, u);
  diceQ = rollmotion::pose(frame);
  renderDie(diceQ, liftToWorld(rollmotion::liftAt(u)));
  board::push(diceBig);
}

static void drawContactAvatar(int cx, int cy, int who) {
  const uint16_t accent = CONTACTS[who].color;
  const uint16_t hair = who == 0 ? 0x39C7 : 0x3186;
  const uint16_t skin = 0xFE15;
  ui.fillCircle(cx, cy, 36, accent);
  ui.fillRoundRect(cx - 22, cy + 16, 44, 16, 8, C_WHITE);
  if (who == 1) {
    ui.fillRoundRect(cx - 22, cy - 22, 44, 46, 18, hair);
    ui.fillCircle(cx, cy - 2, 18, skin);
    ui.fillRoundRect(cx - 18, cy - 24, 36, 12, 6, hair);
  } else {
    ui.fillCircle(cx, cy - 4, 18, skin);
    ui.fillRoundRect(cx - 18, cy - 22, 36, 12, 6, hair);
  }
  ui.fillCircle(cx - 7, cy - 4, 2, hair);
  ui.fillCircle(cx + 7, cy - 4, 2, hair);
  ui.fillRect(cx - 4, cy + 8, 8, 2, hair);
}

static void drawVoice() {
  ui.fillScreen(C_BG);
  if (voiceState == VOICE_RECORDING) {
    ui.fillScreen(C_RED);
    drawCenteredText("Recording", 110, C_WHITE, 2);
    ui.fillCircle(233, 240, 54, C_WHITE);
    ui.fillRoundRect(223, 206, 18, 48, 9, C_RED);
    ui.fillRect(230, 256, 6, 18, C_RED);
    drawCenteredText("0 sec", 320, C_WHITE);
    drawCenteredText("Release to send", 380, C_WHITE);
    ui.flush();
    return;
  }
  int sender = messageCount ? messages[0].sender : 0;
  drawBack("Family");
  const int holdTop = messageCount ? 230 : 120;
  if (messageCount) {
    pill(78, 112, 310, 100, C_CARD, CONTACTS[sender].color);
    drawContactAvatar(124, 162, sender);
    char duration[32];
    snprintf(duration, sizeof(duration), "%s %u sec", CONTACTS[sender].name, messages[0].seconds);
    drawText(duration, 176, 128, C_WHITE);
    drawText(voiceState == VOICE_PLAYING ? "Playing" : "Tap to replay", 176, 168, C_MUTED);
    ui.fillTriangle(348, 148, 348, 176, 370, 162, C_TEAL);
  }
  const int holdH = 400 - holdTop;
  ui.fillRoundRect(78, holdTop, 310, holdH, 26, C_TEAL);
  int center = holdTop + holdH * 2 / 5;
  ui.fillCircle(233, center, 36, C_WHITE);
  ui.fillRoundRect(225, center - 22, 16, 34, 8, C_TEAL);
  ui.fillRect(231, center + 10, 5, 14, C_TEAL);
  drawCenteredText(voiceState == VOICE_SENDING ? "Sending" : "Hold to talk",
                   holdTop + holdH * 3 / 4, C_WHITE, 2);
  if (bannerUntil > millis() && banner[0]) {
    pill(110, 404, 246, 36, C_CARD);
    drawText(banner, 128, 410, C_WHITE);
  }
  ui.flush();
}

static void drawVoiceResult(bool ok) {
  ui.fillScreen(ok ? C_TEAL : C_RED);
  if (ok) {
    ui.fillTriangle(140, 250, 164, 226, 226, 292, C_WHITE);
    ui.fillTriangle(164, 226, 250, 314, 226, 292, C_WHITE);
    ui.fillTriangle(226, 292, 250, 314, 340, 180, C_WHITE);
    ui.fillTriangle(226, 292, 320, 160, 340, 180, C_WHITE);
  } else {
    drawCenteredText("Try again", 220, C_WHITE, 2);
  }
  ui.flush();
}

static void drawSlider(int y, const char *name, int value, uint16_t color) {
  drawText(name, SL_X, y, C_WHITE);
  char v[10];
  snprintf(v, sizeof(v), "%d%%", value);
  drawText(v, SL_X + SL_W - 70, y, C_MUTED);
  pill(SL_X, y + 40, SL_W, 22, C_CARD);
  int width = value * (SL_W - 36) / 100;
  if (width > 0) ui.fillRoundRect(SL_X + 8, y + 45, width, 12, 6, color);
  ui.fillCircle(SL_X + 8 + width, y + 51, 12, C_WHITE);
}

static const char *wifiWord() {
  if (portalRunning) return "Setup portal";
  if (!joinDone) return "Joining";
  return WiFi.status() == WL_CONNECTED ? "Connected" : "Offline";
}

static void drawSettings() {
  ui.fillScreen(C_BG);
  drawBack("Settings");
  drawSlider(112, "Brightness", brightness * 100 / 255, C_GOLD);
  drawSlider(210, "Volume", volume, C_TEAL);
  pill(90, 328, 286, 56, C_CARD);
  drawText("Reconnect Wi-Fi", 108, 342, C_WHITE);
  drawText(wifiWord(), 108, 400, C_MUTED);
  ui.flush();
}

static void drawPortal() {
  ui.fillScreen(C_BG);
  drawBack("Wi-Fi setup");
  drawText("Connect phone to", 96, 160, C_WHITE);
  drawText("FamilyHub-Setup", 96, 208, C_TEAL);
  drawText("Open 192.168.4.1", 96, 260, C_MUTED);
  drawText("Home key cancels", 96, 330, C_MUTED);
  ui.flush();
}

static void drawLocked() { ui.fillScreen(0); }

static void redraw() {
  if (portalRunning) { drawPortal(); return; }
  switch (page) {
    case PAGE_HOME: drawHome(); break;
    case PAGE_DICE: drawDice(); break;
    case PAGE_VOICE: drawVoice(); break;
    case PAGE_SETTINGS: drawSettings(); break;
  }
}

static void setPage(Page p) {
  if (page == PAGE_DICE && p != PAGE_DICE) {
    diceSoundCancel = true;
    rolling = false;
  }
  if (p == PAGE_VOICE && page != PAGE_VOICE) {
    voiceContactChosen = true;
    inboxCheckRequested = true;
  }
  page = p;
  if (p == PAGE_VOICE) lastListAt = 0;
  redraw();
}

static void savePrefs() {
  prefs.putUChar("bright", brightness);
  prefs.putUChar("volume", volume);
}

static void wakeScreen() {
  if (!screenLocked) return;
  screenLocked = false;
  board::wakeDisplay(brightness);
  noteActivity();
  logf("[pwr] screen on\n");
}

static void lockScreen(const char *why) {
  if (screenLocked) return;
  if (rolling) {
    diceValue = rollToValue;
    diceQ = diceTargetQ;
    rolling = false;
  }
  diceSoundCancel = true;
  screenLocked = true;
  drawLocked();
  ui.flush();
  board::sleepDisplay();
  logf("[pwr] screen off (%s)\n", why);
}

static void wavHeader(uint8_t *h, uint32_t bytes) {
  memcpy(h, "RIFF", 4);
  uint32_t v32 = 36 + bytes;
  memcpy(h + 4, &v32, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  v32 = 16;
  memcpy(h + 16, &v32, 4);
  uint16_t v16 = 1;
  memcpy(h + 20, &v16, 2);
  memcpy(h + 22, &v16, 2);
  v32 = SAMPLE_RATE;
  memcpy(h + 24, &v32, 4);
  v32 = SAMPLE_RATE * 2;
  memcpy(h + 28, &v32, 4);
  v16 = 2;
  memcpy(h + 32, &v16, 2);
  v16 = 16;
  memcpy(h + 34, &v16, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &bytes, 4);
}

static size_t recordAudio() {
  if (!pcm || !board::micConfigured()) return 0;
  banner[0] = 0;
  voiceState = VOICE_RECORDING;
  drawVoice();
  noteActivity();
  if (!board::micBegin()) {
    logf("[rec] mic begin failed\n");
    voiceState = VOICE_READY;
    return 0;
  }
  uint32_t start = millis(), lastDraw = start, lastFingerAt = start;
  size_t written = 0;
  const size_t chunk = 1024;
  while (millis() - start < MAX_REC_MS && written + chunk <= pcmCap) {
    feedWdt();
    if (!board::micRecord(pcm + written, chunk, SAMPLE_RATE)) break;
    written += chunk;
    board::poll();
    uint32_t now = millis();
    auto k = board::keys();
    if (k.homeDown || k.nextDown) break;
    int16_t hx = 0, hy = 0;
    if (board::readTouch(hx, hy)) lastFingerAt = now;
    if (now - start > MIN_REC_MS && now - lastFingerAt > 220) break;
    if (now - lastDraw > 250) {
      lastDraw = now;
      char t[18];
      snprintf(t, sizeof(t), "%u sec", (unsigned)((now - start) / 1000));
      ui.fillRect(150, 310, 166, 36, C_RED);
      drawCenteredText(t, 316, C_WHITE);
      ui.flush();
    }
  }
  board::micEnd();
  size_t first = 0;
  while (first < written && abs(pcm[first]) < 500) ++first;
  size_t preroll = SAMPLE_RATE * 80 / 1000;
  size_t trim = first > preroll ? first - preroll : 0;
  if (trim > 0 && trim < written) {
    memmove(pcm, pcm + trim, (written - trim) * sizeof(int16_t));
    written -= trim;
  }
  int32_t peak = 0;
  for (size_t i = 0; i < written; ++i) peak = max(peak, (int32_t)abs(pcm[i]));
  float gain = 1.0f;
  if (peak > 200) {
    gain = (32768.0f * 0.707f) / (float)peak;
    if (gain > 12.0f) gain = 12.0f;
    if (gain < 0.5f) gain = 0.5f;
    for (size_t i = 0; i < written; ++i) {
      int32_t v = (int32_t)(pcm[i] * gain);
      pcm[i] = (int16_t)constrain(v, -32768, 32767);
    }
  }
  logf("[rec] samples=%u peak=%d gain=%.2f\n", (unsigned)written, (int)peak, (double)gain);
  noteActivity();
  return written;
}

static bool uploadAudio(size_t count) {
  if (WiFi.status() != WL_CONNECTED || !serverBase[0]) return false;
  size_t total = 44 + count * 2;
  uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  if (!body) return false;
  wavHeader(body, count * 2);
  memcpy(body + 44, pcm, count * 2);
  char url[200];
  snprintf(url, sizeof(url), "%s/voice?from=%s&to=%s", serverBase, SEND_CONTACT, "dad");
  WiFiClient client;
  HTTPClient http;
  bool ok = false;
  if (http.begin(client, url)) {
    http.setConnectTimeout(4000);
    http.setTimeout(8000);
    http.addHeader("Content-Type", "audio/wav");
    if (*INBOX_SECRET) http.addHeader("X-Voice-Secret", INBOX_SECRET);
    int status = http.POST(body, total);
    ok = status == 200;
    logf("[upload] status=%d bytes=%u\n", status, (unsigned)total);
    http.end();
  }
  free(body);
  noteActivity();
  return ok;
}

static bool playMessage(int idx);

static void fetchMessages() {
  lastListAt = millis();
  messageCount = 0;
  if (WiFi.status() != WL_CONNECTED || !serverBase[0]) {
    if (page == PAGE_VOICE) drawVoice();
    return;
  }
  inboxCheckRequested = false;
  char url[184];
  snprintf(url, sizeof(url), "%s/api/list?direction=in&limit=3", serverBase);
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return;
  http.setReuse(false);
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  int listStatus = http.GET();
  String body;
  if (listStatus == 200 && http.getSize() < 65536) body = http.getString();
  if (listStatus == 200 && body.length()) {
    cJSON *root = cJSON_Parse(body.c_str());
    cJSON *items = root ? cJSON_GetObjectItem(root, "items") : nullptr;
    if (cJSON_IsArray(items)) {
      cJSON *item = nullptr;
      cJSON_ArrayForEach(item, items) {
        cJSON *c = cJSON_GetObjectItem(item, "contact");
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *direction = cJSON_GetObjectItem(item, "direction");
        if (!cJSON_IsString(c) || !cJSON_IsString(id) || !cJSON_IsString(direction)) continue;
        if (strcmp(direction->valuestring, "in")) continue;
        Message &m = messages[messageCount];
        strlcpy(m.id, id->valuestring, sizeof(m.id));
        cJSON *seconds = cJSON_GetObjectItem(item, "seconds");
        m.seconds = cJSON_IsNumber(seconds) ? (uint16_t)ceil(seconds->valuedouble) : 0;
        m.incoming = true;
        m.sender = !strcmp(c->valuestring, "mom") ? 1 : 0;
        cJSON *ts = cJSON_GetObjectItem(item, "ts");
        m.ts = cJSON_IsNumber(ts) ? (uint32_t)(ts->valuedouble / 1000) : 0;
        if (++messageCount == MAX_MESSAGES) break;
      }
    }
    cJSON_Delete(root);
  }
  http.end();
  if (page == PAGE_VOICE) drawVoice();
  if (messageCount && page == PAGE_VOICE && voiceState == VOICE_READY && !rolling && !diceSoundActive) {
    uint64_t cursor = prefs.getULong64("inbox-ts", 0);
    uint64_t newest = (uint64_t)messages[0].ts * 1000ULL;
    if (newest > cursor && playMessage(0)) prefs.putULong64("inbox-ts", newest);
  }
}

static bool playMessage(int idx) {
  if (!board::speakerOk() || idx < 0 || idx >= messageCount) return false;
  if (WiFi.status() != WL_CONNECTED || !serverBase[0]) return false;
  char url[190];
  snprintf(url, sizeof(url), "%s/audio/%s", serverBase, messages[idx].id);
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setConnectTimeout(2000);
  http.setTimeout(6000);
  bool ok = false;
  size_t played = 0;
  int audioStatus = http.GET();
  if (audioStatus == 200) {
    int len = http.getSize();
    if (len > 44 && len <= 2 * 1024 * 1024) {
      uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
      if (buf) {
        WiFiClient *stream = http.getStreamPtr();
        size_t got = stream->readBytes(buf, len);
        if (got == (size_t)len && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
          uint32_t rate = 0;
          uint16_t ch = 0, bits = 0;
          memcpy(&rate, buf + 24, 4);
          memcpy(&ch, buf + 22, 2);
          memcpy(&bits, buf + 34, 2);
          if (rate >= 8000 && rate <= 48000 && ch == 1 && bits == 16) {
            voiceState = VOICE_PLAYING;
            if (page == PAGE_VOICE) drawVoice();
            board::playWav(buf, len);
            delay(30);
            uint32_t t0 = millis();
            while (board::speakerBusy() && millis() - t0 < 25000) {
              feedWdt();
              board::poll();
              int16_t sx = 0, sy = 0;
              if (board::readTouch(sx, sy) && sy < 100) {
                board::speakerStop();
                break;
              }
              auto k = board::keys();
              if (k.homeDown || k.nextDown) {
                board::speakerStop();
                break;
              }
              delay(20);
            }
            played = (size_t)len - 44;
            ok = true;
          }
        }
        free(buf);
      }
    }
  }
  http.end();
  voiceState = VOICE_READY;
  logf("[play] ok=%d bytes=%u\n", ok, (unsigned)played);
  noteActivity();
  return ok;
}

static void pollIncoming() {
  if (portalRunning || WiFi.status() != WL_CONNECTED || !serverBase[0]) return;
  if (rolling || diceSoundActive || voiceState != VOICE_READY) return;
  if (page == PAGE_SETTINGS && !screenLocked) return;
  inboxCheckRequested = false;
  char url[220];
  snprintf(url, sizeof(url), "%s/api/list?direction=in&limit=5%s%s", serverBase,
           LISTEN_CONTACT[0] ? "&contact=" : "", LISTEN_CONTACT[0] ? LISTEN_CONTACT : "");
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return;
  http.setReuse(true);
  http.setConnectTimeout(800);
  http.setTimeout(1200);
  int pollStatus = http.GET();
  String body;
  if (pollStatus == 200) body = http.getString();
  http.end();
  if (pollStatus != 200) return;
  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *items = root ? cJSON_GetObjectItem(root, "items") : nullptr;
  uint64_t cursor = prefs.getULong64("inbox-ts", 0);
  cJSON *next = nullptr;
  uint64_t nextTs = UINT64_MAX;
  if (cJSON_IsArray(items)) {
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
      cJSON *ts = cJSON_GetObjectItem(item, "ts");
      if (!cJSON_IsNumber(ts)) continue;
      uint64_t t = (uint64_t)ts->valuedouble;
      if (t > cursor && t < nextTs) {
        next = item;
        nextTs = t;
      }
    }
  }
  if (next) {
    cJSON *id = cJSON_GetObjectItem(next, "id");
    cJSON *who = cJSON_GetObjectItem(next, "contact");
    cJSON *seconds = cJSON_GetObjectItem(next, "seconds");
    if (cJSON_IsString(id)) {
      Message pending{};
      strlcpy(pending.id, id->valuestring, sizeof(pending.id));
      pending.sender = cJSON_IsString(who) && !strcmp(who->valuestring, "mom") ? 1 : 0;
      pending.seconds = cJSON_IsNumber(seconds) ? (uint16_t)ceil(seconds->valuedouble) : 0;
      pending.incoming = true;
      wakeScreen();
      page = PAGE_VOICE;
      voiceContactChosen = true;
      messages[0] = pending;
      messageCount = 1;
      board::vibrate(25, 50);
      logf("[inbox] new voice\n");
      if (playMessage(0)) prefs.putULong64("inbox-ts", nextTs);
      else logf("[inbox] play failed wifi=%d\n", WiFi.status() == WL_CONNECTED);
      if (page == PAGE_VOICE) drawVoice();
    }
  }
  cJSON_Delete(root);
}

static void seedInboxOnce() {
  if (inboxSeedTried || prefs.isKey("inbox-ts") || !serverBase[0]) return;
  inboxSeedTried = true;
  WiFiClient client;
  HTTPClient http;
  char url[180];
  snprintf(url, sizeof(url), "%s/api/list?direction=in&limit=1", serverBase);
  if (!http.begin(client, url)) return;
  http.setTimeout(1500);
  int seedStatus = http.GET();
  String body;
  if (seedStatus == 200) body = http.getString();
  if (seedStatus == 200) {
    cJSON *root = cJSON_Parse(body.c_str());
    cJSON *items = root ? cJSON_GetObjectItem(root, "items") : nullptr;
    cJSON *first = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : nullptr;
    cJSON *ts = first ? cJSON_GetObjectItem(first, "ts") : nullptr;
    prefs.putULong64("inbox-ts", cJSON_IsNumber(ts) ? (uint64_t)ts->valuedouble : 0);
    cJSON_Delete(root);
  }
  http.end();
}

static void ensureWm() {
  if (wmReady) return;
  serverParam = new WiFiManagerParameter("srv", "Server URL", serverBase, 110);
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(20);
  wm.setCaptivePortalEnable(true);
  wm.addParameter(serverParam);
  wmReady = true;
}

static void startPortal() {
  ensureWm();
  if (portalRunning) return;
  drawPortal();
  wm.startConfigPortal("FamilyHub-Setup");
  portalRunning = true;
  portalUntil = millis() + 180000;
  noteActivity();
  logf("[wifi] portal open\n");
}

static void closePortal() {
  if (!portalRunning) return;
  wm.stopConfigPortal();
  portalRunning = false;
  const char *s = serverParam ? serverParam->getValue() : nullptr;
  if (s && !strncmp(s, "http://", 7)) {
    strlcpy(serverBase, s, sizeof(serverBase));
    prefs.putString("server", serverBase);
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  loggedOnline = false;
  joinDone = true;
  logf("[wifi] portal closed status=%d\n", (int)WiFi.status());
  if (!screenLocked) redraw();
}

static void beginWifiNet(int index) {
  wifiNetAt = index;
  if (index < wifiNetCount) {
    logf("[wifi] try %s\n", wifiNets[index].ssid);
    WiFi.begin(wifiNets[index].ssid, wifiNets[index].pass);
  } else {
    logf("[wifi] try saved\n");
    WiFi.begin();
  }
  joinUntil = millis() + 12000;
}

static void serviceWifi() {
  if (portalRunning) {
    wm.process();
    if (!wm.getConfigPortalActive() || (int32_t)(millis() - portalUntil) >= 0) closePortal();
    return;
  }
  if (!joinDone) {
    if (WiFi.status() == WL_CONNECTED) {
      joinDone = true;
      WiFi.setAutoReconnect(true);
    } else if ((int32_t)(millis() - joinUntil) >= 0) {
      int next = wifiNetAt + 1;
      if (next < wifiNetCount) {
        WiFi.disconnect(false, false, 0);
        beginWifiNet(next);
      } else {
        joinDone = true;
        logf("[wifi] offline\n");
      }
    }
  }
  if (WiFi.status() == WL_CONNECTED && !loggedOnline) {
    loggedOnline = true;
    WiFi.setSleep(false);
    logf("[wifi] %s %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    seedInboxOnce();
  } else if (WiFi.status() != WL_CONNECTED) {
    loggedOnline = false;
  }
}

static void diceSoundTask(void *) {
  const int total = SAMPLE_RATE * 9 / 10;
  int16_t *buf = (int16_t *)heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (buf) {
    uint32_t noise = esp_random();
    float filtered = 0;
    for (int i = 0; i < total; ++i) {
      float t = i / (float)SAMPLE_RATE;
      float rattle = 0;
      const float hits[] = {0.02f, 0.17f, 0.35f, 0.55f, 0.70f};
      for (int h = 0; h < 5; ++h) {
        float age = t - hits[h];
        if (age >= 0 && age < 0.055f) {
          float env = 1.0f - age / 0.055f;
          rattle += env * env * (0.45f + 0.08f * h);
        }
      }
      noise = noise * 1664525u + 1013904223u;
      float white = ((int32_t)(noise >> 16) - 32768) / 32768.0f;
      filtered += 0.19f * (white - filtered);
      float landAge = t - 0.76f;
      float thud = landAge >= 0 && landAge < 0.17f
                       ? sinf(2.0f * PI * (150.0f - 200.0f * landAge) * landAge) * (1.0f - landAge / 0.17f)
                       : 0;
      buf[i] = (int16_t)(filtered * rattle * 6500 + thud * 5000);
    }
    board::playRaw(buf, total, SAMPLE_RATE);
    while (board::speakerBusy() && !diceSoundCancel) delay(20);
    if (diceSoundCancel) board::speakerStop();
    free(buf);
  }
  diceSoundActive = false;
  vTaskDelete(nullptr);
}

static void rollDice() {
  if (rolling || page != PAGE_DICE) return;
  rollFromValue = diceValue;
  rollToValue = 1 + (esp_random() % 6);
  diceTargetQ = dice::targetForValue(rollToValue, 0.1f);
  rolling = true;
  rollAt = millis();
  rollFrames = 1;
  diceNextAt = rollAt;
  noteActivity();
  drawRollFrame(0.045f);
  if (board::speakerOk() && !diceSoundActive && voiceState == VOICE_READY) {
    diceSoundCancel = false;
    diceSoundActive = true;
    if (xTaskCreatePinnedToCore(diceSoundTask, "dice-sound", 6144, nullptr, 1, nullptr, 0) != pdPASS)
      diceSoundActive = false;
  }
}

static void updateDice() {
  if (page != PAGE_DICE || !rolling) return;
  uint32_t now = millis();
  if ((int32_t)(now - diceNextAt) < 0) return;
  float u = fminf(1.0f, float(now - rollAt) / ROLL_MS);
  if (u >= 1.0f) {
    diceValue = rollToValue;
    diceQ = diceTargetQ;
    rolling = false;
    drawDice();
    board::vibrate(40, 70);
    logf("[dice] result=%d frames=%u ms=%lu\n", diceValue, rollFrames, (unsigned long)(now - rollAt));
    return;
  }
  drawRollFrame(u);
  ++rollFrames;
  diceNextAt = millis() + 8;
}

static int sliderFromX(int x, int full) {
  int w = SL_W - 36;
  if (w < 1) w = 1;
  return constrain((x - (SL_X + 8)) * full / w, 0, full);
}

static void handleTap(int x, int y) {
  if (millis() - lastTouchAt < 260) return;
  lastTouchAt = millis();
  noteActivity();
  logf("[tap] x=%d y=%d page=%d\n", x, y, (int)page);
  if (page == PAGE_HOME) {
    if (y >= HOME_DICE_Y && y < HOME_DICE_Y + HOME_DICE_H) setPage(PAGE_DICE);
    else if (y >= HOME_VOICE_Y && y < HOME_VOICE_Y + HOME_VOICE_H) {
      setPage(PAGE_VOICE);
      fetchMessages();
    } else if (y >= HOME_SET_Y) {
      settingsReturn = PAGE_HOME;
      setPage(PAGE_SETTINGS);
    }
  } else if (page == PAGE_DICE) {
    if (x < 250 && y < 90) setPage(PAGE_HOME);
    else rollDice();
  } else if (page == PAGE_VOICE) {
    const int holdTop = messageCount ? 230 : 120;
    if (x < 250 && y < 100) setPage(PAGE_HOME);
    else if (messageCount && y >= 112 && y < 212) {
      if (!playMessage(0)) notice("Playback failed");
      drawVoice();
    } else if (y >= holdTop) {
      size_t n = recordAudio();
      bool sent = false;
      if (n >= SAMPLE_RATE * MIN_REC_MS / 1000) {
        voiceState = VOICE_SENDING;
        drawVoice();
        sent = uploadAudio(n);
      }
      banner[0] = 0;
      drawVoiceResult(sent);
      delay(700);
      voiceState = VOICE_READY;
      drawVoice();
    }
  } else if (page == PAGE_SETTINGS) {
    if (x < 250 && y < 100) setPage(settingsReturn);
    else if (y >= 150 && y <= 196) {
      brightness = (uint8_t)constrain(sliderFromX(x, 255), 12, 255);
      board::setBrightness(brightness);
      savePrefs();
      drawSettings();
    } else if (y >= 248 && y <= 294) {
      volume = (uint8_t)sliderFromX(x, 100);
      board::setVolumePercent(volume);
      savePrefs();
      drawSettings();
    } else if (y >= 328 && y <= 384) startPortal();
  }
}

static void serviceSerial() {
  if (!Serial.available()) return;
  char cmd = Serial.read();
  noteActivity();
  if (cmd == 'r') { setPage(PAGE_DICE); rollDice(); }
  else if (cmd == 'h') setPage(PAGE_HOME);
  else if (cmd == 'v') {
    setPage(PAGE_VOICE);
    fetchMessages();
    logf("[voice] %d messages\n", messageCount);
  } else if (cmd == 'p' && page == PAGE_VOICE) {
    if (!playMessage(0)) notice("Playback failed");
    drawVoice();
  } else if (cmd == 'c') inboxCheckRequested = true;
  else if (cmd == 'g') setPage(PAGE_SETTINGS);
  else if (cmd == 'l') lockScreen("serial");
  else if (cmd == 's') {
    logf("[status] touch=%d imu=%d spk=%d mic=%d wifi=%d locked=%d psram=%uKB ip=%s\n",
         board::touchOk(), board::imuOk(), board::speakerOk(), board::micConfigured(),
         WiFi.status() == WL_CONNECTED, screenLocked, (unsigned)(ESP.getFreePsram() / 1024),
         WiFi.localIP().toString().c_str());
  }
}

#if defined(SW_SELFTEST)
static void runSelfTest() {
  board::setBrightness(160);
  M5.Display.fillScreen(0);
  M5.Display.setTextColor(0xFFFF, 0);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(48, 90);
  M5.Display.printf("board %d", board::boardId());
  M5.Display.setCursor(48, 130);
  M5.Display.printf("panel %dx%d", board::panelWidth(), board::panelHeight());
  M5.Display.setCursor(48, 170);
  M5.Display.printf("psram %uM", (unsigned)(ESP.getPsramSize() / 1024 / 1024));
  int16_t lastX = -2, lastY = -2;
  for (;;) {
    board::poll();
    int16_t x = -1, y = -1;
    bool down = board::readTouch(x, y);
    m5gfx::touch_point_t raw{};
    int nraw = M5.Display.getTouchRaw(&raw, 1);
    auto k = board::keys();
    if (k.homeDown) board::vibrate(20, 40);
    if (k.nextDown) board::vibrate(40, 80);
    if (!down) { x = -1; y = -1; }
    if (x != lastX || y != lastY || k.homeDown || k.nextDown) {
      lastX = x;
      lastY = y;
      M5.Display.fillRect(30, 220, 420, 140, 0);
      M5.Display.setCursor(48, 230);
      if (nraw > 0) M5.Display.printf("raw %d,%d", (int)raw.x, (int)raw.y);
      else M5.Display.print("raw -,-");
      M5.Display.setCursor(48, 270);
      if (down) M5.Display.printf("xy %d,%d", x, y);
      else M5.Display.print("xy -,-  touch");
      M5.Display.setCursor(48, 310);
      M5.Display.printf("G1 %d  G2 %d", digitalRead(board::kPinHome), digitalRead(board::kPinNext));
    }
    delay(30);
  }
}
#endif

void setup() {
  board::begin();
#if defined(SW_SELFTEST)
  runSelfTest();
#endif
  logf("[hub] role=%s send=%s listen=%s\n", DEVICE_ROLE, SEND_CONTACT, LISTEN_CONTACT);
  prefs.begin("family-hub", false);
  brightness = prefs.getUChar("bright", 160);
  uint8_t storedVol = prefs.getUChar("volume", 70);
  volume = storedVol > 100 ? (uint8_t)((storedVol * 100 + 127) / 255) : storedVol;
  String saved = prefs.getString("server", DEFAULT_SERVER);
  strlcpy(serverBase, saved.c_str(), sizeof(serverBase));
  board::setBrightness(brightness);
  board::setVolumePercent(volume);

  diceBig = (uint16_t *)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
  pcm = (int16_t *)heap_caps_malloc(pcmCap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!diceBig || !pcm) {
    logf("[mem] framebuffer alloc failed\n");
    while (true) {
      feedWdt();
      delay(1000);
    }
  }
  logf("[mem] psram free=%uKB\n", (unsigned)(ESP.getFreePsram() / 1024));
  diceQ = dice::targetForValue(1, 0.1f);
  drawHome();
  noteActivity();

  if (WIFI_SSID[0]) wifiNets[wifiNetCount++] = {WIFI_SSID, WIFI_PASS};
  if (WIFI_SSID2[0]) wifiNets[wifiNetCount++] = {WIFI_SSID2, WIFI_PASS2};
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // Stay off until one candidate associates. Otherwise a failed first
  // network is retried in the background and crowds out WIFI_SSID2.
  WiFi.setAutoReconnect(false);
  WiFi.persistent(true);
  beginWifiNet(wifiNetCount ? 0 : 1);
  logf("[hub] ready\n");
}

void loop() {
  board::poll();
  auto k = board::keys();
  serviceWifi();

  if (portalRunning) {
    if (k.homeShort || k.homeLong || k.homeDown) closePortal();
    noteActivity();
    delay(10);
    return;
  }

  if (screenLocked) {
    if (k.homeDown || k.nextDown) {
      wakeScreen();
      redraw();
      delay(180);
    } else if (inboxCheckRequested || millis() - lastInboxPollAt > kPollMs) {
      lastInboxPollAt = millis();
      pollIncoming();
    }
    delay(20);
    return;
  }

  if (k.homeDown || k.nextDown || k.homeShort || k.nextShort || k.homeLong || k.nextLong) noteActivity();
  if (k.homeLong || k.nextLong) {
    lockScreen("button");
    delay(200);
    return;
  }
  if (k.homeShort) setPage(PAGE_HOME);
  else if (k.nextShort) setPage((Page)((page + 1) % 4));

  serviceSerial();

  int16_t x = 0, y = 0;
  if (board::readTouch(x, y)) handleTap(x, y);
  if (portalRunning) {
    delay(10);
    return;
  }

  if (page == PAGE_DICE && board::imuOk() && !rolling) {
    static uint32_t lastShake = 0;
    if (millis() - lastShake > 20) {
      lastShake = millis();
      float ax, ay, az;
      if (board::readAccel(ax, ay, az)) {
        float mag = sqrtf(ax * ax + ay * ay + az * az);
        shakeScore = fabsf(mag - 1.0f) > 0.55f ? shakeScore + 1 : shakeScore * 0.75f;
        if (shakeScore > 4) {
          shakeScore = 0;
          rollDice();
        }
      }
    }
  }

  updateDice();

  const bool touching = board::readTouch(x, y);
  if (touching) noteActivity();
  if (!touching && !portalRunning && voiceState == VOICE_READY &&
      (inboxCheckRequested || millis() - lastInboxPollAt > kPollMs)) {
    lastInboxPollAt = millis();
    pollIncoming();
  }
  if (page == PAGE_VOICE && voiceContactChosen && voiceState == VOICE_READY &&
      millis() - lastListAt > 15000) {
    fetchMessages();
  }
  if (page == PAGE_VOICE && banner[0] && millis() > bannerUntil) {
    banner[0] = 0;
    drawVoice();
  }

  if (!rolling && voiceState == VOICE_READY && !diceSoundActive && !portalRunning &&
      millis() - lastActivity > kIdleSleepMs) {
    lockScreen("idle");
  }
  delay(rolling ? 1 : 8);
}

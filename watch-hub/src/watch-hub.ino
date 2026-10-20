/* Family Hub for Waveshare ESP32-S3-Touch-AMOLED-2.16.
 * One display, one touch controller, two apps: SDF dice and voice messages.
 * ES8311 driver is Espressif's Apache-2.0 driver from the board's 07_ES8311 example.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <M5Unified.h>
extern "C" {
#include <cJSON.h>
}
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
#ifndef DEFAULT_SERVER
#define DEFAULT_SERVER ""
#endif
#ifndef INBOX_SECRET
#define INBOX_SECRET ""
#endif


// M5Stack StopWatch：屏 / 触摸 / 电源 / 功放 / 震动全部由 M5Unified 接管
// (M5GFX 驱动 CO5300 QSPI 468x468 offset6，M5IOE1 管屏电源与复位，M5PM1 管电源)
#define KYB_PIN 2   // KEYB（蓝）
#define KYA_PIN 1   // KEYA（黄）
#define SCR_W 466
#define SCR_H 466

// 设备角色：mom = 妈妈机 / baby = 宝宝机 / dad = 爸爸机
#ifndef DEVICE_ROLE
#define DEVICE_ROLE "mom"
#endif
// 本机发出的语音记到谁名下（服务端只接受 dad/mom/me）
#ifndef SEND_CONTACT
#define SEND_CONTACT "mom"
#endif
// 本机监听谁的来信（"dad" / "mom" / "" = 全部）
#ifndef LISTEN_CONTACT
#define LISTEN_CONTACT "dad"
#endif
#define ROLL_MS 960
#define ROLL_FRAMES rollmotion::COUNT
#define ROLL_RES rollmotion::RES
#define SAMPLE_RATE 16000
#define MAX_REC_MS 15000
#define MIN_REC_MS 400
#define MAX_MESSAGES 3

static const uint16_t C_BG = 0xFF9B, C_CARD = 0xFFFF, C_LINE = 0xD651;
static const uint16_t C_WHITE = 0x2126, C_MUTED = 0x632C;
static const uint16_t C_TEAL = 0x76B5, C_PINK = 0xFCB5, C_GOLD = 0xFE0C;
static const uint16_t C_RED = 0xFC51, C_OUT = 0x2B69;

struct Contact { const char *key; const char *name; uint16_t color; };
static const Contact CONTACTS[] = {
  {"dad", "Dad", C_GOLD}, {"mom", "Mom", C_RED}, {"me", "Me", C_TEAL}
};
struct Message { char id[48]; uint32_t ts; uint16_t seconds; bool incoming; uint8_t sender; };
enum Page { PAGE_HOME, PAGE_DICE, PAGE_VOICE, PAGE_SETTINGS };
enum VoiceState { VOICE_READY, VOICE_RECORDING, VOICE_SENDING, VOICE_PLAYING, VOICE_ERROR };

#define USBSerial Serial
static float acc[3] = {0, 0, 0};

// CST820B 原始坐标 0..233 → 468
// M5Unified 的 getDetail() 内部已 convertRawXY 到屏幕坐标，不要再缩放
static inline uint8_t readTouch(int16_t *x, int16_t *y) {
  M5.update();
  auto d = M5.Touch.getDetail();
  if (!d.isPressed()) return 0;
  *x = (int16_t)constrain((int)d.x, 0, SCR_W - 1);
  *y = (int16_t)constrain((int)d.y, 0, SCR_H - 1);
  return 1;
}
// 分带推屏：一次 32 行（约 30KB），避免 434KB 单次 DMA 与触摸 I2C 抢总线导致花屏
static inline void blitFull(const uint16_t *fb) {
  constexpr int kBand = 32;
  M5.Display.startWrite();
  for (int y = 0; y < SCR_H; y += kBand) {
    const int h = (y + kBand <= SCR_H) ? kBand : (SCR_H - y);
    M5.Display.pushImage(0, y, SCR_W, h, fb + (size_t)y * SCR_W);
    delay(1);  // 给触摸中断留出插入机会
  }
  M5.Display.endWrite();
}
static Preferences prefs;
static bool touchOk = false, imuOk = false, audioOk = false, codecOk = false;
static volatile bool touchPending = false;
static int16_t tx[2], ty[2];
static Page page = PAGE_HOME, settingsReturn = PAGE_HOME;
static VoiceState voiceState = VOICE_READY;
static char serverBase[112] = DEFAULT_SERVER;
static char banner[64] = "";
static uint32_t bannerUntil = 0, lastTouchAt = 0, lastListAt = 0;
static uint8_t brightness = 200, volume = 68;
static int contact = 0, messageCount = 0;
static bool voiceContactChosen = false, voiceListening = false;
static Message messages[MAX_MESSAGES];
static uint32_t lastInboxPollAt = 0;
static volatile bool inboxCheckRequested = true;
static bool screenLocked = false;
static int16_t *pcm = nullptr;
static size_t pcmCap = SAMPLE_RATE * MAX_REC_MS / 1000;
static dice::Renderer diceRenderer;
static uint16_t *diceLow = nullptr, *diceBig = nullptr;
static uint16_t *diceFaces = nullptr;
static volatile bool diceFaceReady[6] = {};
static int diceFaceBox[6][4] = {};
static uint16_t *diceAnim = nullptr;
static uint16_t *animCanvas = nullptr;
static volatile bool diceAnimReady = false;
static bool diceCacheFallback = false;
static uint32_t rollCacheNextAt = 0;
static int rollFromValue = 1, rollToValue = 1;
static bool lockReleaseArmed = false;
// All UI primitives render to RAM first. CO5300 QSPI corrupts odd-size
// per-pixel writes; one aligned 480x480 transfer avoids those artifacts.
struct UiCanvas {
  void drawPixel(int x, int y, uint16_t c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) diceBig[y * SCR_W + x] = c;
  }
  void fillScreen(uint16_t c) {
    if (diceBig) for (size_t i = 0; i < (size_t)SCR_W * SCR_H; i++) diceBig[i] = c;
  }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    int x1 = max(0, x), y1 = max(0, y);
    int x2 = min(SCR_W, x + w), y2 = min(SCR_H, y + h);
    for (int yy = y1; yy < y2; yy++) {
      uint16_t *row = diceBig + yy * SCR_W;
      for (int xx = x1; xx < x2; xx++) row[xx] = c;
    }
  }
  void drawRect(int x, int y, int w, int h, uint16_t c) {
    fillRect(x, y, w, 1, c); fillRect(x, y + h - 1, w, 1, c);
    fillRect(x, y, 1, h, c); fillRect(x + w - 1, y, 1, h, c);
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    r = min(r, min(w, h) / 2);
    for (int yy = 0; yy < h; yy++) {
      int inset = 0;
      if (yy < r) { float d = r - yy - 0.5f; inset = r - (int)sqrtf(max(0.0f, r*r - d*d)); }
      else if (yy >= h - r) { float d = yy - (h - r) + 0.5f; inset = r - (int)sqrtf(max(0.0f, r*r - d*d)); }
      fillRect(x + inset, y + yy, w - 2 * inset, 1, c);
    }
  }
  void fillCircle(int cx, int cy, int r, uint16_t c) {
    for (int yy = -r; yy <= r; yy++) {
      int dx = (int)sqrtf(max(0.0f, (float)(r*r - yy*yy)));
      fillRect(cx - dx, cy + yy, dx * 2 + 1, 1, c);
    }
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int minx = max(0, min(x0, min(x1, x2))), maxx = min(SCR_W - 1, max(x0, max(x1, x2)));
    int miny = max(0, min(y0, min(y1, y2))), maxy = min(SCR_H - 1, max(y0, max(y1, y2)));
    int area = (x1-x0)*(y2-y0) - (y1-y0)*(x2-x0);
    if (!area) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
      int a = (x1-x)*(y2-y) - (y1-y)*(x2-x);
      int b = (x2-x)*(y0-y) - (y2-y)*(x0-x);
      int d = (x0-x)*(y1-y) - (y0-y)*(x1-x);
      if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) drawPixel(x,y,c);
    }
  }
  void flush() { blitFull(diceBig); }
} ui;
static dice::Quat diceQ = {1, 0, 0, 0}, diceStartQ = {1, 0, 0, 0}, diceTargetQ = {1, 0, 0, 0};
static uint32_t rollAt = 0, diceFrameAt = 0;
static bool rolling = false;
static int diceValue = 1;
static float shakeScore = 0;
static uint16_t rollFrames = 0;
static uint32_t rollRenderMs = 0, rollScaleMs = 0, rollPushMs = 0;
static int oldX0 = 0, oldY0 = 0, oldX1 = 479, oldY1 = 479;
static volatile bool diceSoundActive = false, diceSoundCancel = false;

static void safeLog(const char *s) {
  if (USBSerial.availableForWrite() >= (int)strlen(s)) USBSerial.print(s);
}
static void notice(const char *s, uint32_t ms = 2600) {
  strlcpy(banner, s, sizeof(banner)); bannerUntil = millis() + ms;
}
static uint32_t utf8Next(const char *&p) {
  uint8_t a = (uint8_t)*p++;
  if (a < 0x80) return a;
  if ((a & 0xE0) == 0xC0) { uint32_t v = (a & 31) << 6; return v | ((uint8_t)*p++ & 63); }
  if ((a & 0xF0) == 0xE0) {
    uint32_t v = (a & 15) << 12; v |= ((uint8_t)*p++ & 63) << 6;
    return v | ((uint8_t)*p++ & 63);
  }
  return '?';
}
static void drawText(const char *s, int x, int y, uint16_t color, int scale = 1) {
  int cx = x;
  while (*s) {
    uint32_t cp = utf8Next(s);
    int lo = 0, hi = (int)(sizeof(ZH_CODES) / sizeof(ZH_CODES[0])) - 1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (ZH_CODES[m] < cp) lo = m + 1;
      else if (ZH_CODES[m] > cp) hi = m - 1;
      else { lo = m; break; }
    }
    if (lo < (int)(sizeof(ZH_CODES) / sizeof(ZH_CODES[0])) && ZH_CODES[lo] == cp) {
      int cell = scale == 1 ? 32 : 40;
      const uint8_t *glyph = scale == 1 ? ZH_ALPHA_32[lo] : ZH_ALPHA_40[lo];
      for (int py = 0; py < cell; py++) for (int px = 0; px < cell; px++) {
        int xx=cx+px, yy=y+py;
        if ((unsigned)xx>=SCR_W || (unsigned)yy>=SCR_H) continue;
        int bit=py*cell+px;
        uint8_t packed=pgm_read_byte(glyph+bit/2);
        int a=(bit&1)?(packed&15):(packed>>4);
        if (!a) continue;
        a = (a * a + 8) / 15;   // gamma≈2：削掉半透明糊边
        uint16_t bg=diceBig[yy*SCR_W+xx];
        int r=(((color>>11)&31)*a+((bg>>11)&31)*(15-a)+7)/15;
        int g=(((color>>5)&63)*a+((bg>>5)&63)*(15-a)+7)/15;
        int b=((color&31)*a+(bg&31)*(15-a)+7)/15;
        ui.drawPixel(xx,yy,(r<<11)|(g<<5)|b);
      }
    }
    cx += cp >= 32 && cp < 127 ? (scale == 1 ? ASCII_ADV_32[cp-32] : ASCII_ADV_40[cp-32]) : (scale == 1 ? 32 : 40);
  }
}
static void drawCenteredText(const char *s, int y, uint16_t color, int scale = 1) {
  const char *p = s;
  int width = 0;
  while (*p) {
    uint32_t cp = utf8Next(p);
    width += cp >= 32 && cp < 127 ? (scale == 1 ? ASCII_ADV_32[cp - 32] : ASCII_ADV_40[cp - 32]) : (scale == 1 ? 32 : 40);
  }
  drawText(s, max(8, (SCR_W - width) / 2), y, color, scale);
}
static void line(int x, int y, int w, uint16_t color = C_LINE) {
  ui.fillRect(x, y, w, 1, color);
}
static void pill(int x, int y, int w, int h, uint16_t fill, uint16_t border = C_LINE) {
  ui.fillRoundRect(x, y, w, h, h / 3, border);
  ui.fillRoundRect(x + 2, y + 2, w - 4, h - 4, h / 3 - 2, fill);
}
static void drawBack(const char *title) {
  ui.fillRect(0, 0, SCR_W, 92, C_BG);
  ui.fillTriangle(78,45,96,31,96,59,C_WHITE);   // 圆内安全位置
  drawText(title, 112, 33, C_WHITE);
  line(20, 90, SCR_W - 40);
}
static void drawHome() {
  ui.fillScreen(C_BG);
  ui.fillRoundRect(96, 44, 274, 74, 20, C_CARD);
  drawText("Family Hub", 122, 64, C_WHITE);

  pill(60, 150, 346, 120, 0xFE12, 0xFE12);
  ui.fillCircle(112, 210, 32, C_RED);
  ui.fillCircle(105, 203, 5, C_WHITE); ui.fillCircle(121, 219, 5, C_WHITE);
  drawText("Roll dice", 160, 190, C_WHITE, 2);

  pill(60, 284, 346, 110, 0xAF19, 0xAF19);
  ui.fillCircle(112, 339, 32, C_TEAL);
  ui.fillRoundRect(104, 324, 15, 24, 7, C_BG);
  ui.fillRect(111, 348, 3, 10, C_BG);
  drawText("Voice notes", 160, 320, C_WHITE, 2);

  // 设置按钮：必须在圆内且可见
  pill(150, 412, 166, 38, C_CARD, C_LINE);
  drawText("Settings", 176, 418, C_WHITE);
  ui.flush();
}
static inline uint16_t avg565(uint16_t a, uint16_t b) {
  return ((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1);
}
static void drawCachedRollFrame(int idx, int lift = 0) {
  if (!diceAnim || !animCanvas || !diceAnimReady || idx < 0 || idx >= ROLL_FRAMES) {
    // 缓存不可用时退化为实时渲染，避免动画整段空白
    if (diceBig) {
      diceRenderer.buf = diceBig;
      diceRenderer.W = diceRenderer.H = SCR_W;
      diceRenderer.p.focal = 800.0f;
      diceRenderer.render(diceQ, 0);
      blitFull(diceBig);
    }
    return;
  }
  rollmotion::compose(diceAnim, idx, lift, animCanvas, SCR_W);
  blitFull(animCanvas);
}
static void drawDice() {
  if (!diceLow || !diceBig) return;
  uint32_t t0 = millis();
  if (!rolling && diceFaces && diceFaceReady[diceValue - 1]) {
    memcpy(diceBig, diceFaces + (size_t)(diceValue - 1) * SCR_W * SCR_H,
           (size_t)SCR_W * SCR_H * sizeof(uint16_t));
    pill(16, 4, 112, 48, C_CARD);
    drawText("Back", 38, 12, C_WHITE);
    blitFull(diceBig);
    const int *b = diceFaceBox[diceValue - 1];
    oldX0 = max(0, b[0] - 8); oldY0 = max(0, b[1] - 8);
    oldX1 = min(SCR_W - 1, b[2] + 8); oldY1 = min(SCR_H - 1, b[3] + 8);
    char s[60]; snprintf(s, sizeof(s), "[dice] cached static=%lu ms\n", (unsigned long)(millis() - t0));
    safeLog(s);
    return;
  }
  const int res = SCR_W;
  memset(diceBig, 0, (size_t)SCR_W * SCR_H * sizeof(uint16_t));
  diceRenderer.buf = diceBig;
  diceRenderer.W = diceRenderer.H = res;
  diceRenderer.p.focal = 800.0f;
  diceRenderer.render(diceQ, 0);
  uint32_t t1 = millis();
  uint32_t t2 = millis();
  if (!rolling) {
    pill(16, 4, 112, 48, C_CARD);
    drawText("Back", 38, 12, C_WHITE);
  }
  blitFull(diceBig);
  uint32_t t3 = millis();
  if (!rolling) {
    oldX0 = max(0, diceRenderer.bboxX0 - 8);
    oldY0 = max(0, diceRenderer.bboxY0 - 8);
    oldX1 = min(SCR_W - 1, diceRenderer.bboxX1 + 8);
    oldY1 = min(SCR_H - 1, diceRenderer.bboxY1 + 8);
    char s[64]; snprintf(s, sizeof(s), "[dice] static render=%lu push=%lu ms\n",
                         (unsigned long)(t1 - t0), (unsigned long)(t3 - t2));
    safeLog(s);
  }
  if (rolling) { rollRenderMs += t1 - t0; rollScaleMs += t2 - t1; rollPushMs += t3 - t2; }
}
static void drawContactAvatar(int cx, int cy, int who) {
  const uint16_t accent = CONTACTS[who].color;
  const uint16_t hair = who == 0 ? 0x39C7 : 0x3186;
  const uint16_t skin = 0xFE15;
  ui.fillCircle(cx, cy, 42, accent);
  ui.fillRoundRect(cx - 25, cy + 19, 50, 19, 9, C_WHITE);
  if (who == 1) {
    ui.fillRoundRect(cx - 25, cy - 25, 50, 53, 21, hair);
    ui.fillCircle(cx, cy - 4, 21, skin);
    ui.fillRoundRect(cx - 22, cy - 28, 44, 13, 6, hair);
  } else {
    ui.fillCircle(cx, cy - 6, 22, skin);
    ui.fillRoundRect(cx - 22, cy - 27, 44, 13, 6, hair);
  }
  ui.fillCircle(cx - 8, cy - 4, 2, hair);
  ui.fillCircle(cx + 8, cy - 4, 2, hair);
  ui.fillRect(cx - 5, cy + 9, 10, 2, hair);
}
static void drawVoice() {
  ui.fillScreen(C_BG);
  if (voiceState == VOICE_RECORDING) {
    ui.fillScreen(C_RED);
    drawCenteredText("Recording", 92, C_WHITE, 2);
    ui.fillCircle(240, 252, 60, C_WHITE);
    ui.fillRoundRect(230, 214, 20, 55, 10, C_RED);
    ui.fillRect(237, 270, 6, 22, C_RED);
    drawCenteredText("0 sec", 336, C_WHITE);
    drawCenteredText("Release to send", 398, C_WHITE);
    ui.flush(); return;
  }
  int sender = messageCount ? messages[0].sender : 0;
  drawBack("Family");
  if (messageCount) {
    pill(52, 96, 362, 108, C_CARD, CONTACTS[sender].color);
    drawContactAvatar(104, 150, sender);
    char duration[32];
    snprintf(duration, sizeof(duration), "%s %u sec", CONTACTS[sender].name,
             messages[0].seconds);
    drawText(duration, 168, 112, C_WHITE);
    drawText(voiceState == VOICE_PLAYING ? "Playing" : "Tap to replay", 168, 156,
             C_MUTED);
    ui.fillTriangle(360, 134, 360, 166, 384, 150, C_TEAL);
  }
  const int top = messageCount ? 216 : 104;
  const int height = 430 - top, center = top + height * 2 / 5;
  ui.fillRoundRect(52, top, 362, height, 28, C_TEAL);
  ui.fillCircle(233, center, 42, C_WHITE);
  ui.fillRoundRect(224, center - 26, 18, 40, 9, C_TEAL);
  ui.fillRect(230, center + 10, 6, 17, C_TEAL);
  drawCenteredText(voiceState == VOICE_SENDING ? "Sending" : "Hold to talk",
                   top + height * 3 / 4, C_WHITE, 2);
  if (bannerUntil > millis() && banner[0]) {
    pill(120, 392, 240, 40, C_CARD);
    drawText(banner, 140, 400, C_WHITE);
  }
  ui.flush();
}
static void drawVoiceResult(bool ok) {
  ui.fillScreen(ok ? C_TEAL : C_RED);
  if (ok) {
    ui.fillTriangle(120, 254, 146, 228, 218, 301, C_WHITE);
    ui.fillTriangle(146, 228, 244, 326, 218, 301, C_WHITE);
    ui.fillTriangle(218, 301, 244, 326, 364, 178, C_WHITE);
    ui.fillTriangle(218, 301, 338, 152, 364, 178, C_WHITE);
  } else {
    drawCenteredText("Try again", 216, C_WHITE, 2);
  }
  ui.flush();
}
static void drawSlider(int y, const char *name, int value, uint16_t color) {
  drawText(name, 38, y, C_WHITE);
  char v[10]; snprintf(v, sizeof(v), "%d%%", value);
  drawText(v, 365, y, C_MUTED);
  pill(38, y + 43, 404, 24, C_CARD);
  int width = value * 380 / 100;
  if (width > 0) ui.fillRoundRect(50, y + 49, width, 12, 6, color);
  ui.fillCircle(50 + width, y + 55, 14, C_WHITE);
}
static void drawSettings() {
  ui.fillScreen(C_BG); drawBack("Settings");
  drawSlider(105, "Brightness", brightness * 100 / 255, C_GOLD);
  drawSlider(217, "Volume", volume, C_TEAL);
  pill(38, 340, 404, 60, C_CARD);
  drawText("Reconnect Wi-Fi", 55, 357, C_WHITE); drawText(">", 412, 357, C_MUTED);
  drawText(WiFi.status() == WL_CONNECTED ? "Connected" : "Offline", 38, 420, C_MUTED);

  ui.flush();
}
static void drawLocked() {
  ui.fillScreen(0x0000);
}
static void redraw() {
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
    voiceListening = false;
    inboxCheckRequested = true;
  }
  page = p;
  if (p == PAGE_VOICE) lastListAt = 0;
  redraw();
}

void IRAM_ATTR onTouchISR() { touchPending = true; }
static bool touchPoint(int16_t &x, int16_t &y) {
  if (!touchOk) return false;
  int16_t px = 0, py = 0;
  if (readTouch(&px, &py) == 0) return false;
  x = px; y = py; return true;
}
static void savePrefs() {
  prefs.putUChar("bright", brightness);
  prefs.putUChar("volume", volume);
}
static void applyVolume() {
  // 只走 M5Unified 官方 API，不手写 codec 寄存器
  M5.Speaker.setVolume(volume);
}
static bool initAudio() {
  audioOk = true;
  codecOk = M5.Speaker.isEnabled();
  if (M5.Mic.isEnabled()) M5.Mic.begin();
  applyVolume();
  safeLog(codecOk ? "[audio] M5 speaker ready\n" : "[audio] speaker missing\n");
  return audioOk;
}
static void wavHeader(uint8_t *h, uint32_t bytes) {
  memcpy(h, "RIFF", 4); uint32_t v32 = 36 + bytes; memcpy(h + 4, &v32, 4);
  memcpy(h + 8, "WAVEfmt ", 8); v32 = 16; memcpy(h + 16, &v32, 4);
  uint16_t v16 = 1; memcpy(h + 20, &v16, 2); memcpy(h + 22, &v16, 2);
  v32 = SAMPLE_RATE; memcpy(h + 24, &v32, 4); v32 = SAMPLE_RATE * 2; memcpy(h + 28, &v32, 4);
  v16 = 2; memcpy(h + 32, &v16, 2); v16 = 16; memcpy(h + 34, &v16, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4);
}
static size_t recordAudio() {
  if (!audioOk || !pcm) return 0;
  banner[0] = 0;
  voiceState = VOICE_RECORDING; drawVoice();
  if (!M5.Mic.begin()) { safeLog("[rec] mic begin failed\n"); return 0; }
  // 不改 ES8311 寄存器：M5Unified 的 mic 回调已经在 I2S 时钟启动后正确 arm
  // (0x0D=0x01)，手改 0x14/0x17 会把 ADC 通路配坏成"嗡"声。
  uint32_t start = millis(), lastDraw = start, lastFingerAt = start;
  size_t written = 0;
  const size_t chunk = 1024;  // 64ms @ 16k
  while (millis() - start < MAX_REC_MS && written + chunk <= pcmCap) {
    if (!M5.Mic.record(pcm + written, chunk, SAMPLE_RATE)) break;
    written += chunk;
    uint32_t now = millis();
    int16_t heldX = 0, heldY = 0;
    if (readTouch(&heldX, &heldY) > 0) lastFingerAt = now;
    if (now - start > MIN_REC_MS && now - lastFingerAt > 220) break;
    if (now - lastDraw > 250) {
      lastDraw = now;
      char t[18]; snprintf(t, sizeof(t), "%u sec", (unsigned)((now - start) / 1000));
      ui.fillRect(170, 339, 140, 38, C_RED); drawCenteredText(t, 344, C_WHITE);
      ui.flush();
    }
  }
  M5.Mic.end();
  // 裁掉开头静音（M5.Mic.begin 启动延迟 ~1s），保留 80ms 预卷
  size_t first = 0;
  while (first < written && abs(pcm[first]) < 500) first++;
  size_t preroll = SAMPLE_RATE * 80 / 1000;
  size_t trim = first > preroll ? first - preroll : 0;
  if (trim > 0 && trim < written) {
    memmove(pcm, pcm + trim, (written - trim) * sizeof(int16_t));
    written -= trim;
  }
  // 软件 AGC：把峰值归一到 -3 dBFS，同时避免放大底噪（上限 12 倍）
  int32_t peak = 0;
  for (size_t i = 0; i < written; i++) peak = max(peak, (int32_t)abs(pcm[i]));
  float gain = 1.0f;
  if (peak > 200) {
    gain = (32768.0f * 0.707f) / (float)peak;
    if (gain > 12.0f) gain = 12.0f;
    if (gain < 0.5f) gain = 0.5f;
    for (size_t i = 0; i < written; i++) {
      int32_t v = (int32_t)(pcm[i] * gain);
      pcm[i] = (int16_t)constrain(v, -32768, 32767);
    }
  }
  char rep2[96]; snprintf(rep2, sizeof(rep2), "[rec] %u samples peak=%d gain=%.2f\n",
                          (unsigned)written, (int)peak, (double)gain);
  safeLog(rep2);
  return written;
}
static bool uploadAudio(size_t count) {
  if (WiFi.status() != WL_CONNECTED) return false;
  size_t total = 44 + count * 2;
  uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  if (!body) return false;
  wavHeader(body, count * 2); memcpy(body + 44, pcm, count * 2);
  char url[200]; snprintf(url, sizeof(url), "%s/voice?from=%s&to=%s", serverBase,
                          SEND_CONTACT, "dad");
  WiFiClient client; HTTPClient http;
  bool ok = false;
  if (http.begin(client, url)) {
    http.setConnectTimeout(4000);
    http.setTimeout(12000); http.addHeader("Content-Type", "audio/wav");
    if (*INBOX_SECRET) http.addHeader("X-Voice-Secret", INBOX_SECRET);
    int status = http.POST(body, total);
    ok = status == 200;
    char report[64]; snprintf(report, sizeof(report), "[upload] status=%d bytes=%u\n",
                              status, (unsigned)total);
    safeLog(report);
    http.end();
  }
  free(body); return ok;
}
static void fetchMessages() {
  lastListAt = millis(); messageCount = 0;
  if (WiFi.status() != WL_CONNECTED) { drawVoice(); return; }
  inboxCheckRequested = false;
  char url[184]; snprintf(url, sizeof(url), "%s/api/list?direction=in&limit=3", serverBase);
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, url)) return;
  http.setReuse(false);
  http.setTimeout(4000);
  if (http.GET() == 200 && http.getSize() < 65536) {
    String body = http.getString();
    cJSON *root = cJSON_Parse(body.c_str());
    cJSON *items = root ? cJSON_GetObjectItem(root, "items") : nullptr;
    if (cJSON_IsArray(items)) {
      cJSON *item = nullptr;
      cJSON_ArrayForEach(item, items) {
        cJSON *c = cJSON_GetObjectItem(item, "contact");
        if (!cJSON_IsString(c)) continue;
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id)) continue;
        cJSON *direction = cJSON_GetObjectItem(item, "direction");
        if (!cJSON_IsString(direction) || strcmp(direction->valuestring, "in")) continue;
        Message &m = messages[messageCount];
        strlcpy(m.id, id->valuestring, sizeof(m.id));
        cJSON *seconds = cJSON_GetObjectItem(item, "seconds");
        m.seconds = cJSON_IsNumber(seconds) ? (uint16_t)ceil(seconds->valuedouble) : 0;
        m.incoming = true; m.sender = !strcmp(c->valuestring,"mom") ? 1 : 0;
        cJSON *ts = cJSON_GetObjectItem(item, "ts");
        m.ts = cJSON_IsNumber(ts) ? (uint32_t)(ts->valuedouble / 1000) : 0;
        if (++messageCount == MAX_MESSAGES) break;
      }
    }
    cJSON_Delete(root);
  }
  http.end(); if (page == PAGE_VOICE) drawVoice();

  // Showing a message in the family list must also play it. pollIncoming() owns
  // playback but bails out while a roll, a sound, or a recording is running, so
  // relying on it alone leaves fresh messages visible but silent. Play here and
  // only advance the high-water mark once the samples really went out.
  if (messageCount && page == PAGE_VOICE && voiceState == VOICE_READY && !rolling && !diceSoundActive) {
    uint64_t cursor = prefs.getULong64("inbox-ts", 0);
    uint64_t newest = (uint64_t)messages[0].ts * 1000ULL;
    if (newest > cursor) {
      safeLog("[voice] newer message than cursor, playing\n");
      if (playMessage(0)) prefs.putULong64("inbox-ts", newest);
    }
  }
}
static bool playMessage(int idx) {
  if (!codecOk || idx < 0 || idx >= messageCount || WiFi.status() != WL_CONNECTED) return false;
  char url[190]; snprintf(url, sizeof(url), "%s/audio/%s", serverBase, messages[idx].id);
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(6000);
  bool ok = false;
  size_t played = 0;
  if (http.GET() == 200) {
    int len = http.getSize();
    if (len > 44 && len <= 2 * 1024 * 1024) {
      uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
      if (buf) {
        WiFiClient *stream = http.getStreamPtr();
        size_t got = stream->readBytes(buf, len);
        if (got == (size_t)len && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
          uint32_t rate = 0; uint16_t ch = 0, bits = 0;
          memcpy(&rate, buf + 24, 4);
          memcpy(&ch, buf + 22, 2);
          memcpy(&bits, buf + 34, 2);
          if (rate >= 8000 && rate <= 48000 && ch == 1 && bits == 16) {
            voiceState = VOICE_PLAYING; drawVoice();
            // 交给库解析 WAV 头，避免手算采样率/声道出错（"嗡"声来源之一）
            M5.Speaker.playWav(buf, len);
            delay(30);
            uint32_t t0 = millis();
            while (M5.Speaker.isPlaying() && millis() - t0 < 25000) {
              int16_t sx = 0, sy = 0;
              if (readTouch(&sx, &sy) > 0 && sy < 65) { M5.Speaker.stop(); break; }
              delay(20);
            }
            played = len - 44;
            ok = true;
          }
        }
        free(buf);
      }
    }
  }
  http.end();
  voiceState = VOICE_READY;
  char report[80]; snprintf(report, sizeof(report), "[play] ok=%d pcm=%u\n", ok, (unsigned)played);
  safeLog(report);
  return ok;
}

static void wakeScreen() {
  if (!screenLocked) return;
  screenLocked = false; M5.Display.setBrightness(brightness);
  touchPending = false;
}
static void lockScreen() {
  if (rolling) { diceValue=rollToValue; diceQ=diceTargetQ; rolling=false; }
  diceSoundCancel=true;
  screenLocked=true; lockReleaseArmed=false;
  drawLocked(); ui.flush(); M5.Display.setBrightness(0);
  touchPending=false;
}
static void pollIncoming() {
  if (WiFi.status() != WL_CONNECTED || rolling || diceSoundActive || voiceState != VOICE_READY || (page == PAGE_SETTINGS && !screenLocked)) return;
  inboxCheckRequested = false;
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, String(serverBase) + "/api/list?direction=in&limit=5" +
                   (strlen(LISTEN_CONTACT) ? String("&contact=") + LISTEN_CONTACT : ""))) return;
  http.setReuse(true);
  http.setConnectTimeout(1200); http.setTimeout(1500);
  if (http.GET()!=200) { http.end(); return; }
  String body=http.getString(); http.end();
  cJSON *root=cJSON_Parse(body.c_str());
  cJSON *items=root?cJSON_GetObjectItem(root,"items"):nullptr;
  uint64_t cursor = prefs.getULong64("inbox-ts",0);
  cJSON *next=nullptr; uint64_t nextTs=UINT64_MAX;
  if(cJSON_IsArray(items)) {
    cJSON *item=nullptr;
    cJSON_ArrayForEach(item,items) {
      cJSON *ts=cJSON_GetObjectItem(item,"ts");
      if(cJSON_IsNumber(ts) && uint64_t(ts->valuedouble)>cursor && uint64_t(ts->valuedouble)<nextTs) {
        next=item; nextTs=uint64_t(ts->valuedouble);
      }
    }
  }
  if(next) {
    cJSON *id=cJSON_GetObjectItem(next,"id"),*who=cJSON_GetObjectItem(next,"contact"),*seconds=cJSON_GetObjectItem(next,"seconds");
    if(cJSON_IsString(id)) {
      Message pending={}; strlcpy(pending.id,id->valuestring,sizeof(pending.id));
      pending.sender=cJSON_IsString(who)&&!strcmp(who->valuestring,"mom")?1:0;
      pending.seconds=cJSON_IsNumber(seconds)?uint16_t(ceil(seconds->valuedouble)):0;
      pending.incoming=true;
      wakeScreen(); page=PAGE_VOICE; voiceContactChosen=true;
      messages[0]=pending; messageCount=1;
      safeLog("[inbox] new family voice\n");
      if(playMessage(0)) { prefs.putULong64("inbox-ts",nextTs); }
      else {
        // Leave the cursor behind so the next poll retries, but say why: a silent
        // failure here looked identical to "the board never noticed".
        char r[96]; snprintf(r,sizeof(r),"[inbox] play failed, will retry (codec=%d wifi=%d)\n",codecOk,WiFi.status()==WL_CONNECTED);
        safeLog(r);
      }
    }
  }
  cJSON_Delete(root);
}
static void wifiPortal(bool force) {
  ui.fillScreen(C_BG); drawBack("Wi-Fi setup");
  drawText("Connect phone to", 58, 140, C_WHITE);
  drawText("FamilyHub-Setup", 58, 190, C_TEAL);
  drawText("Open 192.168.4.1", 58, 245, C_MUTED);
  ui.flush();
  WiFiManager wm; wm.setConfigPortalTimeout(180);
  WiFiManagerParameter serverParam("srv", "Server URL", serverBase, 110);
  wm.addParameter(&serverParam);
  bool ok = force ? wm.startConfigPortal("FamilyHub-Setup") : wm.autoConnect("FamilyHub-Setup");
  if (ok) {
    const char *s = serverParam.getValue();
    if (s && !strncmp(s, "http://", 7)) {
      strlcpy(serverBase, s, sizeof(serverBase)); prefs.putString("server", serverBase);
    }
    notice("Connected");
  } else notice("Connection failed");
  redraw();
}
static void diceSoundTask(void *) {
  // 直接合成一段骰子声，交给 M5.Speaker 播放
  const int total = SAMPLE_RATE * 9 / 10;
  int16_t *buf = (int16_t *)heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (buf) {
    uint32_t noise = esp_random();
    float filtered = 0;
    for (int i = 0; i < total; i++) {
      float t = i / (float)SAMPLE_RATE;
      float rattle = 0;
      const float hits[] = {0.02f, 0.17f, 0.35f, 0.55f, 0.70f};
      for (int h = 0; h < 5; h++) {
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
          ? sinf(2.0f * PI * (150.0f - 200.0f * landAge) * landAge) *
            (1.0f - landAge / 0.17f) : 0;
      buf[i] = (int16_t)(filtered * rattle * 6500 + thud * 5000);
    }
    M5.Speaker.playRaw(buf, total, SAMPLE_RATE, false, 1, 0);
    while (M5.Speaker.isPlaying() && !diceSoundCancel) delay(20);
    if (diceSoundCancel) M5.Speaker.stop();
    free(buf);
  }
  diceSoundActive = false;
  vTaskDelete(nullptr);
}
static void diceCacheTask(void *) {
  dice::Renderer renderer;
  renderer.W = renderer.H = SCR_W;
  renderer.p.focal = 800.0f;
  for (int i = 0; i < 6; i++) {
    renderer.buf = diceFaces + (size_t)i * SCR_W * SCR_H;
    memset(renderer.buf, 0, (size_t)SCR_W * SCR_H * sizeof(uint16_t));
    renderer.render(dice::targetForValue(i + 1, 0.1f), 0);
    diceFaceBox[i][0] = renderer.bboxX0; diceFaceBox[i][1] = renderer.bboxY0;
    diceFaceBox[i][2] = renderer.bboxX1; diceFaceBox[i][3] = renderer.bboxY1;
    diceFaceReady[i] = true;
    vTaskDelay(1);
  }
  safeLog("[dice] six faces cached\n");
  if (!diceAnim) { vTaskDelete(nullptr); return; }
  diceAnimReady = false;
  renderer.W = renderer.H = ROLL_RES;
  renderer.p.focal = 800.0f;
  for (int frame = 0; frame < ROLL_FRAMES; frame++) {
    renderer.buf = diceAnim + (size_t)frame * ROLL_RES * ROLL_RES;
    memset(renderer.buf, 0, (size_t)ROLL_RES * ROLL_RES * sizeof(uint16_t));
    renderer.render(rollmotion::pose(frame), 0);
    vTaskDelay(1);
  }
  diceAnimReady = true;
  safeLog("[dice] throw sequence ready\n");
  vTaskDelete(nullptr);
}
static void rollDice() {
  if (rolling) return;
  if (!diceAnimReady) { safeLog("[dice] cache preparing\n"); return; }
  rollFromValue = diceValue;
  rollToValue = 1 + (esp_random() % 6);
  diceTargetQ = dice::targetForValue(rollToValue, 0.1f);
  rolling = true; rollAt = millis(); rollFrames = 1;
  rollCacheNextAt = rollAt + 80;
  // The first transfer already changes physical pose. No digit swaps or rendering here.
  drawCachedRollFrame(rollmotion::frameAt(rollFromValue, rollToValue, 0.045f), 10);
  if (codecOk && !diceSoundActive) {
    diceSoundCancel = false; diceSoundActive = true;
    if (xTaskCreatePinnedToCore(diceSoundTask, "dice-sound", 4096, nullptr, 1, nullptr, 0) != pdPASS)
      diceSoundActive = false;
  }
}
static void updateDice() {
  if (page != PAGE_DICE || !rolling || millis() < rollCacheNextAt) return;
  float u = fminf(1.0f, float(millis() - rollAt) / ROLL_MS);
  if (u >= 1) {
    diceValue = rollToValue; diceQ = diceTargetQ; rolling = false;
    drawDice();
    char report[100]; snprintf(report, sizeof(report), "[dice] physical throw %u frames %lu ms result=%d\n",
      rollFrames, (unsigned long)(millis()-rollAt), diceValue);
    safeLog(report); return;
  }
  drawCachedRollFrame(rollmotion::frameAt(rollFromValue,rollToValue,u), rollmotion::liftAt(u));
  rollFrames++;
  rollCacheNextAt = rollAt + (uint32_t)(u * ROLL_MS / 80 + 1) * 80;
}
static void handleTap(int x, int y) {
  if (millis() - lastTouchAt < 260) return;
  lastTouchAt = millis();
  if (page == PAGE_HOME) {
    if (y >= 150 && y <= 270) setPage(PAGE_DICE);
    else if (y >= 284 && y <= 394) { setPage(PAGE_VOICE); fetchMessages(); }
    else if (y >= 400) { settingsReturn = PAGE_HOME; setPage(PAGE_SETTINGS); }
  } else if (page == PAGE_DICE) {
    if (x < 210 && y < 96) setPage(PAGE_HOME); else rollDice();
  } else if (page == PAGE_VOICE) {
    if (x < 210 && y < 96) {
      setPage(PAGE_HOME);
    } else if (messageCount && y >= 96 && y < 204) {
      if (messageCount && !playMessage(0)) notice("Playback failed");
    } else if (y >= (messageCount ? 216 : 104)) {
      size_t n = recordAudio();
      bool sent = false;
      if (n >= SAMPLE_RATE * MIN_REC_MS / 1000) {
        voiceState = VOICE_SENDING; drawVoice();
        sent = uploadAudio(n);
      }
      banner[0] = 0;
      drawVoiceResult(sent); delay(800);
      voiceState = VOICE_READY; drawVoice();
    }
  } else if (page == PAGE_SETTINGS) {
    if (x < 210 && y < 96) setPage(settingsReturn);
    else if (y >= 145 && y <= 192) {
      brightness = constrain((x - 50) * 255 / 380, 12, 255);
      M5.Display.setBrightness(brightness);
 savePrefs(); drawSettings();
    } else if (y >= 257 && y <= 304) {
      volume = constrain((x - 50) * 100 / 380, 0, 100);
      applyVolume(); savePrefs(); drawSettings();
    } else if (y >= 340 && y <= 400) wifiPortal(true);
  }
}

void setup() {
  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  USBSerial.begin(115200); delay(50);
  Serial.printf("[hub] role=%s send=%s listen=%s\n", DEVICE_ROLE, SEND_CONTACT, LISTEN_CONTACT);
  prefs.begin("family-hub", false);
  brightness = prefs.getUChar("bright", 90); volume = prefs.getUChar("volume", 230);
  String saved = prefs.getString("server", DEFAULT_SERVER);
  strlcpy(serverBase, saved.c_str(), sizeof(serverBase));
  pinMode(KYB_PIN, INPUT_PULLUP); pinMode(KYA_PIN, INPUT_PULLUP);
  touchOk = true;   // M5.begin() 已初始化 CST820B
  imuOk = true;     // BMI270 已由 M5Unified 初始化
  M5.Display.setBrightness(brightness);
  diceLow = (uint16_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
  diceBig = (uint16_t *)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
  animCanvas = (uint16_t *)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
  pcm = (int16_t *)heap_caps_malloc(pcmCap * 2, MALLOC_CAP_SPIRAM);
  // 6 面整屏缓存要 2.5MB；PSRAM 紧张时宁可不缓存（每帧现算）也不能崩
  diceFaces = (uint16_t *)heap_caps_malloc((size_t)6 * SCR_W * SCR_H * 2,
                                           MALLOC_CAP_SPIRAM);
  diceAnim = (uint16_t *)heap_caps_malloc(
      (size_t)ROLL_FRAMES * ROLL_RES * ROLL_RES * 2, MALLOC_CAP_SPIRAM);
  diceCacheFallback = (diceFaces == nullptr);
  if (!diceBig || !pcm) {
    safeLog("PSRAM critical allocation failed\n");
    while (true) delay(1000);
  }
  char memrep[96];
  snprintf(memrep, sizeof(memrep), "[mem] free=%uKB biggest=%uKB faces=%d\n",
           (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
           (int)!diceCacheFallback);
  safeLog(memrep);
#if defined(SW_SELFTEST)
  Wire.begin(47, 48, 400000);
  char i2c[96] = "";
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char b[8]; snprintf(b, sizeof(b), " %02X", a);
      strncat(i2c, b, sizeof(i2c) - strlen(i2c) - 1);
    }
  }
  // 静止画面：只画一次，避免整屏刷新造成闪烁
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(40, 90);
  M5.Display.printf("board %d", (int)M5.getBoard());
  M5.Display.setCursor(40, 130);
  M5.Display.printf("disp %dx%d", M5.Display.width(), M5.Display.height());
  M5.Display.setCursor(40, 170);
  M5.Display.printf("psram %uM", (unsigned)(ESP.getPsramSize() / 1024 / 1024));
  M5.Display.setCursor(30, 210);
  M5.Display.printf("i2c%s", i2c);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(40, 300);
  M5.Display.printf("raw -,-");
  int16_t lastX = -1, lastY = -1;
  for (;;) {
    M5.update();
    auto detail = M5.Touch.getDetail();
    int16_t rx = detail.isPressed() ? (int16_t)detail.x : -1;
    int16_t ry = detail.isPressed() ? (int16_t)detail.y : -1;
    if (rx != lastX || ry != lastY) {
      lastX = rx; lastY = ry;
      M5.Display.fillRect(30, 296, 400, 28, TFT_BLACK);
      M5.Display.setCursor(40, 300);
      if (rx >= 0) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.printf("raw %d,%d -> %d,%d", rx, ry, rx * 2, ry * 2);
      } else {
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.printf("raw -,- touch me");
      }
    }
    delay(20);
  }
#endif
  diceQ = dice::targetForValue(1, 0.1f);
  initAudio();
  drawHome();
  if (diceFaces) xTaskCreatePinnedToCore(diceCacheTask, "dice-cache", 6144, nullptr, 1, nullptr, 0);
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(100);
  if (WiFi.status() != WL_CONNECTED) wifiPortal(false);
  if (WiFi.status() == WL_CONNECTED && !prefs.isKey("inbox-ts")) {
    WiFiClient client; HTTPClient http;
    if(http.begin(client,String(serverBase)+"/api/list?direction=in&limit=1")) {
      http.setTimeout(2500);
      if(http.GET()==200) {
        cJSON *root=cJSON_Parse(http.getString().c_str());
        cJSON *items=root?cJSON_GetObjectItem(root,"items"):nullptr;
        cJSON *first=cJSON_IsArray(items)?cJSON_GetArrayItem(items,0):nullptr;
        cJSON *ts=first?cJSON_GetObjectItem(first,"ts"):nullptr;
        prefs.putULong64("inbox-ts",cJSON_IsNumber(ts)?uint64_t(ts->valuedouble):0);
        cJSON_Delete(root);
      }
      http.end();
    }
  }
  safeLog("family-hub ready\n");
}

void loop() {
  if (screenLocked) {
    bool released = digitalRead(KYB_PIN)==HIGH && digitalRead(KYA_PIN)==HIGH;
    if (released) lockReleaseArmed = true;
    if (lockReleaseArmed && !released) { wakeScreen(); redraw(); delay(200); }
    if (millis()-lastInboxPollAt>700) { lastInboxPollAt=millis(); pollIncoming(); }
    delay(20); return;
  }
  if (USBSerial.available()) {
    char cmd = USBSerial.read();
    if (cmd == 'r') { setPage(PAGE_DICE); rollDice(); }
    else if (cmd == 'h') setPage(PAGE_HOME);
    else if (cmd == 'v') {
      setPage(PAGE_VOICE); fetchMessages();
      char s[50]; snprintf(s, sizeof(s), "[voice] %d messages for %s\n",
                            messageCount, CONTACTS[contact].key);
      safeLog(s);
    }
    else if (cmd == 'p' && page == PAGE_VOICE) playMessage(0);
    else if (cmd == 'c') inboxCheckRequested = true;
    else if (cmd == 'f') {
      USBSerial.print("FRAME480\n");
      USBSerial.write((uint8_t *)diceBig, SCR_W * SCR_H * 2);
    }
    else if (cmd == 'g') setPage(PAGE_SETTINGS);
    else if (cmd == 'l') {
      lockScreen();
    }
    else if (cmd == 's') {
      char s[160]; snprintf(s, sizeof(s), "[status] touch=%d imu=%d audio=%d codec=%d wifi=%d cache=%d psram=%u mac=%s\n",
                            touchOk, imuOk, audioOk, codecOk, WiFi.status() == WL_CONNECTED, diceAnimReady, (unsigned)ESP.getFreePsram(), WiFi.macAddress().c_str());
      safeLog(s);
    }
  }
  if (screenLocked) return;
  int16_t x, y;
  if (touchPoint(x, y)) handleTap(x, y);
  static int lastBlue = HIGH, lastYellow = HIGH;
  static uint32_t blueAt = 0, yellowAt = 0, lastShake = 0;
  const int blue = digitalRead(KYB_PIN), yellow = digitalRead(KYA_PIN);
  const uint32_t nowMs = millis();
  // 蓝键 KEYB：短按切下一页，长按 0.8s 熄屏/唤醒
  if (blue == LOW && lastBlue == HIGH) blueAt = nowMs;
  if (blue == HIGH && lastBlue == LOW) {
    if (nowMs - blueAt > 800) { screenLocked ? wakeScreen() : lockScreen(); }
    else setPage((Page)((page + 1) % 4));
  }
  // 黄键 KEYA：短按回首页，长按 0.8s 熄屏/唤醒
  if (yellow == LOW && lastYellow == HIGH) yellowAt = nowMs;
  if (yellow == HIGH && lastYellow == LOW) {
    if (nowMs - yellowAt > 800) { screenLocked ? wakeScreen() : lockScreen(); }
    else { setPage(PAGE_HOME); }
  }
  lastBlue = blue; lastYellow = yellow;
  if (page == PAGE_DICE && imuOk && millis() - lastShake > 20) {
    lastShake = millis();
    if (M5.Imu.getAccel(&acc[0], &acc[1], &acc[2])) {
      float mag = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
      shakeScore = fabsf(mag - 1.0f) > 0.55f ? shakeScore + 1 : shakeScore * 0.75f;
      if (shakeScore > 4) { shakeScore = 0; rollDice(); }
    }
  }
  updateDice();
  // 触摸进行中不轮询，避免 HTTP 阻塞吞掉点击；空闲时 1.2s 一次
  const bool touching = M5.Touch.getDetail().isPressed();
  if (!touching && (inboxCheckRequested || millis() - lastInboxPollAt > 1200)) {
    lastInboxPollAt = millis();
    pollIncoming();
  }
  if (page == PAGE_VOICE && voiceContactChosen && millis() - lastListAt > 15000 && voiceState == VOICE_READY)
    fetchMessages();
  if (page == PAGE_VOICE && banner[0] && millis() > bannerUntil) {
    banner[0] = 0; drawVoice();
  }
  delay(10);
}

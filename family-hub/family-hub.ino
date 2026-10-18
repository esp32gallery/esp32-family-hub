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
#include <Arduino_GFX_Library.h>
#include <font/glcdfont.h>
#include <TouchDrvCSTXXX.hpp>
#include <SensorQMI8658.hpp>
#include <HWCDC.h>
extern "C" {
#include <cJSON.h>
#include "es8311.h"
}
#include "es7210.h"
#include "dice_sdf.h"
#include "zh_font.h"

#define WIFI_SSID "ASUS"
#define WIFI_PASS "123qweasd"
#define DEFAULT_SERVER "http://192.168.31.114:8896"
#define INBOX_SECRET ""
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_RESET 39
#define LCD_CS 12
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RST 40
#define KEY_PIN 18
#define BOOT_PIN 0
#define PA_PIN 46
#define I2S_BCLK 9
#define I2S_LRCK 45
#define I2S_DIN 10
#define I2S_DOUT 8
#define I2S_MCLK 42
#define SCR_W 480
#define SCR_H 480
#define ROLL_MS 680
#define ROLL_FRAMES 10
#define ROLL_RES 160
#define SAMPLE_RATE 16000
#define MAX_REC_MS 8000
#define MIN_REC_MS 400
#define MAX_MESSAGES 3

static const uint16_t C_BG = 0x0843, C_CARD = 0x18C6, C_LINE = 0x294A;
static const uint16_t C_WHITE = 0xFFDF, C_MUTED = 0x9CF3;
static const uint16_t C_TEAL = 0x35D5, C_PINK = 0xFB4F, C_GOLD = 0xFD27;
static const uint16_t C_RED = 0xE2A8, C_OUT = 0x2B69;

struct Contact { const char *key; const char *name; uint16_t color; };
static const Contact CONTACTS[] = {
  {"dad", "爸爸", C_GOLD}, {"mom", "妈妈", C_RED}, {"me", "我自己", C_TEAL}
};
struct Message { char id[48]; uint32_t ts; uint16_t seconds; bool incoming; };
enum Page { PAGE_HOME, PAGE_DICE, PAGE_VOICE, PAGE_SETTINGS };
enum VoiceState { VOICE_READY, VOICE_RECORDING, VOICE_SENDING, VOICE_PLAYING, VOICE_ERROR };

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, SCR_W, SCR_H, 0, 0, 0, 0);
HWCDC USBSerial;
static TouchDrvCST92xx touch;
static SensorQMI8658 imu;
static IMUdata acc;
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
static char lastIncomingId[2][48] = {};
static bool inboxPrimed[2] = {};
static uint32_t lastInboxPollAt = 0;
static volatile bool inboxCheckRequested = true;
static bool screenLocked = false;
static int16_t *pcm = nullptr;
static size_t pcmCap = SAMPLE_RATE * MAX_REC_MS / 1000;
static es8311_handle_t codec = nullptr;
static dice::Renderer diceRenderer;
static uint16_t *diceLow = nullptr, *diceBig = nullptr;
static uint16_t *diceFaces = nullptr;
static volatile bool diceFaceReady[6] = {};
static int diceFaceBox[6][4] = {};
static uint16_t *diceAnim = nullptr;
static uint16_t *animCanvas = nullptr;
static int diceAnimBox[6][ROLL_FRAMES][4] = {};
static volatile bool diceAnimReady[6] = {};
static bool rollUsesCache = false;
static uint8_t rollCacheFrame = 0;
static uint32_t rollCacheNextAt = 0;
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
  void flush() { gfx->draw16bitRGBBitmap(0, 0, diceBig, SCR_W, SCR_H); }
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
    if (cp < 128) {
      int size = scale == 1 ? 2 : 3;
      for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font[cp * 5 + col]);
        for (int row = 0; row < 7; row++) if (bits & (1 << row))
          ui.fillRect(cx + col * size, y + 4 * scale + row * size,
                      size, size, color);
      }
      cx += 6 * size; continue;
    }
    int lo = 0, hi = (int)(sizeof(ZH_CODES) / sizeof(ZH_CODES[0])) - 1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (ZH_CODES[m] < cp) lo = m + 1;
      else if (ZH_CODES[m] > cp) hi = m - 1;
      else { lo = m; break; }
    }
    if (lo < (int)(sizeof(ZH_CODES) / sizeof(ZH_CODES[0])) && ZH_CODES[lo] == cp) {
      for (int py = 0; py < 24; py++) for (int px = 0; px < 24; px++) {
        if (pgm_read_byte(&ZH_BITS[lo][py * 3 + px / 8]) & (0x80 >> (px & 7))) {
          if (scale == 1) ui.drawPixel(cx + px, y + py, color);
          else ui.fillRect(cx + px * scale, y + py * scale, scale, scale, color);
        }
      }
    }
    cx += 24 * scale;
  }
}
static void line(int x, int y, int w, uint16_t color = C_LINE) {
  ui.fillRect(x, y, w, 1, color);
}
static void pill(int x, int y, int w, int h, uint16_t fill, uint16_t border = C_LINE) {
  ui.fillRoundRect(x, y, w, h, h / 3, border);
  ui.fillRoundRect(x + 2, y + 2, w - 4, h - 4, h / 3 - 2, fill);
}
static void drawBack(const char *title) {
  ui.fillRect(0, 0, SCR_W, 65, C_BG);
  drawText("<", 22, 20, C_WHITE);
  drawText(title, 74, 19, C_WHITE);
  line(0, 64, SCR_W);
}
static void drawHome() {
  ui.fillScreen(C_BG);
  ui.fillRoundRect(28, 25, 424, 89, 24, C_CARD);
  drawText("家庭小站", 54, 45, C_WHITE);
  drawText("DICE + VOICE", 54, 82, C_MUTED);
  pill(28, 136, 424, 132, 0x3908, 0x792C);
  ui.fillCircle(87, 202, 37, C_RED);
  ui.fillCircle(78, 194, 6, C_WHITE); ui.fillCircle(98, 212, 6, C_WHITE);
  drawText("骰子", 158, 166, C_WHITE, 2);
  drawText("轻触或摇一摇", 160, 228, C_MUTED);
  pill(28, 286, 424, 132, 0x1268, 0x2C73);
  ui.fillCircle(87, 352, 37, C_TEAL);
  ui.fillRoundRect(78, 334, 18, 28, 8, C_BG);
  ui.fillRect(86, 362, 3, 12, C_BG);
  drawText("语音信箱", 158, 316, C_WHITE);
  drawText("给家人留一段话", 160, 368, C_MUTED);
  drawText("设置", 354, 442, C_MUTED);
  ui.flush();
}
static inline uint16_t avg565(uint16_t a, uint16_t b) {
  return ((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1);
}
static dice::Quat rollCachePose(float u) {
  u = u < 0 ? 0 : (u > 1 ? 1 : u);
  float ease = 1.0f - powf(1.0f - u, 3.0f);
  dice::Quat settled = diceStartQ;
  float d = fabsf(settled.w * diceTargetQ.w + settled.x * diceTargetQ.x +
                  settled.y * diceTargetQ.y + settled.z * diceTargetQ.z);
  if (d > 0.9999f) {
    int alt = (diceValue % 6) + 1;
    settled = dice::targetForValue(alt, 0.1f);
    float qdot = settled.w * diceTargetQ.w + settled.x * diceTargetQ.x +
                 settled.y * diceTargetQ.y + settled.z * diceTargetQ.z;
    if (qdot < 0) settled = {-settled.w, -settled.x, -settled.y, -settled.z};
  }
  float qdot = settled.w * diceTargetQ.w + settled.x * diceTargetQ.x +
               settled.y * diceTargetQ.y + settled.z * diceTargetQ.z;
  if (qdot < 0) settled = {-settled.w, -settled.x, -settled.y, -settled.z};
  // First rotate under the throw, then settle into the requested upward face.
  float swing = sinf(ease * PI);
  dice::Quat flight = dice::qaxis(dice::v3(0.72f, 0.33f, 0.61f), 2.0f * PI * ease);
  dice::Quat base = {
    settled.w * (1.0f - ease) + diceTargetQ.w * ease,
    settled.x * (1.0f - ease) + diceTargetQ.x * ease,
    settled.y * (1.0f - ease) + diceTargetQ.y * ease,
    settled.z * (1.0f - ease) + diceTargetQ.z * ease
  };
  dice::Quat hop = dice::qaxis(dice::v3(-0.25f, 0.08f, 0.18f), 0.48f * swing);
  return dice::qnorm(dice::qmul(flight, dice::qmul(hop, base)));
}
static void drawCachedRollFrame(int idx) {
  if (!diceAnim || !animCanvas || !diceAnimReady[diceValue - 1] || idx < 0 || idx >= ROLL_FRAMES) return;
  const uint16_t *src = diceAnim + (size_t)(diceValue - 1) * ROLL_FRAMES * ROLL_RES * ROLL_RES;
  const int *box = diceAnimBox[diceValue - 1][idx];
  memset(animCanvas, 0, (size_t)SCR_W * SCR_H * sizeof(uint16_t));
  int cropX = max(0, box[0] - 3);
  int cropY = max(0, box[1] - 3);
  int cropW = min(ROLL_RES - cropX, box[2] - cropX + 4);
  int cropH = min(ROLL_RES - cropY, box[3] - cropY + 4);
  for (int dy = 0; dy < SCR_H; dy++) {
    int sy = cropY + dy / 3;
    if (sy >= cropY + cropH) break;
    const uint16_t *row = src + (size_t)sy * ROLL_RES + cropX;
    for (int dx = 0; dx < SCR_W; dx++) {
      int sx = dx / 3;
      if (sx >= cropW) break;
      animCanvas[(size_t)dy * SCR_W + dx] = row[sx];
    }
  }
  gfx->draw16bitRGBBitmap(0, 0, animCanvas, SCR_W, SCR_H);
}
static void drawDice() {
  if (!diceLow || !diceBig) return;
  uint32_t t0 = millis();
  if (!rolling && diceFaces && diceFaceReady[diceValue - 1]) {
    memcpy(diceBig, diceFaces + (size_t)(diceValue - 1) * SCR_W * SCR_H,
           (size_t)SCR_W * SCR_H * sizeof(uint16_t));
    pill(16, 4, 94, 36, C_CARD);
    drawText("返回", 35, 10, C_WHITE);
    gfx->draw16bitRGBBitmap(0, 0, diceBig, SCR_W, SCR_H);
    const int *b = diceFaceBox[diceValue - 1];
    oldX0 = max(0, b[0] - 8); oldY0 = max(0, b[1] - 8);
    oldX1 = min(SCR_W - 1, b[2] + 8); oldY1 = min(SCR_H - 1, b[3] + 8);
    char s[60]; snprintf(s, sizeof(s), "[dice] cached static=%lu ms\n", (unsigned long)(millis() - t0));
    safeLog(s);
    return;
  }
  const int res = rolling ? 240 : SCR_W;
  // The SDF renderer only clears its current bounds. Clear the entire frame so
  // pixels from a previous pose or resolution cannot survive around the die.
  uint16_t *frame = rolling ? diceLow : diceBig;
  memset(frame, 0, (size_t)res * res * sizeof(uint16_t));
  diceRenderer.buf = frame;
  diceRenderer.W = diceRenderer.H = res;
  diceRenderer.p.focal = 800.0f;
  diceRenderer.render(diceQ, rolling ? 0.22f * sinf((millis() - rollAt) * 0.003f) : 0);
  uint32_t t1 = millis();
  int x0 = 0, y0 = 0, x1 = SCR_W - 1, y1 = SCR_H - 1;
  if (rolling) {
    // Motion renders at half display resolution; the resting frame is native 480.
    int newX0 = max(0, diceRenderer.bboxX0 * SCR_W / res - 8);
    int newY0 = max(0, diceRenderer.bboxY0 * SCR_H / res - 8);
    int newX1 = min(SCR_W - 1, (diceRenderer.bboxX1 + 1) * SCR_W / res + 8);
    int newY1 = min(SCR_H - 1, (diceRenderer.bboxY1 + 1) * SCR_H / res + 8);
    x0 = min(newX0, oldX0) & ~1; y0 = min(newY0, oldY0) & ~1;
    x1 = min(SCR_W - 1, max(newX1, oldX1) | 1);
    y1 = min(SCR_H - 1, max(newY1, oldY1) | 1);
    oldX0 = newX0; oldY0 = newY0; oldX1 = newX1; oldY1 = newY1;
    int width = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++) {
      int iy = y >> 1;
      const uint16_t *src = diceLow + iy * res;
      const uint16_t *next = diceLow + min(iy + 1, res - 1) * res;
      uint16_t *dest = diceBig + (y - y0) * width;
      for (int x = x0; x <= x1; x++) {
        int ix = x >> 1, ix1 = min(ix + 1, res - 1);
        uint16_t top = (x & 1) ? avg565(src[ix], src[ix1]) : src[ix];
        if (y & 1) {
          uint16_t bottom = (x & 1) ? avg565(next[ix], next[ix1]) : next[ix];
          dest[x - x0] = avg565(top, bottom);
        } else dest[x - x0] = top;
      }
    }
  }
  uint32_t t2 = millis();
  if (!rolling) {
    pill(16, 4, 94, 36, C_CARD);
    drawText("返回", 35, 10, C_WHITE);
  }
  gfx->draw16bitRGBBitmap(0, 0, diceBig, SCR_W, SCR_H);
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
    drawText("正在说话", 144, 104, C_WHITE, 2);
    ui.fillCircle(240, 252, 60, C_WHITE);
    ui.fillRoundRect(230, 214, 20, 55, 10, C_RED);
    ui.fillRect(237, 270, 6, 22, C_RED);
    drawText("0秒", 216, 344, C_WHITE);
    drawText("松开发送", 192, 405, C_WHITE);
    ui.flush(); return;
  }
  if (!voiceContactChosen) {
    drawBack("给谁说话");
    for (int i = 0; i < 2; i++) {
      int y = 126 + i * 166;
      pill(28, y, 424, 138, C_CARD, CONTACTS[i].color);
      drawContactAvatar(105, y + 69, i);
      drawText(CONTACTS[i].name, 174, y + 49, C_WHITE, 2);
    }
    ui.flush(); return;
  }
  drawBack(contact == 0 ? "爸爸的语音" : "妈妈的语音");
  if (messageCount == 0) {
    pill(24, 78, 432, 94, C_CARD, CONTACTS[contact].color);
    drawContactAvatar(74, 125, contact);
    drawText("还没有语音", 142, 105, C_WHITE, 2);
  } else {
    pill(24, 72, 432, 112, C_CARD, CONTACTS[contact].color);
    drawContactAvatar(72, 128, contact);
    char duration[32]; snprintf(duration, sizeof(duration), "最新 %u秒", messages[0].seconds);
    drawText(duration, 138, 88, C_WHITE, 2);
    drawText(voiceState == VOICE_PLAYING ? "正在播放" : "点一下重听", 138, 139,
             voiceState == VOICE_PLAYING ? C_TEAL : C_MUTED);
    ui.fillTriangle(396, 108, 396, 148, 426, 128, C_TEAL);
  }
  ui.fillRoundRect(16, 214, 448, 248, 28, C_TEAL);
  ui.fillCircle(240, 308, 50, C_WHITE);
  ui.fillRoundRect(230, 278, 20, 47, 10, C_TEAL);
  ui.fillRect(237, 322, 6, 19, C_TEAL);
  drawText(voiceState == VOICE_SENDING ? "正在发送" : "按住说话",
           130, 378, C_BG, 2);
  if (bannerUntil > millis() && banner[0]) {
    pill(76, 405, 328, 44, C_CARD);
    drawText(banner, 94, 414, C_WHITE);
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
    drawText("再试一次", 144, 216, C_WHITE, 2);
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
  ui.fillScreen(C_BG); drawBack("设置");
  drawSlider(105, "亮度", brightness * 100 / 255, C_GOLD);
  drawSlider(217, "音量", volume, C_TEAL);
  pill(38, 340, 404, 60, C_CARD);
  drawText("重新配网", 55, 357, C_WHITE); drawText(">", 412, 357, C_MUTED);
  drawText(WiFi.status() == WL_CONNECTED ? "网络已连接" : "网络离线", 38, 420, C_MUTED);
  drawText(serverBase, 38, 450, C_MUTED);
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
    voiceContactChosen = false;
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
  bool pending = touchPending;
  touchPending = false;
  if (!pending) return false;
  if (touch.getPoint(tx, ty, touch.getSupportTouchPoint()) == 0) return false;
  x = tx[0]; y = ty[0]; return true;
}
static void savePrefs() {
  prefs.putUChar("bright", brightness);
  prefs.putUChar("volume", volume);
}
static void applyVolume() {
  if (codecOk) es8311_voice_volume_set(codec, volume, nullptr);
}
static bool initAudio() {
  audio_hal_codec_config_t cfg = {
    .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
    .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
    .i2s_iface = { .mode = AUDIO_HAL_MODE_SLAVE, .fmt = AUDIO_HAL_I2S_NORMAL,
      .samples = AUDIO_HAL_16K_SAMPLES, .bits = AUDIO_HAL_BIT_LENGTH_16BITS },
  };
  esp_err_t err = es7210_adc_init(&Wire, &cfg);
  err |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  err |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), GAIN_37_5DB);
  err |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4), GAIN_37_5DB);
  err |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  i2s_config_t ic = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8,
    .dma_buf_len = 256, .use_apll = false, .tx_desc_auto_clear = true,
    .fixed_mclk = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
  };
  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCLK; pins.ws_io_num = I2S_LRCK;
  pins.data_out_num = I2S_DOUT; pins.data_in_num = I2S_DIN;
  pins.mck_io_num = I2S_MCLK;
  err |= i2s_driver_install(I2S_NUM_1, &ic, 0, nullptr);
  err |= i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
  audioOk = err == ESP_OK;
  if (!audioOk) return false;
  codec = es8311_create(0, ES8311_ADDRESS_0);
  es8311_clock_config_t clk = {
    .mclk_inverted = false, .sclk_inverted = false, .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256, .sample_frequency = SAMPLE_RATE,
  };
  codecOk = codec && es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK;
  if (codecOk) {
    codecOk = es8311_sample_frequency_config(codec, clk.mclk_frequency, clk.sample_frequency) == ESP_OK;
    codecOk &= es8311_microphone_config(codec, false) == ESP_OK;
    applyVolume();
  }
  pinMode(PA_PIN, OUTPUT); digitalWrite(PA_PIN, LOW);
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
  i2s_zero_dma_buffer(I2S_NUM_1);
  uint32_t start = millis(), lastDraw = start;
  size_t written = 0;
  int16_t raw[512];
  while (millis() - start < MAX_REC_MS && written + 256 <= pcmCap) {
    size_t got = 0;
    if (i2s_read(I2S_NUM_1, raw, sizeof(raw), &got, pdMS_TO_TICKS(80)) == ESP_OK) {
      size_t samples = got / 2;
      for (size_t i = 0; i + 1 < samples && written < pcmCap; i += 2)
        pcm[written++] = ((int32_t)raw[i] + raw[i + 1]) / 2;
    }
    uint32_t now = millis();
    // IRQ edges can occur while a finger moves. Confirm a release by polling
    // the controller twice before stopping the recording.
    if (now - start > MIN_REC_MS) {
      touchPending = false;
      if (!touch.isPressed()) {
        vTaskDelay(pdMS_TO_TICKS(45));
        if (!touch.isPressed()) break;
      }
    }
    if (now - lastDraw > 250) {
      lastDraw = now;
      char s[18]; snprintf(s, sizeof(s), "%u秒", (unsigned)((now - start) / 1000));
      ui.fillRect(185, 339, 110, 38, C_RED); drawText(s, 216, 344, C_WHITE);
      ui.flush();
    }
  }
  return written;
}
static bool uploadAudio(size_t count) {
  if (WiFi.status() != WL_CONNECTED) return false;
  size_t total = 44 + count * 2;
  uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  if (!body) return false;
  wavHeader(body, count * 2); memcpy(body + 44, pcm, count * 2);
  char url[180]; snprintf(url, sizeof(url), "%s/voice?to=%s", serverBase, CONTACTS[contact].key);
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
  char url[184]; snprintf(url, sizeof(url), "%s/api/list?contact=%s&direction=in&limit=3", serverBase, CONTACTS[contact].key);
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
        if (!cJSON_IsString(c) || strcmp(c->valuestring, CONTACTS[contact].key)) continue;
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id)) continue;
        cJSON *direction = cJSON_GetObjectItem(item, "direction");
        if (!cJSON_IsString(direction) || strcmp(direction->valuestring, "in")) continue;
        Message &m = messages[messageCount];
        strlcpy(m.id, id->valuestring, sizeof(m.id));
        cJSON *seconds = cJSON_GetObjectItem(item, "seconds");
        m.seconds = cJSON_IsNumber(seconds) ? (uint16_t)ceil(seconds->valuedouble) : 0;
        m.incoming = true;
        cJSON *ts = cJSON_GetObjectItem(item, "ts");
        m.ts = cJSON_IsNumber(ts) ? (uint32_t)(ts->valuedouble / 1000) : 0;
        if (++messageCount == MAX_MESSAGES) break;
      }
    }
    cJSON_Delete(root);
  }
  http.end(); if (page == PAGE_VOICE) drawVoice();
}
static bool playMessage(int idx) {
  if (!codecOk || idx < 0 || idx >= messageCount || WiFi.status() != WL_CONNECTED) return false;
  char url[190]; snprintf(url, sizeof(url), "%s/audio/%s", serverBase, messages[idx].id);
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(6000);
  bool ok = false;
  size_t playedBytes = 0;
  if (http.GET() == 200) {
    WiFiClient *stream = http.getStreamPtr();
    uint8_t header[44];
    uint32_t rate = 0;
    if (stream->readBytes(header, 44) == 44) memcpy(&rate, header + 24, 4);
    if (rate && !memcmp(header, "RIFF", 4) &&
        !memcmp(header + 8, "WAVE", 4) && !memcmp(header + 36, "data", 4) &&
        header[20] == 1 && header[22] == 1 && header[34] == 16 &&
        (rate == SAMPLE_RATE || rate == 32000)) {
      bool clockChanged = rate != SAMPLE_RATE;
      bool clockOk = !clockChanged ||
          (i2s_set_clk(I2S_NUM_1, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) == ESP_OK &&
           es8311_sample_frequency_config(codec, rate * 256, rate) == ESP_OK);
      if (clockOk) {
      voiceState = VOICE_PLAYING; drawVoice();
      digitalWrite(PA_PIN, HIGH);
      uint32_t remaining; memcpy(&remaining, header + 40, 4);
      int16_t mono[256], stereo[512]; ok = true;
      touchPending = false;
      while (remaining > 0 && (stream->connected() || stream->available())) {
        size_t want = remaining < sizeof(mono) ? remaining : sizeof(mono);
        size_t got = stream->readBytes((uint8_t *)mono, want);
        if (!got) { ok = false; break; }
        remaining -= got;
        for (size_t i = 0; i < got / 2; i++) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        size_t sent = 0;
        if (i2s_write(I2S_NUM_1, stereo, got * 2, &sent, pdMS_TO_TICKS(1000)) != ESP_OK || sent != got * 2) {
          ok = false; break;
        }
        playedBytes += got;
        int16_t stopX, stopY;
        if (touchPoint(stopX, stopY)) break;
      }
      i2s_zero_dma_buffer(I2S_NUM_1);
      digitalWrite(PA_PIN, LOW);
      }
      if (clockChanged) {
        i2s_set_clk(I2S_NUM_1, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
        es8311_sample_frequency_config(codec, SAMPLE_RATE * 256, SAMPLE_RATE);
      }
    }
  }
  http.end(); voiceState = VOICE_READY; drawVoice();
  char report[75]; snprintf(report, sizeof(report), "[play] ok=%d pcm=%u bytes\n",
                            ok, (unsigned)playedBytes);
  safeLog(report);
  return ok;
}
static void pollIncoming() {
  if (WiFi.status() != WL_CONNECTED || rolling || diceSoundActive || voiceState != VOICE_READY) return;
  if (page == PAGE_SETTINGS) return;
  inboxCheckRequested = false;
  for (int who = 0; who < 2; who++) {
    char url[184];
    snprintf(url, sizeof(url), "%s/api/list?contact=%s&direction=in&limit=1", serverBase, CONTACTS[who].key);
    WiFiClient client; HTTPClient http;
    if (!http.begin(client, url)) continue;
  http.setReuse(false);
  http.setConnectTimeout(1500);
  http.setTimeout(1800);
    int status = http.GET();
    if (status != 200) { http.end(); continue; }
    String body = http.getString();
    http.end();
    cJSON *root = cJSON_Parse(body.c_str());
    cJSON *items = root ? cJSON_GetObjectItem(root, "items") : nullptr;
    cJSON *item = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : nullptr;
    cJSON *id = item ? cJSON_GetObjectItem(item, "id") : nullptr;
    if (cJSON_IsString(id) && id->valuestring && id->valuestring[0]) {
      bool changed = inboxPrimed[who] && strcmp(lastIncomingId[who], id->valuestring);
      strlcpy(lastIncomingId[who], id->valuestring, sizeof(lastIncomingId[who]));
      inboxPrimed[who] = true;
      cJSON_Delete(root);
      if (changed) {
        char logLine[90];
        snprintf(logLine, sizeof(logLine), "[inbox] new %s id=%s\n", CONTACTS[who].key, lastIncomingId[who]);
        safeLog(logLine);
        setPage(PAGE_VOICE);
        contact = who; voiceContactChosen = true; voiceListening = true;
        fetchMessages();
        if (messageCount && !strcmp(messages[0].id, lastIncomingId[who])) {
          notice(who == 0 ? "爸爸发来语音" : "妈妈发来语音");
          playMessage(0);
        }
        return;
      }
    } else { inboxPrimed[who] = true; cJSON_Delete(root); }
  }
}
static void wifiPortal(bool force) {
  ui.fillScreen(C_BG); drawBack("配网");
  drawText("手机连接热点", 58, 140, C_WHITE);
  drawText("FamilyHub-Setup", 58, 190, C_TEAL);
  drawText("打开 192.168.4.1", 58, 245, C_MUTED);
  ui.flush();
  WiFiManager wm; wm.setConfigPortalTimeout(180);
  WiFiManagerParameter serverParam("srv", "服务器地址", serverBase, 110);
  wm.addParameter(&serverParam);
  bool ok = force ? wm.startConfigPortal("FamilyHub-Setup") : wm.autoConnect("FamilyHub-Setup");
  if (ok) {
    const char *s = serverParam.getValue();
    if (s && !strncmp(s, "http://", 7)) {
      strlcpy(serverBase, s, sizeof(serverBase)); prefs.putString("server", serverBase);
    }
    notice("网络已连接");
  } else notice("网络连接失败");
  redraw();
}
static void diceSoundTask(void *) {
  // Short filtered rattles followed by a low landing thud. The I2S write
  // paces this task while the dice renderer runs on the other core.
  int16_t stereo[512];
  uint32_t noise = esp_random();
  float filtered = 0;
  const int total = SAMPLE_RATE * 9 / 10;
  digitalWrite(PA_PIN, HIGH);
  for (int start = 0; start < total && !diceSoundCancel; start += 256) {
    int count = min(256, total - start);
    for (int i = 0; i < count; i++) {
      float t = (start + i) / (float)SAMPLE_RATE;
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
      int16_t sample = (int16_t)(filtered * rattle * 6500 + thud * 5000);
      stereo[2 * i] = stereo[2 * i + 1] = sample;
    }
    size_t sent = 0;
    if (i2s_write(I2S_NUM_1, stereo, count * 4, &sent, pdMS_TO_TICKS(200)) != ESP_OK || sent != (size_t)count * 4) break;
  }
  if (!diceSoundCancel) vTaskDelay(pdMS_TO_TICKS(80));
  i2s_zero_dma_buffer(I2S_NUM_1);
  digitalWrite(PA_PIN, LOW);
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
  if (diceAnim) {
    for (int face = 0; face < 6; face++) {
      diceTargetQ = dice::targetForValue(face + 1, 0.1f);
      for (int frame = 0; frame < ROLL_FRAMES; frame++) {
        renderer.W = renderer.H = ROLL_RES;
        renderer.p.focal = 800.0f;
        renderer.buf = diceAnim + ((size_t)face * ROLL_FRAMES + frame) * ROLL_RES * ROLL_RES;
        memset(renderer.buf, 0, (size_t)ROLL_RES * ROLL_RES * sizeof(uint16_t));
        renderer.render(rollCachePose((float)frame / ROLL_FRAMES),
                        0.20f * sinf((float)frame / ROLL_FRAMES * PI));
        diceAnimBox[face][frame][0] = renderer.bboxX0;
        diceAnimBox[face][frame][1] = renderer.bboxY0;
        diceAnimBox[face][frame][2] = renderer.bboxX1;
        diceAnimBox[face][frame][3] = renderer.bboxY1;
        vTaskDelay(1);
      }
      diceAnimReady[face] = true;
    }
    safeLog("[dice] motion cache ready\n");
  }
  vTaskDelete(nullptr);
}
static void rollDice() {
  if (rolling) return;
  diceValue = 1 + (esp_random() % 6);
  diceStartQ = diceQ;
  diceTargetQ = dice::targetForValue(diceValue, 0.1f);
  rollAt = millis(); rolling = true; rollFrames = 0;
  rollRenderMs = rollScaleMs = rollPushMs = 0;
  rollUsesCache = diceAnim && animCanvas && diceAnimReady[diceValue - 1];
  rollCacheFrame = 0;
  rollCacheNextAt = rollAt + ROLL_MS / ROLL_FRAMES;
  if (rollUsesCache) drawCachedRollFrame(0);
  // Do not start I2S on the same frame as the first dice refresh while
  // isolating the CO5300 corruption.
}
static void updateDice() {
  if (page == PAGE_DICE && rolling && rollUsesCache) {
    if (millis() < rollCacheNextAt) return;
    if (rollCacheFrame + 1 < ROLL_FRAMES) {
      rollCacheFrame++;
      drawCachedRollFrame(rollCacheFrame);
      rollCacheNextAt = rollAt + (uint32_t)(rollCacheFrame + 1) * ROLL_MS / ROLL_FRAMES;
      rollFrames++;
      return;
    }
    diceQ = diceTargetQ;
    rolling = false;
    drawDice();
    char s[100]; snprintf(s, sizeof(s), "[dice] cached %u frames in %lu ms\n",
                          rollFrames, (unsigned long)(millis() - rollAt));
    safeLog(s);
    return;
  }
  if (page != PAGE_DICE || !rolling || millis() - diceFrameAt < 50) return;
  diceFrameAt = millis();
  float t = (millis() - rollAt) / 1000.0f;
  if (t >= ROLL_MS / 1000.0f) {
    diceQ = diceTargetQ;
    rolling = false;
  } else {
    float ease = 1.0f - powf(1.0f - t * 1000.0f / ROLL_MS, 3.0f);
    dice::Quat spin = dice::qaxis(dice::v3(0.64f, 0.78f, 0.21f), (1.0f - ease) * 6.5f);
    diceQ = dice::qnorm(dice::qmul(spin, diceTargetQ));
  }
  drawDice();
  if (rolling) rollFrames++;
  if (!rolling) {
    char s[120]; snprintf(s, sizeof(s), "[dice] %u motion frames in %lu ms\n",
                          rollFrames, (unsigned long)(millis() - rollAt));
    safeLog(s);
  }
}
static void handleTap(int x, int y) {
  if (millis() - lastTouchAt < 260) return;
  lastTouchAt = millis();
  if (page == PAGE_HOME) {
    if (y >= 136 && y <= 268) setPage(PAGE_DICE);
    else if (y >= 286 && y <= 418) setPage(PAGE_VOICE);
    else if (y > 425) { settingsReturn = PAGE_HOME; setPage(PAGE_SETTINGS); }
  } else if (page == PAGE_DICE) {
    if (x < 120 && y < 75) setPage(PAGE_HOME); else rollDice();
  } else if (page == PAGE_VOICE) {
    if (!voiceContactChosen) {
      if (x < 70 && y < 65) setPage(PAGE_HOME);
      else if (y >= 126 && y < 264) {
        contact = 0; voiceContactChosen = true; voiceListening = false;
        fetchMessages();
      } else if (y >= 292 && y < 430) {
        contact = 1; voiceContactChosen = true; voiceListening = false;
        fetchMessages();
      }
    } else if (x < 70 && y < 65) {
      voiceContactChosen = false; voiceListening = false; drawVoice();
    } else if (y >= 72 && y < 184) {
      if (messageCount && !playMessage(0)) notice("播放失败");
    } else if (y >= 214) {
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
    if (x < 75 && y < 65) setPage(settingsReturn);
    else if (y >= 145 && y <= 192) {
      brightness = constrain((x - 50) * 255 / 380, 12, 255);
      gfx->setBrightness(brightness); savePrefs(); drawSettings();
    } else if (y >= 257 && y <= 304) {
      volume = constrain((x - 50) * 100 / 380, 0, 100);
      applyVolume(); savePrefs(); drawSettings();
    } else if (y >= 340 && y <= 400) wifiPortal(true);
  }
}

void setup() {
  USBSerial.begin(115200); delay(120);
  prefs.begin("family-hub", false);
  brightness = prefs.getUChar("bright", 200); volume = prefs.getUChar("volume", 68);
  String saved = prefs.getString("server", DEFAULT_SERVER);
  strlcpy(serverBase, saved.c_str(), sizeof(serverBase));
  Wire.begin(IIC_SDA, IIC_SCL);
  pinMode(KEY_PIN, INPUT_PULLUP); pinMode(BOOT_PIN, INPUT_PULLUP);
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(SCR_W, SCR_H); touch.setSwapXY(true); touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchISR, FALLING);
  }
  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_500Hz, SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
  }
  if (!gfx->begin()) { safeLog("display failed\n"); while (true) delay(1000); }
  bus->writeC8D8(0x36, 0xA0); gfx->setBrightness(brightness);
  diceLow = (uint16_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
  diceBig = (uint16_t *)heap_caps_malloc(SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
  diceFaces = (uint16_t *)heap_caps_malloc((size_t)6 * SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
  diceAnim = (uint16_t *)heap_caps_malloc((size_t)6 * ROLL_FRAMES * ROLL_RES * ROLL_RES * 2, MALLOC_CAP_SPIRAM);
  animCanvas = (uint16_t *)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
  pcm = (int16_t *)heap_caps_malloc(pcmCap * 2, MALLOC_CAP_SPIRAM);
  if (!diceLow || !diceBig || !pcm) {
    safeLog("PSRAM allocation failed\n");
    while (true) delay(1000);
  }
  diceQ = dice::targetForValue(1, 0.1f);
  initAudio();
  drawHome();
  if (diceFaces) xTaskCreatePinnedToCore(diceCacheTask, "dice-cache", 6144, nullptr, 1, nullptr, 0);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(100);
  if (WiFi.status() != WL_CONNECTED) wifiPortal(false);
  if (WiFi.status() == WL_CONNECTED) pollIncoming();
  safeLog("family-hub ready\n");
}

void loop() {
  if (screenLocked) {
    if (digitalRead(KEY_PIN) == LOW || digitalRead(BOOT_PIN) == LOW) {
      screenLocked = false;
      bus->writeC8D8(0x53, 0x20);  // display on
      gfx->setBrightness(brightness);
      redraw();
      delay(250);
    } else delay(20);
    return;
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
    else if (cmd == 's') {
      char s[100]; snprintf(s, sizeof(s), "[status] touch=%d imu=%d audio=%d codec=%d wifi=%d\n",
                            touchOk, imuOk, audioOk, codecOk, WiFi.status() == WL_CONNECTED);
      safeLog(s);
    }
  }
  int16_t x, y;
  if (touchPoint(x, y)) handleTap(x, y);
  static int lastKey = HIGH, lastBoot = HIGH;
  static uint32_t bootAt = 0, lastShake = 0;
  int key = digitalRead(KEY_PIN), boot = digitalRead(BOOT_PIN);
  if (key == LOW && lastKey == HIGH) {
    if (page == PAGE_DICE) rollDice();
    else {
      screenLocked = true;
      bus->writeC8D8(0x53, 0x28);  // display off
      drawLocked();
    }
  }
  if (boot == LOW && lastBoot == HIGH) {
    if (millis() - bootAt < 430) setPage(PAGE_HOME);
    bootAt = millis();
  }
  lastKey = key; lastBoot = boot;
  if (page == PAGE_DICE && imuOk && millis() - lastShake > 20) {
    lastShake = millis();
    if (imu.getDataReady() && imu.getAccelerometer(acc.x, acc.y, acc.z)) {
      float mag = sqrtf(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);
      shakeScore = fabsf(mag - 1.0f) > 0.55f ? shakeScore + 1 : shakeScore * 0.75f;
      if (shakeScore > 4) { shakeScore = 0; rollDice(); }
    }
  }
  updateDice();
  if (inboxCheckRequested || millis() - lastInboxPollAt > 5000) {
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

/*
 * 语音信箱 · 微雪 ESP32-S3-Touch-AMOLED-2.16
 *
 * 按住屏幕上的联系人按钮 → 录音 → 松手发送 → Mac 上的 voice-inbox 存档 + 给那个人手机推送
 *
 * 为什么不在板子上编码 Opus：
 *   libopus 编进固件要几百 KB flash，240MHz 单核实时编码 16kHz 有风险。
 *   板子只负责「录裸 WAV + POST」，转码/推送放 Mac —— 代码量小、出问题好排查。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>   // 连不上时开热点，手机网页配网（存 NVS，下次自动连）
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <Wire.h>

#include "Arduino_GFX_Library.h"
#include <TouchDrvCSTXXX.hpp>
#include "HWCDC.h"
#include "es7210.h"

#if __has_include("private-config.h")
#include "private-config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef INBOX_BASE
#define INBOX_BASE ""
#endif
#ifndef INBOX_SECRET
#define INBOX_SECRET ""
#endif


// ---------------------------------------------------------------- 配置
// WiFi：优先用 NVS 里保存的（配网过就用那个）；没有则尝试这里的默认值；
// 都连不上 → 开热点 "VoiceMemo-Setup"，手机连上去配网。

#define RECORD_MS     8000            // 最长录音
#define SAMPLE_RATE   16000
#define MIN_MS        400             // 短于这个当误触

// ---------------------------------------------------------------- 引脚
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  38
#define LCD_RESET 39
#define LCD_CS    12
#define IIC_SDA   15
#define IIC_SCL   14
#define TP_INT    11
#define TP_RST    40

#define PIN_I2S_BCLK  9
#define PIN_I2S_LRCK  45
#define PIN_I2S_DIN   10
#define PIN_I2S_MCLK  42

#define SCR_W 480
#define SCR_H 480

// ---------------------------------------------------------------- 联系人（改这里）
struct Contact { const char *key; const char *label; uint16_t color; };
static const Contact CONTACTS[] = {
  { "dad", "爸爸",   0xFD20 },   // 橙
  { "mom", "妈妈",   0xF81F },   // 粉
  { "me",  "我自己", 0x07FF },   // 青
};
static const int N_CONTACTS = sizeof(CONTACTS) / sizeof(CONTACTS[0]);

// ---------------------------------------------------------------- 全局
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, SCR_W, SCR_H, 0, 0, 0, 0);

HWCDC USBSerial;
static TouchDrvCST92xx touch;
static int16_t tx[2], ty[2];
static volatile bool touchPending = false;
static bool touchOk = false;

static int16_t *pcm = nullptr;
static size_t   pcmCap = 0;

enum State { ST_IDLE, ST_RECORDING, ST_SENDING, ST_DONE, ST_ERROR };
static State state = ST_IDLE;
static int   sel = 0;                 // 当前选中的联系人
static char  status[64] = "";
static char  gInboxBase[112] = INBOX_BASE;   // 可在配网页覆盖
static uint32_t stateAt = 0;

// 按钮几何（3 个横排）
#define BTN_W 140
#define BTN_H 120
#define BTN_Y 210
static inline int btnX(int i) { return 20 + i * (BTN_W + 20); }

// ---------------------------------------------------------------- UI
static void drawUI() {
  gfx->fillScreen(0x0000);

  // 标题
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(3);
  gfx->setCursor(150, 60);
  gfx->print("Voice Memo");

  gfx->setTextSize(2);
  gfx->setTextColor(0x8410);
  gfx->setCursor(120, 110);
  gfx->print("hold to record");

  // 三个联系人按钮
  for (int i = 0; i < N_CONTACTS; i++) {
    int x = btnX(i);
    bool active = (i == sel);
    uint16_t c = CONTACTS[i].color;

    if (state == ST_RECORDING && active) {
      // 录音中：红底 + 边框
      gfx->fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 16, 0xF800);
      gfx->drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 16, 0xFFFF);
      gfx->drawRoundRect(x - 1, BTN_Y - 1, BTN_W + 2, BTN_H + 2, 17, 0xFFFF);
    } else if (active) {
      gfx->fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 16, c);
      gfx->drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 16, 0xFFFF);
    } else {
      gfx->drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 16, c);
    }

    // 名字（中文用内建字体画不出，用 ASCII 兜底 + 颜色区分）
    gfx->setTextSize(3);
    gfx->setTextColor(active ? 0x0000 : c);
    const char *ascii = (i == 0) ? "DAD" : (i == 1) ? "MOM" : "ME";
    gfx->setCursor(x + 34, BTN_Y + 48);
    gfx->print(ascii);
  }

  // 状态行
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(20, 370);
  gfx->print(status);

  // 录音进度条
  if (state == ST_RECORDING) {
    uint32_t el = millis() - stateAt;
    int w = (int)((float)el / RECORD_MS * (SCR_W - 40));
    if (w > SCR_W - 40) w = SCR_W - 40;
    gfx->fillRect(20, 400, w, 8, 0xF800);
  }
}

static void setState(State s, const char *msg) {
  state = s;
  stateAt = millis();
  strncpy(status, msg, sizeof(status) - 1);
  status[sizeof(status) - 1] = 0;
  drawUI();
}

// ---------------------------------------------------------------- ES7210 + I2S
static bool audioInit() {
  Wire.begin(IIC_SDA, IIC_SCL);

  audio_hal_codec_config_t cfg = {
    .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
    .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
    .i2s_iface = {
      .mode = AUDIO_HAL_MODE_SLAVE,
      .fmt = AUDIO_HAL_I2S_NORMAL,
      .samples = AUDIO_HAL_16K_SAMPLES,
      .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
    },
  };

  esp_err_t r = ESP_OK;
  r |= es7210_adc_init(&Wire, &cfg);
  r |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  r |= es7210_adc_set_gain(
      (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
      (es7210_gain_value_t)GAIN_37_5DB);
  r |= es7210_adc_set_gain(
      (es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
      (es7210_gain_value_t)GAIN_37_5DB);
  r |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  if (r != ESP_OK) {
    USBSerial.printf("[audio] ES7210 init failed: %d\n", r);
    return false;
  }

  i2s_config_t ic = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ALL_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    .chan_mask = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
    .total_chan = 2,
  };

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRCK;
  pins.data_in_num = PIN_I2S_DIN;
  pins.mck_io_num = PIN_I2S_MCLK;

  if (i2s_driver_install(I2S_NUM_1, &ic, 0, NULL) != ESP_OK) return false;
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
  USBSerial.println("[audio] ok");
  return true;
}

// ---------------------------------------------------------------- 录音
// 一边录一边检测松手：松手就停，不用等满 RECORD_MS
static bool touchStillDown();

static size_t recordLoop() {
  const size_t CH = 512;
  static int16_t raw[CH];
  size_t written = 0;
  uint32_t t0 = millis();

  while (millis() - t0 < RECORD_MS && written + CH / 2 <= pcmCap) {
    // 每 ~100ms 检查一次是否松手
    static uint32_t lastChk = 0;
    if (millis() - lastChk > 50) {
      lastChk = millis();
      if (!touchStillDown() && millis() - t0 > MIN_MS) break;
      static uint32_t lastDraw = 0;
      if (millis() - lastDraw > 100) { lastDraw = millis(); drawUI(); }
    }

    size_t got = 0;
    if (i2s_read(I2S_NUM_1, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK) continue;
    if (got < 4) continue;

    size_t n = got / 2;
    for (size_t i = 0; i + 1 < n; i += 2) {
      pcm[written++] = (int16_t)(((int32_t)raw[i] + (int32_t)raw[i + 1]) / 2);
    }
  }
  return written;
}

// ---------------------------------------------------------------- WAV + 上传
static void putWavHeader(uint8_t *h, size_t dataBytes) {
  const uint32_t sr = SAMPLE_RATE;
  const uint16_t ch = 1, bits = 16;
  const uint32_t byteRate = sr * ch * bits / 8;
  const uint16_t align = ch * bits / 8;
  uint32_t v32; uint16_t v16;

  memcpy(h + 0, "RIFF", 4);
  v32 = 36 + dataBytes; memcpy(h + 4, &v32, 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  v32 = 16; memcpy(h + 16, &v32, 4);
  v16 = 1; memcpy(h + 20, &v16, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  v32 = dataBytes; memcpy(h + 40, &v32, 4);
}

static bool upload(size_t samples) {
  const size_t dataBytes = samples * 2;
  const size_t total = 44 + dataBytes;

  char url[192];
  snprintf(url, sizeof(url), "%s/voice?to=%s", gInboxBase, CONTACTS[sel].key);

  uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  if (!body) { USBSerial.println("[http] alloc fail"); return false; }
  putWavHeader(body, dataBytes);
  memcpy(body + 44, pcm, dataBytes);

  WiFiClient client;
  HTTPClient http;
  bool ok = false;
  if (http.begin(client, url)) {
    http.setTimeout(30000);
    http.addHeader("Content-Type", "audio/wav");
    if (strlen(INBOX_SECRET) > 0) http.addHeader("X-Voice-Secret", INBOX_SECRET);
    int code = http.POST(body, total);
    USBSerial.printf("[http] %d\n", code);
    ok = (code == 200);
    http.end();
  } else {
    USBSerial.println("[http] begin fail");
  }
  free(body);
  return ok;
}

// ---------------------------------------------------------------- 触摸
void IRAM_ATTR onTouchISR() { touchPending = true; }

/** 读一次触摸；有按下返回 true 并给出坐标 */
static bool readTouch(int16_t &x, int16_t &y) {
  if (!touchOk) return false;
  bool p;
  noInterrupts(); p = touchPending; touchPending = false; interrupts();
  if (!p) return false;
  if (touch.getPoint(tx, ty, touch.getSupportTouchPoint()) == 0) return false;
  x = tx[0]; y = ty[0];
  return true;
}

// 录音期间用：只判断「还在按」——靠 TP_INT 是否持续触发不准，
// 改为主动读触摸芯片当前状态
// 按住期间 getPoint() 可能偶发返回 0（触摸芯片通常只在状态变化时更新坐标），
// 单次读不到不能算松手 —— 要连续 MISS_TO_STOP 次才算。
static bool touchStillDown() {
  if (!touchOk) return false;
  static uint8_t miss = 0;
  uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
  if (n > 0) { miss = 0; return true; }
  if (++miss >= 5) { miss = 0; return false; }   // 约 500ms 连续读不到才停
  return true;
}

static int hitButton(int16_t x, int16_t y) {
  for (int i = 0; i < N_CONTACTS; i++) {
    int bx = btnX(i);
    if (x >= bx && x <= bx + BTN_W && y >= BTN_Y && y <= BTN_Y + BTN_H) return i;
  }
  return -1;
}

// ---------------------------------------------------------------- setup
void setup() {
  USBSerial.begin(115200);
  delay(200);
  USBSerial.println("\n=== voice memo ===");

  if (!gfx->begin()) { USBSerial.println("gfx fail"); while (1) delay(1000); }
  bus->writeC8D8(0x36, 0xA0);
  gfx->setBrightness(200);
  gfx->fillScreen(0x0000);

  // ⚠️ 触摸/IMU 要在 gfx->begin() 之前初始化（官方 05_LVGL_Widgets 顺序）
  //    这里 gfx 已经先起了，但 CST9220 实测仍可用；若出现 touch not found，
  //    把这段挪到 gfx->begin() 之前。
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(SCR_W, SCR_H);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchISR, FALLING);
    USBSerial.printf("touch ok: %s\n", touch.getModelName());
  } else {
    USBSerial.println("touch not found");
  }

  if (!audioInit()) setState(ST_ERROR, "audio init failed");

  pcmCap = (size_t)SAMPLE_RATE * RECORD_MS / 1000;
  pcm = (int16_t *)heap_caps_malloc(pcmCap * 2, MALLOC_CAP_SPIRAM);
  if (!pcm) { USBSerial.println("pcm alloc fail"); while (1) delay(1000); }
  USBSerial.printf("pcm %u samples (%.1f s)\n", (unsigned)pcmCap, (float)pcmCap / SAMPLE_RATE);

  // ---- WiFi：先试默认，不行就开热点配网
  setState(ST_IDLE, "wifi connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(300);

  if (WiFi.status() != WL_CONNECTED) {
    // 连不上 → 开热点让手机配网
    USBSerial.println("[wifi] default failed, starting config portal");
    setState(ST_ERROR, "wifi setup: join VoiceMemo-Setup");

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);          // 3 分钟没配好就放弃，避免一直卡着
    wm.setAPCallback([](WiFiManager *) {
      USBSerial.println("[wifi] AP: VoiceMemo-Setup  打开 192.168.4.1");
    });
    // 自定义参数：服务器地址也能在网页里改（换地方时不用重烧固件）
    WiFiManagerParameter pSrv("srv", "服务器地址 (Mac/Air 的 IP:端口)",
                              INBOX_BASE, 96);
    wm.addParameter(&pSrv);

    bool ok = wm.autoConnect("VoiceMemo-Setup");
    if (ok) {
      USBSerial.printf("[wifi] configured, ip=%s\n", WiFi.localIP().toString().c_str());
      const char *srv = pSrv.getValue();
      if (srv && strlen(srv) > 8) {
        strncpy(gInboxBase, srv, sizeof(gInboxBase) - 1);
        USBSerial.printf("[wifi] server overridden: %s\n", gInboxBase);
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    USBSerial.printf("wifi ok %s\n", WiFi.localIP().toString().c_str());
    setState(ST_IDLE, "ready");
  } else {
    setState(ST_ERROR, "wifi failed");
  }
}

// ---------------------------------------------------------------- loop
void loop() {
  int16_t x, y;
  bool down = readTouch(x, y);

  if (down) {
    int hit = hitButton(x, y);
    if (hit >= 0) {
      sel = hit;
      setState(ST_RECORDING, "recording...");
      size_t n = recordLoop();
      float secs = (float)n / SAMPLE_RATE;
      USBSerial.printf("[rec] %u samples (%.2f s)\n", (unsigned)n, secs);

      if (n < (size_t)(SAMPLE_RATE * MIN_MS / 1000)) {
        setState(ST_ERROR, "too short");
      } else {
        setState(ST_SENDING, "sending...");
        bool ok = upload(n);
        if (ok) {
          char m[48];
          snprintf(m, sizeof(m), "sent to %s (%.1fs)",
                   CONTACTS[sel].key, secs);
          setState(ST_DONE, m);
        } else {
          setState(ST_ERROR, "send failed");
        }
      }
    }
  }

  // 完成/错误状态 3 秒后回到 idle
  if ((state == ST_DONE || state == ST_ERROR) && millis() - stateAt > 3000) {
    setState(ST_IDLE, "ready");
  }

  delay(20);
}

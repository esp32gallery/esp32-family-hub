#include "board.h"

#include <math.h>
#include <string.h>

#include <M5Unified.h>

namespace board {
namespace {

constexpr uint8_t kIoeAddr = 0x4F;
constexpr uint32_t kI2cHz = 100000;

bool gTouch = false;
bool gVibe = false;
uint32_t gVibeOffAt = 0;

// PWM channel 0 on M5IOE1 is pin index 8, which is PYG9, the vibration motor.
// Channel 1 is the panel power rail. Do not program any other channel.
void vibeDuty(uint16_t duty12, bool on) {
  if (!M5.In_I2C.isEnabled()) return;
  uint8_t hi = (uint8_t)((duty12 >> 8) & 0x0F);
  if (on) hi |= 0x80;
  M5.In_I2C.writeRegister8(kIoeAddr, 0x1B, (uint8_t)(duty12 & 0xFF), kI2cHz);
  M5.In_I2C.writeRegister8(kIoeAddr, 0x1C, hi, kI2cHz);
}

void vibeInit() {
  if (!M5.In_I2C.isEnabled()) return;
  bool ok = true;
  ok = M5.In_I2C.bitOn(kIoeAddr, 0x04, 0x01, kI2cHz) && ok;
  ok = M5.In_I2C.bitOff(kIoeAddr, 0x14, 0x01, kI2cHz) && ok;
  ok = M5.In_I2C.writeRegister8(kIoeAddr, 0x25, 0xA0, kI2cHz) && ok;  // 4000 Hz
  ok = M5.In_I2C.writeRegister8(kIoeAddr, 0x26, 0x0F, kI2cHz) && ok;
  vibeDuty(0, false);
  gVibe = ok;
  Serial.printf("[vibe] %s\n", ok ? "ready" : "unavailable");
}

void applyTouchAffine() {
  auto *panel = M5.Display.getPanel();
  if (!panel) {
    Serial.println("[touch] no panel");
    gTouch = false;
    return;
  }
  // Five-point fit on this StopWatch: screen = a * raw + b, residual <= 16px.
  // M5GFX's framebuffer panel runs touchCalibrate() while its config is still
  // the default 240x240 (raw 0..233 -> 0..239, scale 1.0258) and never
  // recomputes it after the real 468 config is copied in. This CST820 also
  // reports about 0..540, not 0..233, so a stock 468 calibration is wrong too.
  float affine[6] = {0.9207f, 0.0f, 23.0f, 0.0f, 0.8664f, 27.0f};
  if (TOUCH_MIRROR_X) {
    affine[0] = -affine[0];
    affine[1] = -affine[1];
    affine[2] = (kWidth - 1) - affine[2];
  }
  if (TOUCH_MIRROR_Y) {
    affine[3] = -affine[3];
    affine[4] = -affine[4];
    affine[5] = (kHeight - 1) - affine[5];
  }
  panel->setCalibrateAffine(affine);
  // The CST820 shares I2C port 1 with the PMIC and IO expander, which run at
  // 100kHz. M5GFX's StopWatch config asks the touch chip for 400kHz. A failed
  // read is reported as "not touching", so a 50–120ms tap often ends before
  // any transaction ACKs; a longer press eventually lands one.
  lgfx::ITouch *tp = panel->touch();
  if (tp) {
    auto tcfg = tp->config();
    tcfg.freq = kI2cHz;
    tp->config(tcfg);
  }
  gTouch = M5.Touch.isEnabled();
  Serial.printf("[touch] affine x=%.4f*raw%+.1f y=%.4f*raw%+.1f mirror=%d%d en=%d i2c=%luHz\n",
                affine[0], affine[2], affine[4], affine[5], TOUCH_MIRROR_X, TOUCH_MIRROR_Y,
                (int)gTouch, (unsigned long)kI2cHz);
}

void maskRound(uint16_t *fb) {
  const int cx = kWidth / 2;
  const int r = kWidth / 2 - 2;
  const int r2 = r * r;
  for (int y = 0; y < kHeight; ++y) {
    int dy = y - cx;
    int rem = r2 - dy * dy;
    uint16_t *row = fb + (size_t)y * kWidth;
    if (rem <= 0) {
      memset(row, 0, (size_t)kWidth * sizeof(uint16_t));
      continue;
    }
    int dx = (int)sqrtf((float)rem);
    int x0 = cx - dx;
    int x1 = cx + dx;
    if (x0 > 0) memset(row, 0, (size_t)x0 * sizeof(uint16_t));
    if (x1 < kWidth - 1) {
      memset(row + x1 + 1, 0, (size_t)(kWidth - x1 - 1) * sizeof(uint16_t));
    }
  }
}

}  // namespace

bool begin() {
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  cfg.clear_display = true;
  cfg.internal_imu = true;
  cfg.internal_rtc = true;
  cfg.internal_spk = true;
  // Arm the official ES8311 mic callback, but do not call Mic.begin() here.
  // begin() takes over I2C port 1 (SDA47/SCL48), which is the same bus the
  // CST820 touch driver uses, and touch reads nothing until Mic.end().
  cfg.internal_mic = true;
  cfg.output_power = false;  // 450mAh pack: leave the Grove 5V boost off
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  pinMode(kPinHome, INPUT_PULLUP);
  pinMode(kPinNext, INPUT_PULLUP);
  M5.BtnA.setHoldThresh(800);
  M5.BtnB.setHoldThresh(800);

  applyTouchAffine();
  if (M5.Mic.isEnabled()) {
    auto mic = M5.Mic.config();
    mic.noise_filter_level = 4;
    M5.Mic.config(mic);
  }
  vibeInit();
  M5.Display.fillScreen(0);

  const int id = (int)M5.getBoard();
  Serial.printf("[board] id=%d panel=%dx%d touch=%d imu=%d spk=%d mic=%d psram=%uKB\n",
                id, M5.Display.width(), M5.Display.height(), (int)gTouch,
                (int)M5.Imu.isEnabled(), (int)M5.Speaker.isEnabled(),
                (int)M5.Mic.isEnabled(), (unsigned)(ESP.getFreePsram() / 1024));
  if (M5.getBoard() != m5::board_t::board_M5StopWatch) {
    Serial.println("[board] not detected as M5StopWatch; panel power may stay off");
  }
  return M5.Display.width() > 0;
}

void poll() {
  M5.update();
  if (gVibeOffAt && (int32_t)(millis() - gVibeOffAt) >= 0) {
    vibeDuty(0, false);
    gVibeOffAt = 0;
  }
}

Keys keys() {
  Keys k{};
  // Library BtnB is GPIO1, BtnA is GPIO2. See kPinHome.
  k.homeDown = M5.BtnB.wasPressed();
  k.nextDown = M5.BtnA.wasPressed();
  k.homeLong = M5.BtnB.wasHold();
  k.nextLong = M5.BtnA.wasHold();
  k.homeShort = M5.BtnB.wasClicked();
  k.nextShort = M5.BtnA.wasClicked();
  return k;
}

bool touchOk() { return gTouch; }
bool imuOk() { return M5.Imu.isEnabled(); }
bool speakerOk() { return M5.Speaker.isEnabled(); }
bool micConfigured() { return M5.Mic.isEnabled(); }
int boardId() { return (int)M5.getBoard(); }
int panelWidth() { return M5.Display.width(); }
int panelHeight() { return M5.Display.height(); }

bool readTouch(int16_t &x, int16_t &y) {
  if (!gTouch) return false;
  auto d = M5.Touch.getDetail();
  if (!d.isPressed()) return false;
  int tx = (int)d.x;
  int ty = (int)d.y;
  if (tx < 0) tx = 0;
  if (ty < 0) ty = 0;
  if (tx >= kWidth) tx = kWidth - 1;
  if (ty >= kHeight) ty = kHeight - 1;
  x = (int16_t)tx;
  y = (int16_t)ty;
  return true;
}

void setBrightness(uint8_t value) { M5.Display.setBrightness(value); }
void sleepDisplay() { M5.Display.setBrightness(0); }
void wakeDisplay(uint8_t brightness) { M5.Display.setBrightness(brightness); }

void setVolumePercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  M5.Speaker.setVolume((uint8_t)((uint16_t)percent * 255 / 100));
}

bool playWav(const uint8_t *data, size_t len) {
  if (!data || !len || !M5.Speaker.isEnabled()) return false;
  return M5.Speaker.playWav(data, len);
}

bool playRaw(const int16_t *pcm, size_t samples, uint32_t rate) {
  if (!pcm || !samples || !M5.Speaker.isEnabled()) return false;
  return M5.Speaker.playRaw(pcm, samples, rate, false, 1, 0);
}

bool speakerBusy() { return M5.Speaker.isPlaying(); }
void speakerStop() { M5.Speaker.stop(); }

bool micBegin() {
  if (!M5.Mic.isEnabled()) return false;
  speakerStop();
  return M5.Mic.begin();
}

void micEnd() { M5.Mic.end(); }

bool micRecord(int16_t *dst, size_t samples, uint32_t rate) {
  return M5.Mic.record(dst, samples, rate);
}

bool readAccel(float &x, float &y, float &z) {
  if (!M5.Imu.isEnabled()) return false;
  return M5.Imu.getAccel(&x, &y, &z);
}

void vibrate(uint16_t ms, uint8_t strength) {
  if (!gVibe || !ms || !strength) return;
  if (strength > 100) strength = 100;
  uint16_t duty = (uint16_t)(1024 + (uint32_t)strength * (4095 - 1024) / 100);
  vibeDuty(duty, true);
  gVibeOffAt = millis() + ms;
}

void push(uint16_t *fb) {
  if (!fb) return;
  maskRound(fb);
  // The CO5300 framebuffer is rgb565_2Byte: QSPI sends the low byte first,
  // and that byte is RRRRRGGG. pushImage(uint16_t*) with swapBytes left at
  // its default false treats the buffer as already swapped and copies it
  // straight through, so native RGB565 (red = 0xF800) is read as 0x00F8
  // and a red die comes out green-cyan. swapBytes true converts on the way
  // in. This is byte order, not an R/B channel swap — the panel MADCTL has
  // no BGR bit and stays 0x00.
  const bool prevSwap = M5.Display.getSwapBytes();
  M5.Display.setSwapBytes(true);
  constexpr int kBand = 32;
  M5.Display.startWrite();
  for (int y = 0; y < kHeight; y += kBand) {
    int h = (y + kBand <= kHeight) ? kBand : (kHeight - y);
    M5.Display.pushImage(0, y, kWidth, h, fb + (size_t)y * kWidth);
  }
  M5.Display.endWrite();
  M5.Display.setSwapBytes(prevSwap);
}

}  // namespace board

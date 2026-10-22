#pragma once
#include <stddef.h>
#include <stdint.h>

// Compile-time mirrors. Default 0 matches the measured affine below.
// Pass -D TOUCH_MIRROR_X=1 / TOUCH_MIRROR_Y=1 if a unit is mounted backwards.
#ifndef TOUCH_MIRROR_X
#define TOUCH_MIRROR_X 0
#endif
#ifndef TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y 0
#endif

// StopWatch hardware behind M5Unified / M5GFX.
// The app never talks to the IO expander, codec, or panel directly.
namespace board {

constexpr int kWidth = 466;
constexpr int kHeight = 466;

// This firmware's on-device mapping: GPIO1 short = home, GPIO2 short = next.
// M5Unified, the official demo, and the product sheet call GPIO2 Button A
// (KEYA, yellow) and GPIO1 Button B (KEYB, blue). If the physical keys feel
// reversed, swap the two actions in keys().
constexpr int kPinHome = 1;
constexpr int kPinNext = 2;

struct Keys {
  bool homeShort;
  bool nextShort;
  bool homeLong;
  bool nextLong;
  bool homeDown;
  bool nextDown;
};

bool begin();
void poll();
Keys keys();

bool touchOk();
bool imuOk();
bool speakerOk();
bool micConfigured();
int boardId();
int panelWidth();
int panelHeight();

bool readTouch(int16_t &x, int16_t &y);

void setBrightness(uint8_t value);
void sleepDisplay();
void wakeDisplay(uint8_t brightness);

void setVolumePercent(uint8_t percent);
bool playWav(const uint8_t *data, size_t len);
bool playRaw(const int16_t *pcm, size_t samples, uint32_t rate);
bool speakerBusy();
void speakerStop();

bool micBegin();
void micEnd();
bool micRecord(int16_t *dst, size_t samples, uint32_t rate);

bool readAccel(float &x, float &y, float &z);
void vibrate(uint16_t ms, uint8_t strength = 60);

// Clears pixels outside the round bezel, then pushes the 466 framebuffer.
void push(uint16_t *fb);

}  // namespace board

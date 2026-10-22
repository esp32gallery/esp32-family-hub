#include "dice_sdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int count_colors(const uint16_t *buf, int n, int *red, int *white) {
  int filled = 0;
  *red = 0;
  *white = 0;
  for (int i = 0; i < n; ++i) {
    uint16_t c = buf[i];
    if (!c) continue;
    ++filled;
    int r = (c >> 11) & 31;
    int g = (c >> 5) & 63;
    int b = c & 31;
    if (r > 16 && r > g / 2 && r > b + 2) ++*red;
    if (r > 20 && g > 40 && b > 16) ++*white;
  }
  return filled;
}

int main() {
  const int N = 160;
  uint16_t *buf = (uint16_t *)calloc((size_t)N * N, sizeof(uint16_t));
  if (!buf) return 2;
  dice::Renderer r;
  r.W = r.H = N;
  r.buf = buf;
  r.p.focal = 1600.0f;
  r.p.camZ = 4.2f;

  int facesWithPips = 0;
  for (int v = 1; v <= 6; ++v) {
    r.renderFast(dice::targetForValue(v, 0.1f), 0);
    int red = 0, white = 0;
    int filled = count_colors(buf, N * N, &red, &white);
    printf("value=%d filled=%d red=%d white=%d bbox=%d,%d-%d,%d\n", v, filled, red, white,
           r.bboxX0, r.bboxY0, r.bboxX1, r.bboxY1);
    if (red > 400 && white > 8) ++facesWithPips;
  }

  const int big = 466;
  uint16_t *full = (uint16_t *)calloc((size_t)big * big, sizeof(uint16_t));
  if (!full) return 2;
  r.W = r.H = big;
  r.buf = full;
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  r.renderFast(dice::targetForValue(5, 0.1f), 0.2f);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
  int red = 0, white = 0;
  int filled = count_colors(full, big * big, &red, &white);
  int minx = big, miny = big, maxx = 0, maxy = 0;
  for (int y = 0; y < big; ++y) {
    for (int x = 0; x < big; ++x) {
      if (!full[y * big + x]) continue;
      if (x < minx) minx = x;
      if (y < miny) miny = y;
      if (x > maxx) maxx = x;
      if (y > maxy) maxy = y;
    }
  }
  printf("466 filled=%d red=%d white=%d ms=%.2f tight=%d,%d-%d,%d\n", filled, red, white, ms,
         minx, miny, maxx, maxy);

  int outside = 0;
  const int cx = big / 2, rad = big / 2 - 2, r2 = rad * rad;
  for (int y = 0; y < big; ++y) {
    for (int x = 0; x < big; ++x) {
      int dx = x - cx, dy = y - cx;
      if (dx * dx + dy * dy > r2 && full[y * big + x]) ++outside;
    }
  }
  const int S = 120;
  uint16_t *small = (uint16_t *)calloc((size_t)S * S, sizeof(uint16_t));
  r.W = r.H = S;
  r.buf = small;
  r.p.focal = 1600.0f;
  dice::Quat q = dice::targetForValue(1, 0.1f);
  r.render(q, 0);
  int sminx = S, sminy = S, smaxx = 0, smaxy = 0, sfill = 0;
  for (int i = 0; i < S * S; ++i) {
    if (!small[i]) continue;
    ++sfill;
    int x = i % S, y = i / S;
    if (x < sminx) sminx = x;
    if (y < sminy) sminy = y;
    if (x > smaxx) smaxx = x;
    if (y > smaxy) smaxy = y;
  }
  printf("sdf120 fill=%d tight=%d,%d-%d,%d (%dx%d)\n", sfill, sminx, sminy, smaxx, smaxy,
         smaxx - sminx + 1, smaxy - sminy + 1);
  memset(small, 0, (size_t)S * S * sizeof(uint16_t));
  r.renderFast(q, 0);
  sminx = S; sminy = S; smaxx = 0; smaxy = 0; sfill = 0;
  for (int i = 0; i < S * S; ++i) {
    if (!small[i]) continue;
    ++sfill;
    int x = i % S, y = i / S;
    if (x < sminx) sminx = x;
    if (y < sminy) sminy = y;
    if (x > smaxx) smaxx = x;
    if (y > smaxy) smaxy = y;
  }
  printf("fast120 fill=%d tight=%d,%d-%d,%d (%dx%d)\n", sfill, sminx, sminy, smaxx, smaxy,
         smaxx - sminx + 1, smaxy - sminy + 1);
  printf("outside-circle=%d faces=%d\n", outside, facesWithPips);
  // Host is much faster than the ESP32. A multi-second frame means the SDF
  // path leaked back in. A few hundred milliseconds is still fine here.
  if (facesWithPips < 6 || red < 1000 || white < 20 || ms > 800.0) return 1;
  return 0;
}

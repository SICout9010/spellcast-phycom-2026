// spell_dtw.h: turns a wand gesture into a spell.
// 1-nearest-neighbour over Dynamic Time Warping (DTW) distance on 3-axis gyro data.
// Plain C++ with no Arduino calls, so it runs unchanged on the UNO R4, an ESP32,
// or a PC (test/test_dtw.cpp compiles this exact file).
#pragma once
#include <math.h>
#include <float.h>

const int SEQ_LEN = 32;  // every cast is resampled to this many points
const int AXES = 3;      // gyro x, y, z in deg/s
const int DTW_BAND = 8;  // how far DTW may stretch time, in points

struct Seq { float v[SEQ_LEN][AXES]; };
struct Template { Seq s; int spell; };  // spell < 0 = empty slot

// Turn `len` raw gyro samples into the shape of the line the wand drew:
// 1. add up rotation speed into a path (how far the wand has turned on each axis),
// 2. take SEQ_LEN points spaced evenly along that path, so speed, pauses and hand tremor stop mattering,
// 3. centre it and scale it to unit size, so big and small casts look alike.
inline void prepare(const float raw[][AXES], int len, Seq &out) {
  float total = 0;
  for (int i = 0; i < len; i++) total += sqrtf(raw[i][0] * raw[i][0] + raw[i][1] * raw[i][1] + raw[i][2] * raw[i][2]);
  float spacing = total / (SEQ_LEN - 1), pos[AXES] = {0, 0, 0}, walked = 0;
  int k = 0;
  for (int i = 0; i < len && k < SEQ_LEN; i++) {
    float step = sqrtf(raw[i][0] * raw[i][0] + raw[i][1] * raw[i][1] + raw[i][2] * raw[i][2]);
    while (k < SEQ_LEN && k * spacing <= walked + step) {
      float f = step > 0 ? (k * spacing - walked) / step : 0;
      for (int a = 0; a < AXES; a++) out.v[k][a] = pos[a] + f * raw[i][a];
      k++;
    }
    for (int a = 0; a < AXES; a++) pos[a] += raw[i][a];
    walked += step;
  }
  for (; k < SEQ_LEN; k++)  // rounding leftovers sit at the end of the path
    for (int a = 0; a < AXES; a++) out.v[k][a] = pos[a];

  float mean[AXES] = {0, 0, 0}, ss = 0;
  for (int i = 0; i < SEQ_LEN; i++)
    for (int a = 0; a < AXES; a++) mean[a] += out.v[i][a] / SEQ_LEN;
  for (int i = 0; i < SEQ_LEN; i++)
    for (int a = 0; a < AXES; a++) { out.v[i][a] -= mean[a]; ss += out.v[i][a] * out.v[i][a]; }
  float rms = sqrtf(ss / SEQ_LEN);
  if (rms < 1e-6f) rms = 1;
  for (int i = 0; i < SEQ_LEN; i++)
    for (int a = 0; a < AXES; a++) out.v[i][a] /= rms;
}

// Average per-step distance along the best time alignment of a and b.
// Keeps two rows only: ~260 bytes of stack.
inline float dtw(const Seq &a, const Seq &b) {
  float prev[SEQ_LEN], cur[SEQ_LEN];
  for (int i = 0; i < SEQ_LEN; i++) {
    for (int j = 0; j < SEQ_LEN; j++) {
      cur[j] = FLT_MAX;
      if (j < i - DTW_BAND || j > i + DTW_BAND) continue;
      float d = 0;
      for (int k = 0; k < AXES; k++) { float e = a.v[i][k] - b.v[j][k]; d += e * e; }
      float best = (i == 0 && j == 0) ? 0 : FLT_MAX;
      if (i > 0) best = fminf(best, prev[j]);
      if (j > 0) best = fminf(best, cur[j - 1]);
      if (i > 0 && j > 0) best = fminf(best, prev[j - 1]);
      cur[j] = sqrtf(d) + best;
    }
    for (int j = 0; j < SEQ_LEN; j++) prev[j] = cur[j];
  }
  return prev[SEQ_LEN - 1] / SEQ_LEN;
}

// Spell of the nearest template (-1 if there are none); its distance goes in *dist.
inline int classify(const Seq &x, const Template *t, int n, float *dist) {
  int best = -1;
  *dist = FLT_MAX;
  for (int i = 0; i < n; i++) {
    if (t[i].spell < 0) continue;
    float d = dtw(x, t[i].s);
    if (d < *dist) { *dist = d; best = t[i].spell; }
  }
  return best;
}

// 0-100 "how clean was that cast" for the leaderboard: 100 at `perfect` distance or closer, 0 at `reject`.
inline int castScore(float dist, float perfect, float reject) {
  float s = 100 * (reject - dist) / (reject - perfect);
  return s < 0 ? 0 : s > 100 ? 100 : (int)(s + 0.5f);
}

// ---- Wand axes ----
// Sensor axes -> wand axes, for the wand held flat with the GY-521 chip side up:
//   w[0] roll  = twisting the stick
//   w[1] pitch = tip moving up (+) / down
//   w[2] yaw   = tip moving left (+) / right
// `along` = the sensor axis that runs along the stick (0 = X, 1 = Y); `sign` = +1 if it points to the tip.
inline void wandAxes(const float *g, int along, int sign, float *w) {
  w[0] = sign * g[along];
  w[1] = along == 1 ? sign * g[0] : -sign * g[1];  // rotation about the axis pointing to the wand's right
  w[2] = g[2];                                      // rotation about up (Z, out of the chip)
}

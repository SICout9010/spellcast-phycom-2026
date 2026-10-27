// test_dtw.cpp: runs the board's classifier (spell_dtw.h) on simulated wand casts.
// Each cast gets a random speed, size, tilt, speed profile, wrist roll and sensor noise.
// Build and run:  c++ -O2 -std=c++17 test_dtw.cpp -o test_dtw && ./test_dtw
#include "../firmware/spellcast/spell_dtw.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

struct P { float x, y; };  // wand-tip position, screen style (y grows downward)
const char *NAMES[] = {"LUMOS", "NOX", "LEVIOSA", "EXPELLIARMUS", "ACCIO"};
const int SPELLS = 5, PER_SPELL = 3, TRIALS = 200;
const float PI_F = 3.14159265f;
const float REJECT = 0.5f, PERFECT = 0.1f;  // keep in sync with spellcast.ino and sim/index.html

std::vector<P> shape(int s) {
  std::vector<P> p;
  switch (s) {
    case 0: for (int i = 0; i <= 24; i++) { float a = 2 * PI_F * i / 24; p.push_back({sinf(a), -cosf(a)}); } break;   // circle, clockwise from the top
    case 1: for (int i = 0; i <= 24; i++) { float a = 2 * PI_F * i / 24; p.push_back({-sinf(a), -cosf(a)}); } break;  // circle, anticlockwise
    case 2: for (int i = 0; i <= 12; i++) { float x = -1 + 2.0f * i / 12; p.push_back({x, -0.35f * sinf(PI_F * (x + 1))}); }
            p.push_back({1.15f, 0.9f}); break;                                                                          // swish, then flick down
    case 3: p = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}; break;                                                            // zigzag Z
    case 4: p = {{-1, -1}, {0, 1}, {1, -1}}; break;                                                                     // V
  }
  return p;
}

P along(const std::vector<P> &p, float u) {  // point at fraction u of the path's length
  std::vector<float> acc(1, 0);
  for (size_t i = 1; i < p.size(); i++) acc.push_back(acc.back() + hypotf(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y));
  float target = std::clamp(u, 0.0f, 1.0f) * acc.back();
  size_t i = 1;
  while (i < p.size() - 1 && acc[i] < target) i++;
  float f = (target - acc[i - 1]) / std::max(acc[i] - acc[i - 1], 1e-6f);
  return {p[i - 1].x + f * (p[i].x - p[i - 1].x), p[i - 1].y + f * (p[i].y - p[i - 1].y)};
}

std::mt19937 rng(7);
float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
float N01() { return std::normal_distribution<float>(0, 1)(rng); }

// Gyro trace of one cast. Tip velocity maps to rotation rate: sideways = yaw (z), up/down = pitch (y).
int simulateCast(const std::vector<P> &path, float raw[][AXES]) {
  int len = (int)U(40, 150);  // 0.4-1.5 s at 100 Hz
  float scale = U(0.6f, 1.6f), rot = U(-0.3f, 0.3f), warp = U(-0.6f, 0.6f), roll = U(-0.3f, 0.3f);
  std::vector<P> pos(len + 1);
  for (int i = 0; i <= len; i++) {
    float t = (float)i / len, u = t + warp * sinf(2 * PI_F * t) / (2 * PI_F);  // uneven speed, still moving forward
    P q = along(path, u);
    pos[i] = {scale * (q.x * cosf(rot) - q.y * sinf(rot)), scale * (q.x * sinf(rot) + q.y * cosf(rot))};
  }
  float ss = 0;
  for (int i = 0; i < len; i++) {
    float vx = (pos[i + 1].x - pos[i].x) * 100, vy = (pos[i + 1].y - pos[i].y) * 100;
    raw[i][2] = -vx * 60;
    raw[i][1] = -vy * 60;
    raw[i][0] = roll * raw[i][2];  // wrists twist a little while drawing
    for (int k = 0; k < AXES; k++) ss += raw[i][k] * raw[i][k];
  }
  float noise = 0.15f * sqrtf(ss / len);
  for (int i = 0; i < len; i++)
    for (int k = 0; k < AXES; k++) raw[i][k] += noise * N01();
  return len;
}

float pct(std::vector<float> v, float q) { std::sort(v.begin(), v.end()); return v[(size_t)(q * (v.size() - 1))]; }

int main() {
  static float raw[250][AXES];
  Template tpl[SPELLS * PER_SPELL];
  for (int s = 0; s < SPELLS; s++)  // "training": 3 casts per spell, like pressing TRAIN on the box
    for (int k = 0; k < PER_SPELL; k++) {
      Template &t = tpl[s * PER_SPELL + k];
      prepare(raw, simulateCast(shape(s), raw), t.s);
      t.spell = s;
    }

  int confusion[SPELLS][SPELLS] = {}, correct = 0;
  std::vector<float> hit, junk;
  for (int s = 0; s < SPELLS; s++)
    for (int n = 0; n < TRIALS; n++) {
      Seq x; float d;
      prepare(raw, simulateCast(shape(s), raw), x);
      int got = classify(x, tpl, SPELLS * PER_SPELL, &d);
      confusion[s][got]++;
      if (got == s) { correct++; hit.push_back(d); }
    }
  for (int n = 0; n < 500; n++) {  // random scribbles should fizzle, not cast
    std::vector<P> scribble;
    for (int k = 0, m = 3 + n % 4; k < m; k++) scribble.push_back({U(-1, 1), U(-1, 1)});
    Seq x; float d;
    prepare(raw, simulateCast(scribble, raw), x);
    classify(x, tpl, SPELLS * PER_SPELL, &d);
    junk.push_back(d);
  }

  printf("%-13s", "cast \\ got");
  for (int g = 0; g < SPELLS; g++) printf("%7.5s", NAMES[g]);
  printf("\n");
  for (int s = 0; s < SPELLS; s++) {
    printf("%-13s", NAMES[s]);
    for (int g = 0; g < SPELLS; g++) printf("%7d", confusion[s][g]);
    printf("\n");
  }
  float acc = (float)correct / (SPELLS * TRIALS);
  int hitsKept = (int)std::count_if(hit.begin(), hit.end(), [](float d) { return d <= REJECT; });
  int junkRejected = (int)std::count_if(junk.begin(), junk.end(), [](float d) { return d > REJECT; });
  printf("\naccuracy %.1f%%  (%d casts)\n", 100 * acc, SPELLS * TRIALS);
  printf("correct-cast distance: median %.2f, 95th pct %.2f\n", pct(hit, 0.5f), pct(hit, 0.95f));
  printf("scribble distance:     5th pct %.2f, median %.2f\n", pct(junk, 0.05f), pct(junk, 0.5f));
  printf("REJECT %.2f keeps %.1f%% of correct casts, fizzles %.1f%% of scribbles\n", REJECT,
         100.0f * hitsKept / hit.size(), 100.0f * junkRejected / junk.size());
  std::vector<int> scores; for (float d : hit) scores.push_back(castScore(d, PERFECT, REJECT));
  std::sort(scores.begin(), scores.end());
  printf("score of correct casts: median %d, 5th pct %d\n", scores[scores.size() / 2], scores[scores.size() / 20]);
  printf("per cast on the board: %d DTW cells\n", SPELLS * PER_SPELL * SEQ_LEN * (2 * DTW_BAND + 1));

  // Wand axes: the same physical move must come out the same for every way the GY-521 can sit on the stick.
  // Frame: F = toward the tip, U = up (out of the chip), R = F x U. Tip up = rotation about +R, tip left = about +U.
  int axisErrors = 0;
  for (int along = 0; along < 2; along++)
    for (int sign = -1; sign <= 1; sign += 2) {
      float F[3] = {0, 0, 0}, U[3] = {0, 0, 1};
      F[along] = (float)sign;
      float R[3] = {F[1] * U[2] - F[2] * U[1], F[2] * U[0] - F[0] * U[2], F[0] * U[1] - F[1] * U[0]};
      float w[3], tipUp[3] = {100 * R[0], 100 * R[1], 100 * R[2]}, tipLeft[3] = {0, 0, 100}, twist[3] = {100 * F[0], 100 * F[1], 100 * F[2]};
      wandAxes(tipUp, along, sign, w);
      axisErrors += !(w[1] > 99 && fabsf(w[0]) < 1 && fabsf(w[2]) < 1);
      wandAxes(tipLeft, along, sign, w);
      axisErrors += !(w[2] > 99 && fabsf(w[0]) < 1 && fabsf(w[1]) < 1);
      wandAxes(twist, along, sign, w);
      axisErrors += !(fabsf(w[0]) > 99 && fabsf(w[1]) < 1 && fabsf(w[2]) < 1);
    }
  printf("wand axes: %d errors over 4 mountings x 3 moves\n", axisErrors);

  if (acc < 0.9f || axisErrors) { printf("FAIL\n"); return 1; }
  printf("PASS\n");
  return 0;
}

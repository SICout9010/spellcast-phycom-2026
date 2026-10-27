// Spellcast: cast spells with a wand on an Arduino UNO R4 WiFi.
// The gesture AI (spell_dtw.h) runs on this board. Over WiFi it streams the wand's motion live to
// server/server.py (UDP, for the web page) and posts each cast's result (HTTP, for the leaderboard).
//
// Libraries (Library Manager): ArduinoGraphics, Keyboard (only if USE_HID is 1).
// WiFi name/password go in arduino_secrets.h, which is git-ignored.
//
// Wiring
//   GY-521 (MPU6050) at the wand tip: VCC->5V, GND->GND, SDA->A4, SCL->A5.
//   Button:                    D12 -> button -> GND
//   Passive buzzer (optional): D8 -> buzzer -> GND. Without it everything works, just silently.
//   Light: the built-in "L" LED and the LED matrix, so nothing else to wire.
//
// Holding the wand
//   Made for holding the stick flat, pointing forward, with the board and the GY-521 chip facing up.
//   Boot it once standing tip-up and still (like a pen in a cup): gravity tells the board which way the GY-521
//   sits on the stick, and it remembers. After that it can boot lying flat; just keep it still for 1 s.
//
// Use
//   Cast: hold the button, draw the spell with the tip, release.
//   Tap the button: reset (light off, READY). While training, a tap skips to the next spell.
//   Hold the button 2 s without moving: start training (the matrix asks for each spell 3 times), or stop.
//   Triple-click: mouse mode on/off. In mouse mode the wand moves the laptop's pointer (via server.py):
//   click = left click, double-click = double click, click then press-and-hold = right click.
//   LUMOS = circle clockwise from the top, NOX = circle anticlockwise, LEVIOSA = swish then flick down,
//   EXPELLIARMUS = zigzag Z, ACCIO = V.

#include <Wire.h>
#include <EEPROM.h>
#include <WiFiS3.h>
#include "ArduinoGraphics.h"  // must come before Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"
#include "arduino_secrets.h"
#include "spell_dtw.h"

#define USE_HID 0  // 1: ACCIO presses the right arrow (next slide). HID moves USB off the bridge chip,
                   // so the Serial Monitor goes quiet and uploads need a double-tap on RESET.
#if USE_HID
#include <Keyboard.h>
#endif

// Calibration knobs: the Serial Monitor prints every cast's distances, tune these against real casts.
const float REJECT = 0.5f;       // farther than this from every template = fizzle (raise it if real spells fizzle)
const float PERFECT = 0.1f;      // this close or closer scores 100
const float ROLL_WEIGHT = 0.3f;  // how much twisting the stick counts; drawing is about where the tip goes
const float STILL_DEG = 40;      // a training hold turns the wand less than this in total (after the press jolt)
const float DRAW_DEG = 45;       // a cast turns the wand at least this much in total; less = just a press
const float REST_STD = 1.0f;     // gyro spread (deg/s) when the wand rests: below this, re-measure the drift
const float BOOT_STD = 5.0f;     // boot calibration accepts a hand-held wand up to this much shake

const char *SERVER_IP = "10.174.19.15";  // laptop running server/server.py (it prints its address)
const int SERVER_PORT = 8050;            // results go over TCP, the live stream over UDP, same number

const int BTN = 12, BUZZ = 8;
const unsigned long HOLD_MS = 2000, TAP_MS = 250, STREAM_MS = 80, MOUSE_STREAM_MS = 20;
const unsigned long MULTI_MS = 350;  // the most time between the clicks of a double or triple click
const int RATE_HZ = 100, MAX_RAW = 300, MIN_CAST = 25;  // a cast is 0.25-3 s at 100 Hz
const uint8_t IMU_ADDR = 0x68;  // GY-521 I2C address ("MPU" is taken by the R4's own memory-protection unit)

const char *SPELLS[] = {"LUMOS", "NOX", "LEVIOSA", "EXPELLIARMUS", "ACCIO"};
const int SPELL_COUNT = 5, PER_SPELL = 3, SLOTS = SPELL_COUNT * PER_SPELL;
const uint32_t MAGIC = 0x5CA57003;  // marks EEPROM as holding templates in the current format (bump when it changes)

struct Orient { uint32_t magic; int8_t along, sign; };  // how the GY-521 sits on the stick, learned from gravity
const uint32_t ORIENT_MAGIC = 0x0A1E0001;
const int ORIENT_ADDR = 8100;  // after the templates (4 + 15 * 388 bytes) in the 8 KB EEPROM

Template tpl[SLOTS];
float raw[MAX_RAW][AXES], bias[AXES];
int along = 1, alongSign = 1;  // the GY-521 axis along the stick (0 = X, 1 = Y), +1 if it points to the tip
int teach = -1, taught = 0;    // spell being taught (-1 = casting), samples taken so far
bool lightOn = false;          // LUMOS / NOX state, shown on the L LED and the matrix
bool online = false;
bool imuOk = false;            // the GY-521 is answering and configured
bool recording = false;        // the button is held: a cast is being drawn
bool mouseMode = false;        // the wand moves the laptop's pointer instead of casting
unsigned long lastCheck = 0;
float restSum[AXES], restSq[AXES];  // 1 s window of idle samples, for re-measuring the gyro drift
int restN = 0;
ArduinoLEDMatrix matrix;
WiFiUDP udp;
int16_t liveBuf[16][AXES];  // samples waiting to be streamed
int liveN = 0;
unsigned long lastSend = 0;
unsigned long statSince = 0, statPackets = 0, statSamples = 0;  // mouse-mode stream health, printed each second
int fifoPeak = 0;

// 12x8 icons for the built-in matrix (same as sim/index.html)
const char *const ICON_LUMOS[8] = {"....####....", "...######...", "..########..", "..########..",
                                   "...######...", "....####....", "....####....", ".....##....."};  // light bulb
const char *const ICON_NOX[8] = {".....###....", "...###......", "..###.......", "..##........",
                                 "..##........", "..###.......", "...###......", ".....###...."};  // crescent moon
const char *const ICON_UP[8] = {".....##.....", "....####....", "...######...", "..##.##.##..",
                                ".....##.....", ".....##.....", ".....##.....", ".....##....."};
const char *const ICON_BOLT[8] = {".......##...", "......##....", ".....##.....", "....######..",
                                  "......##....", ".....##.....", "....##......", "...##......."};
const char *const ICON_RIGHT[8] = {".......#....", ".......##...", ".#########..", ".##########.",
                                   ".#########..", ".......##...", ".......#....", "............"};
const char *const ICON_REC[8] = {"############", "#..........#", "#..........#", "#..........#",
                                 "#..........#", "#..........#", "#..........#", "############"};
const char *const ICON_MOUSE[8] = {"...#........", "...##.......", "...###......", "...####.....",
                                   "...#####....", "...###......", "...#..##....", "........#..."};
const char *const ICON_LOST[8] = {"............", "..##....##..", "...##..##...", "....####....",
                                  "....####....", "...##..##...", "..##....##..", "............"};

void show(const char *msg) {
  matrix.beginDraw();
  matrix.stroke(0xFFFFFFFF);
  matrix.textScrollSpeed(45);
  matrix.textFont(Font_5x7);
  matrix.beginText(0, 1, 0xFFFFFF);
  matrix.println(msg);
  matrix.endText(SCROLL_LEFT);
  matrix.endDraw();
}

void fillMatrix(uint8_t on) {
  uint8_t frame[8][12];
  memset(frame, on, sizeof frame);
  matrix.renderBitmap(frame, 8, 12);
}

void drawIcon(const char *const icon[8], int dx, int dy) {  // icon shifted by dx columns, dy rows
  uint8_t frame[8][12];
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 12; c++) {
      int rr = r - dy, cc = c - dx;
      frame[r][c] = rr >= 0 && rr < 8 && cc >= 0 && cc < 12 && icon[rr][cc] == '#';
    }
  matrix.renderBitmap(frame, 8, 12);
}

bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool mpuRead(uint8_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(IMU_ADDR, (size_t)n) != (size_t)n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

float word16(const uint8_t *b) { return (int16_t)(b[0] << 8 | b[1]); }

void toDps(const uint8_t *b, float *g) {
  for (int k = 0; k < AXES; k++) g[k] = word16(b + 2 * k) / 32.8f;  // +-1000 deg/s range
}

void fifoReset() {
  mpuWrite(0x6A, 0x04);  // USER_CTRL: clear the FIFO
  mpuWrite(0x6A, 0x40);  // USER_CTRL: FIFO on
}

// The gyro reads a small false turn ("drift") even when still, and it changes with temperature.
// Whenever the wand has rested for 1 s (spread under REST_STD), re-measure it, so a bad boot reading fixes itself.
void trackRest(const float *g) {
  for (int k = 0; k < AXES; k++) { restSum[k] += g[k]; restSq[k] += g[k] * g[k]; }
  if (++restN < RATE_HZ) return;
  float worst = 0, mean[AXES];
  for (int k = 0; k < AXES; k++) {
    mean[k] = restSum[k] / restN;
    worst = fmaxf(worst, sqrtf(fmaxf(0, restSq[k] / restN - mean[k] * mean[k])));
  }
  if (worst < REST_STD) {
    float moved = 0;
    for (int k = 0; k < AXES; k++) { moved = fmaxf(moved, fabsf(mean[k] - bias[k])); bias[k] = mean[k]; }
    if (moved > 1) {
      Serial.print("Gyro drift re-measured, it was off by ");
      Serial.print(moved);
      Serial.println(" deg/s");
    }
  }
  restN = 0;
  for (int k = 0; k < AXES; k++) restSum[k] = restSq[k] = 0;
}

// Next 100 Hz sample from the MPU6050's own FIFO, in wand axes (see wandAxes in spell_dtw.h),
// so WiFi and display delays never drop samples. `idle` = nobody is drawing, so it may re-measure the drift.
bool nextSample(float *w, bool idle) {
  uint8_t c[2], b[6];
  if (!mpuRead(0x72, c, 2)) { imuOk = false; return false; }  // FIFO_COUNT
  int count = c[0] << 8 | c[1];
  if (count / 6 > fifoPeak) fifoPeak = count / 6;  // samples waiting: should stay small
  if (count >= 1020) { fifoReset(); return false; }  // overflowed (1 KB): start fresh
  if (count < 6) return false;
  if (!mpuRead(0x74, b, 6)) { imuOk = false; return false; }  // FIFO_R_W
  float g[AXES];
  toDps(b, g);
  if (idle) trackRest(g);
  for (int k = 0; k < AXES; k++) g[k] -= bias[k];
  wandAxes(g, along, alongSign, w);
  return true;
}

// If a glitch cut the GY-521 off mid-byte, it can hold SDA low and block the bus until power-off.
// Clocking SCL up to 9 times lets it finish that byte and let go.
void clearBus() {
  Wire.end();
  pinMode(A4, INPUT_PULLUP);  // SDA
  pinMode(A5, OUTPUT);        // SCL
  for (int i = 0; i < 9 && digitalRead(A4) == LOW; i++) {
    digitalWrite(A5, LOW);
    delayMicroseconds(5);
    digitalWrite(A5, HIGH);
    delayMicroseconds(5);
  }
  pinMode(A5, INPUT_PULLUP);
  Wire.begin();
}

// Set the GY-521 up. Also used after a power blip resets it (it wakes up asleep, with the FIFO off).
bool imuInit() {
  clearBus();
  Wire.beginTransmission(IMU_ADDR);
  if (Wire.endTransmission() != 0) return false;  // not answering
  bool ok = mpuWrite(0x6B, 0x00)      // wake up
            && mpuWrite(0x1A, 0x04)   // ~21 Hz low-pass: filters hand tremor (gyro still runs at 1 kHz)
            && mpuWrite(0x19, 9)      // sample rate: 1 kHz / (1 + 9) = 100 Hz
            && mpuWrite(0x1B, 0x10)   // gyro range +-1000 deg/s
            && mpuWrite(0x23, 0x70);  // FIFO collects gyro x, y, z
  if (ok) fifoReset();
  return ok;
}

bool imuHealthy() {  // still answering, and still configured (a reset turns the FIFO setting back to 0)
  uint8_t fifoEn;
  return mpuRead(0x23, &fifoEn, 1) && fifoEn == 0x70;
}

// Gravity (in g, wand still) -> which GY-521 axis runs along the stick. Standing tip-up, gravity runs along it.
void learnOrientation(const float *a) {
  Orient o;
  EEPROM.get(ORIENT_ADDR, o);
  if (fabsf(a[2]) < 0.5f && fmaxf(fabsf(a[0]), fabsf(a[1])) > 0.7f) {
    o.magic = ORIENT_MAGIC;
    o.along = fabsf(a[1]) > fabsf(a[0]) ? 1 : 0;
    o.sign = a[o.along] > 0 ? 1 : -1;  // the axis pointing up reads +1 g
    EEPROM.put(ORIENT_ADDR, o);
    Serial.print("Learned how the GY-521 sits: ");
  } else if (o.magic != ORIENT_MAGIC) {
    o.along = 1;  // never booted standing yet: guess Y points to the tip
    o.sign = 1;
    Serial.print("Guessing how the GY-521 sits (boot once standing tip-up to learn it): ");
  } else {
    Serial.print("GY-521 orientation from memory: ");
  }
  along = o.along;
  alongSign = o.sign;
  Serial.print(along ? "Y" : "X");
  Serial.println(alongSign > 0 ? " points to the tip" : " points to the handle");
}

void sendLive() {
  lastSend = millis();
  if (!online) { liveN = 0; return; }
  char buf[400];
  int n = snprintf(buf, sizeof buf, "{\"rec\":%d,\"teach\":%d,\"taught\":%d,\"light\":%d,\"lost\":%d,\"mouse\":%d,\"g\":[",
                   recording, teach, taught, lightOn, !imuOk, mouseMode);
  for (int i = 0; i < liveN; i++)
    for (int k = 0; k < AXES; k++) n += snprintf(buf + n, sizeof buf - n, "%s%d", i || k ? "," : "", liveBuf[i][k]);
  n += snprintf(buf + n, sizeof buf - n, "]}");
  udp.beginPacket(SERVER_IP, SERVER_PORT);
  udp.write((const uint8_t *)buf, n);
  udp.endPacket();
  statPackets++;
  statSamples += liveN;
  liveN = 0;
}

void streamSample(const float *w) {  // only collects; streamFlush() sends
  if (liveN < 16) {
    for (int k = 0; k < AXES; k++) liveBuf[liveN][k] = (int16_t)w[k];
    liveN++;
  }
}

// Send what's collected, at most once per call. One UDP packet takes ~25-40 ms (3 AT commands over the 115200-baud
// link to the WiFi chip), longer than 2-3 samples. So callers first read every sample that's waiting, then call this
// once: sending after each sample let the FIFO fall seconds behind and kept loop() from ever reading the button.
void streamFlush() {
  if (liveN && millis() - lastSend >= (mouseMode ? MOUSE_STREAM_MS : STREAM_MS)) sendLive();
}

// ---- Training backup: export to / import from server/training.json on the laptop ----

// Collects bytes and sends them 256 at a time (one WiFi write per byte would take ages), or with no
// client, just counts them, to fill in Content-Length before sending.
class Chunker : public Print {
 public:
  explicit Chunker(WiFiClient *client) : c(client) {}
  size_t total = 0;
  size_t write(uint8_t b) override {
    total++;
    if (!c) return 1;
    buf[n++] = b;
    if (n == sizeof buf) flush();
    return 1;
  }
  void flush() override {
    if (c && n) c->write(buf, n);
    n = 0;
  }

 private:
  WiFiClient *c;
  uint8_t buf[256];
  size_t n = 0;
};

void printTraining(Print &out) {  // {"format": MAGIC, "templates": [{"slot", "spell", "v": [96 numbers]}, ...]}
  out.print("{\"format\":");
  out.print((unsigned long)MAGIC);
  out.print(",\"templates\":[");
  bool first = true;
  for (int i = 0; i < SLOTS; i++) {
    if (tpl[i].spell < 0) continue;
    out.print(first ? "{\"slot\":" : ",{\"slot\":");
    first = false;
    out.print(i);
    out.print(",\"spell\":");
    out.print(tpl[i].spell);
    out.print(",\"v\":[");
    for (int j = 0; j < SEQ_LEN; j++)
      for (int a = 0; a < AXES; a++) {
        if (j || a) out.print(',');
        out.print(tpl[i].s.v[j][a], 4);
      }
    out.print("]}");
  }
  out.print("]}");
}

// Send every trained example to the server, which saves server/training.json (and a dated copy in backups/).
bool uploadTraining() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient c;
  if (!c.connect(SERVER_IP, SERVER_PORT)) return false;
  Chunker count(nullptr), out(&c);
  printTraining(count);
  c.print("POST /api/training HTTP/1.1\r\nHost: spellcast\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
  c.print(count.total);
  c.print("\r\n\r\n");
  printTraining(out);
  out.flush();
  c.setTimeout(3000);
  bool ok = c.find((char *)" 200 ");
  c.stop();
  Serial.println(ok ? "Training backed up to the laptop" : "Training backup failed");
  return ok;
}

bool loadTemplates() {  // from EEPROM; false if it holds nothing in the current format
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  for (int i = 0; i < SLOTS; i++) {
    if (magic == MAGIC) EEPROM.get(4 + i * sizeof(Template), tpl[i]);
    else tpl[i].spell = -1;
  }
  return magic == MAGIC;
}

// Download server/training.json (as plain numbers: format count / slot spell 96 numbers ... / count again)
// and save it to EEPROM. Anything wrong or cut short: keep what the wand had.
void importTraining() {
  WiFiClient c;
  bool ok = WiFi.status() == WL_CONNECTED && c.connect(SERVER_IP, SERVER_PORT);
  if (ok) {
    c.print("GET /api/training.txt HTTP/1.1\r\nHost: spellcast\r\nConnection: close\r\n\r\n");
    c.setTimeout(3000);
    ok = c.find((char *)" 200 ") && c.find((char *)"\r\n\r\n") && c.parseInt() == (long)MAGIC;
  }
  int n = ok ? c.parseInt() : 0;
  ok = ok && n > 0 && n <= SLOTS;
  if (ok) {
    for (int i = 0; i < SLOTS; i++) tpl[i].spell = -1;
    for (int i = 0; i < n && ok; i++) {
      int slot = c.parseInt(), spell = c.parseInt();
      ok = slot >= 0 && slot < SLOTS && spell >= 0 && spell < SPELL_COUNT;
      for (int j = 0; j < SEQ_LEN && ok; j++)
        for (int a = 0; a < AXES; a++) tpl[slot].s.v[j][a] = c.parseFloat();
      if (ok) tpl[slot].spell = spell;
    }
    ok = ok && c.parseInt() == n;  // the end marker: proves nothing was cut off
  }
  c.stop();
  if (!ok) {
    loadTemplates();  // back to what was in EEPROM
    Serial.println("Import failed: kept the wand's own training");
    show("LOAD FAILED");
  } else {
    for (int i = 0; i < SLOTS; i++) EEPROM.put(4 + i * sizeof(Template), tpl[i]);
    EEPROM.put(0, MAGIC);
    Serial.print("Imported ");
    Serial.print(n);
    Serial.println(" examples from the laptop");
    tone(BUZZ, 1500, 60);
    show("LOADED");
  }
  fillMatrix(lightOn);
}

void checkCommands() {  // the web page's buttons arrive as UDP commands from the server
  if (!online || udp.parsePacket() <= 0) return;
  char cmd[16] = {0};
  udp.read(cmd, sizeof cmd - 1);
  if (!strncmp(cmd, "EXPORT", 6)) uploadTraining();
  else if (!strncmp(cmd, "IMPORT", 6)) importTraining();
  fifoReset();  // drop what piled up meanwhile
}

void promptTeach() {
  if (teach >= SPELL_COUNT) teach = -1;
  if (teach < 0) { show("READY"); return; }
  char msg[24];
  snprintf(msg, sizeof msg, "%s %d/%d", SPELLS[teach], taught + 1, PER_SPELL);
  Serial.print("Teach: ");
  Serial.println(msg);
  show(msg);
}

void learn(const Seq &s) {
  int slot = teach * PER_SPELL + taught;
  tpl[slot].s = s;
  tpl[slot].spell = teach;
  EEPROM.put(4 + slot * sizeof(Template), tpl[slot]);  // survives power-off
  EEPROM.put(0, MAGIC);
  tone(BUZZ, 1500, 60);
  if (++taught == PER_SPELL) { taught = 0; teach++; }
  promptTeach();
  if (teach < 0) uploadTraining();  // just finished the last spell: back it up on the laptop
}

void act(int spell) {
  switch (spell) {
    case 0:  // light bulb, then the light comes on
      drawIcon(ICON_LUMOS, 0, 0);
      lightOn = true;
      delay(500);
      break;
    case 1:  // moon, then dark
      drawIcon(ICON_NOX, 0, 0);
      lightOn = false;
      delay(500);
      break;
    case 2:  // levitate: an arrow floats up the matrix twice
      for (int i = 0; i < 2; i++)
        for (int y = 8; y > -8; y--) { drawIcon(ICON_UP, 0, y); delay(45); }
      break;
    case 3:  // zap: the bolt flashes 3 times, with a falling tone if the buzzer is fitted
      for (int i = 0; i < 3; i++) {
        drawIcon(ICON_BOLT, 0, 0);
        for (int f = 2400; f > 300; f -= 140) { tone(BUZZ, f); delay(6); }
        noTone(BUZZ);
        fillMatrix(0);
        delay(80);
      }
      drawIcon(ICON_BOLT, 0, 0);
      delay(400);
      break;
    case 4:  // summon: an arrow slides in (and presses -> for the next slide if USE_HID is 1)
      for (int x = -12; x <= 0; x++) { drawIcon(ICON_RIGHT, x, 0); delay(35); }
#if USE_HID
      Keyboard.write(KEY_RIGHT_ARROW);
#endif
      delay(400);
      break;
  }
  digitalWrite(LED_BUILTIN, lightOn);
}

// Tell the laptop right away (UDP, ~30 ms; HTTP took 0.3-1 s), so its effects start before the wand's own show.
// Sent twice in case WiFi drops one; the server ignores the repeat by its id.
void report(const char *spell, int score, const float *best) {
  if (!online) return;  // offline: spells still work, the laptop just doesn't hear about them
  char buf[160];
  int n = snprintf(buf, sizeof buf, "{\"cast\":\"%s\",\"id\":%lu,\"score\":%d,\"d\":[", spell, millis(), score);
  for (int i = 0; i < SPELL_COUNT; i++)  // distance per spell in thousandths, -1 = not taught
    n += snprintf(buf + n, sizeof buf - n, "%s%d", i ? "," : "", best[i] < 99 ? (int)(best[i] * 1000) : -1);
  n += snprintf(buf + n, sizeof buf - n, "]}");
  for (int i = 0; i < 2; i++) {
    udp.beginPacket(SERVER_IP, SERVER_PORT);
    udp.write((const uint8_t *)buf, n);
    udp.endPacket();
  }
}

void handleCast(int n) {
  Seq s;
  prepare(raw, n, s);
  if (teach >= 0) { learn(s); return; }

  float best[SPELL_COUNT];
  for (int i = 0; i < SPELL_COUNT; i++) best[i] = FLT_MAX;
  for (int i = 0; i < SLOTS; i++)
    if (tpl[i].spell >= 0) best[tpl[i].spell] = fminf(best[tpl[i].spell], dtw(s, tpl[i].s));
  int spell = 0;
  for (int i = 1; i < SPELL_COUNT; i++)
    if (best[i] < best[spell]) spell = i;
  if (best[spell] == FLT_MAX) { show("HOLD 2s: TRAIN"); fillMatrix(lightOn); return; }

  for (int i = 0; i < SPELL_COUNT; i++) {  // every spell's distance, for tuning REJECT
    Serial.print(SPELLS[i]);
    Serial.print(' ');
    Serial.print(best[i] < 99 ? best[i] : -1);
    Serial.print(i < SPELL_COUNT - 1 ? " | " : "\n");
  }
  bool ok = best[spell] <= REJECT;
  int score = ok ? castScore(best[spell], PERFECT, REJECT) : 0;
  report(ok ? SPELLS[spell] : "FIZZLE", score, best);  // the laptop first: its effects start now
  if (ok) {
    act(spell);         // the spell's icon / effect,
    show(SPELLS[spell]);  // then its name (the score is on the web page)
  } else {
    tone(BUZZ, 180, 250);
    show("...");
  }
  fillMatrix(lightOn);  // after LUMOS the matrix stays lit like a lamp, until NOX
}

void onTap() {
  if (teach >= 0) {  // skip this spell
    teach++;
    taught = 0;
    promptTeach();
    if (teach < 0) uploadTraining();  // skipped past the last one: training is over, back it up
  } else {
    lightOn = false;
    digitalWrite(LED_BUILTIN, LOW);
    show("READY");
  }
  fillMatrix(lightOn);
}

void toggleTraining() {
  tone(BUZZ, 1200, 80);  // you can let go now
  if (teach < 0) { teach = 0; taught = 0; promptTeach(); }
  else {
    teach = -1;
    show("READY");
    uploadTraining();  // back up what was taught
  }
}

// ---- Clicks and mouse mode ----

// After a first short click, count the clicks that follow within MULTI_MS of each other (up to 3).
// If a press lasts longer than a click, stop there with longPress = true; the button is still down then.
int countTaps(bool &longPress) {
  int taps = 1;
  longPress = false;
  while (taps < 3) {
    unsigned long gap = millis();
    while (digitalRead(BTN) == HIGH)
      if (millis() - gap > MULTI_MS) return taps;
    delay(20);  // press bounce
    unsigned long down = millis();
    while (digitalRead(BTN) == LOW)
      if (millis() - down >= TAP_MS) { longPress = true; return taps; }
    delay(20);  // release bounce
    taps++;
  }
  return taps;
}

void sendClick(const char *kind) {  // server.py clicks the laptop's mouse
  if (!online) return;
  char buf[32];
  int n = snprintf(buf, sizeof buf, "{\"click\":\"%s\"}", kind);
  udp.beginPacket(SERVER_IP, SERVER_PORT);
  udp.write((const uint8_t *)buf, n);
  udp.endPacket();
  Serial.print("Mouse: ");
  Serial.println(kind);
}

void toggleMouse() {
  mouseMode = !mouseMode;
  tone(BUZZ, mouseMode ? 1600 : 900, 80);
  Serial.println(mouseMode ? "Mouse mode on" : "Mouse mode off");
  if (mouseMode) drawIcon(ICON_MOUSE, 0, 0);
  else fillMatrix(lightOn);
  sendLive();
}

// Mouse mode: click = left, double-click = double, click then press-and-hold = right, triple-click = leave.
// The pointer doesn't move while the button works: pressing jolts the stick, and that shouldn't aim the click.
void mouseButton() {
  unsigned long down = millis();
  delay(20);  // press bounce
  while (digitalRead(BTN) == LOW) {}
  delay(20);  // release bounce
  if (millis() - down < TAP_MS) {  // a long first press does nothing in mouse mode
    bool longPress;
    int taps = countTaps(longPress);
    if (taps == 3) toggleMouse();
    else if (longPress) {
      if (taps == 1) sendClick("right");
      while (digitalRead(BTN) == LOW) {}
      delay(20);
    } else sendClick(taps == 2 ? "double" : "left");
  }
  fifoReset();  // forget the jolt
}

// Hold = draw. Samples come from the FIFO at exactly 100 Hz until the button is released.
void castFromButton() {
  fifoReset();  // start from the moment the button went down
  restN = 0;    // a hand on the button isn't "resting"
  for (int k = 0; k < AXES; k++) restSum[k] = restSq[k] = 0;
  recording = true;
  drawIcon(ICON_REC, 0, 0);
  int len = 0;
  float turned = 0, turnedLate = 0;  // degrees the wand turned: in total, and after the press jolt
  bool toggled = false;
  unsigned long t0 = millis();
  while (digitalRead(BTN) == LOW && imuOk) {
    float w[AXES];
    if (!nextSample(w, false)) {  // caught up: now there's time to send
      streamFlush();
      continue;
    }
    streamSample(w);
    float step = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) / RATE_HZ;
    turned += step;
    if (millis() - t0 > 300) turnedLate += step;  // pressing the button jolts the stick for a moment
    if (len < MAX_RAW) {
      raw[len][0] = ROLL_WEIGHT * w[0];  // twisting the stick counts less than moving the tip
      raw[len][1] = w[1];
      raw[len][2] = w[2];
      len++;
    }
    if (!toggled && millis() - t0 >= HOLD_MS && turnedLate < STILL_DEG) { toggled = true; toggleTraining(); }
  }
  unsigned long held = millis() - t0;
  recording = false;
  sendLive();
  delay(20);  // let the button stop bouncing
  if (!imuOk || toggled) fillMatrix(lightOn);
  else if (held < TAP_MS) {
    bool longPress = false;
    int taps = held >= 30 ? countTaps(longPress) : 0;  // shorter than 30 ms is contact bounce
    if (taps == 3) toggleMouse();
    else if (taps == 1 && !longPress) onTap();
    else fillMatrix(lightOn);  // a tap then a long press: that press is a new cast, loop() picks it up
  } else if (turned < DRAW_DEG || len < MIN_CAST) fillMatrix(lightOn);  // pressed but didn't draw anything
  else handleCast(len);
  fifoReset();  // drop what piled up while the effect and the name played
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  matrix.begin();
#if USE_HID
  Keyboard.begin();
#endif

  Wire.begin();
  if (!imuInit()) {
    Serial.println("Wand sensor not answering: check the GY-521 wires (SDA->A4, SCL->A5, VCC, GND)");
    show("CHECK WAND");
    while (!imuInit()) { drawIcon(ICON_LOST, 0, 0); delay(500); }
  }
  imuOk = true;
  delay(100);
  // 1 s of a still wand: gyro drift, and which way gravity points. Retry while it's moving
  // (keeping the steadiest second), so picking the wand up right after an upload can't spoil it.
  float a[AXES], bestShake = 1e9;
  for (int attempt = 0; attempt < 10 && bestShake > BOOT_STD; attempt++) {
    if (attempt == 1) show("KEEP STILL");
    float gSum[AXES] = {0, 0, 0}, gSq[AXES] = {0, 0, 0}, aSum[AXES] = {0, 0, 0};
    int got = 0;
    for (int i = 0; i < 100; i++) {
      uint8_t b[14];  // accel x, y, z, temperature, gyro x, y, z
      if (mpuRead(0x3B, b, 14)) {
        float g[AXES];
        toDps(b + 8, g);
        for (int k = 0; k < AXES; k++) {
          gSum[k] += g[k];
          gSq[k] += g[k] * g[k];
          aSum[k] += word16(b + 2 * k) / 16384.0f;  // +-2 g range
        }
        got++;
      }
      delay(10);
    }
    if (!got) continue;
    float shake = 0;
    for (int k = 0; k < AXES; k++) shake = fmaxf(shake, sqrtf(fmaxf(0, gSq[k] / got - (gSum[k] / got) * (gSum[k] / got))));
    if (shake < bestShake) {
      bestShake = shake;
      for (int k = 0; k < AXES; k++) { bias[k] = gSum[k] / got; a[k] = aSum[k] / got; }
    }
  }
  Serial.print("Gyro drift: ");
  for (int k = 0; k < AXES; k++) { Serial.print(bias[k]); Serial.print(k < AXES - 1 ? ", " : " deg/s, shake "); }
  Serial.println(bestShake);
  learnOrientation(a);

  bool trained = loadTemplates();

  WiFi.begin(SECRET_SSID, SECRET_PASS);  // one try; the wand works offline too
  online = WiFi.status() == WL_CONNECTED;
  if (online) udp.begin(SERVER_PORT);
  Serial.println(online ? "WiFi connected" : "WiFi failed: running offline");
  show(trained ? "READY" : "HOLD 2s: TRAIN");
  fillMatrix(0);
  fifoReset();
}

void loop() {
  if (millis() - lastCheck >= 250) {  // is the wand sensor still there? recover by itself if not
    lastCheck = millis();
    checkCommands();
    if (imuOk && !imuHealthy()) imuOk = false;
    if (!imuOk) {
      if (imuInit()) {
        imuOk = true;
        Serial.println("Wand sensor back");
        tone(BUZZ, 1800, 60);
        fillMatrix(lightOn);
      } else {
        Serial.println("Wand sensor lost: check the GY-521 wires");
        drawIcon(ICON_LOST, 0, 0);
        sendLive();  // tells the web page too
      }
    }
  }
  if (!imuOk) return;
  if (digitalRead(BTN) == LOW) {
    if (mouseMode) mouseButton();
    else castFromButton();
    return;
  }
  float w[AXES];
  while (nextSample(w, true)) streamSample(w);  // read everything waiting first (fast)...
  streamFlush();                                 // ...then at most one WiFi send, then back to the button
  if (mouseMode && millis() - statSince >= 1000) {
    Serial.print("Mouse stream: ");
    Serial.print(statPackets);
    Serial.print(" packets/s, ");
    Serial.print(statPackets ? (float)statSamples / statPackets : 0, 1);
    Serial.print(" samples each, most waiting ");
    Serial.println(fifoPeak);
    statSince = millis();
    statPackets = statSamples = 0;
    fifoPeak = 0;
  }
}

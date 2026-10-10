/*
  ESP32 Bluetooth MP3 Player  -  v2  (volume control added)

  Buttons (INPUT_PULLUP, dusra pin GND):
    GPIO 25 = PREV   : short press = pichla gaana | long press = volume (neeche config dekho)
    GPIO 26 = PLAY   : short press = play / pause
    GPIO 27 = NEXT   : short press = agla gaana   | long press = volume (neeche config dekho)

  Long press ko pakad ke rakho to volume DHIRE DHIRE badalta rehta hai.
  Volume flash me save hota hai, to restart ke baad bhi yaad rehta hai.

  Libraries: arduino-audio-tools, arduino-libhelix, ESP32-A2DP (pschatzmann),
             Adafruit SSD1306 + Adafruit GFX
  Board: "ESP32 Dev Module", Partition Scheme: "Huge APP (3MB No OTA)"
*/

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/Communication/A2DPStream.h"
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <vector>

// ================= CONFIG =================
const char* BUDS_NAME = "YOUR_BUDS_NAME";   // <-- apne buds ka exact Bluetooth naam

#define SD_CS     5
#define BTN_PREV  25
#define BTN_PLAY  26
#define BTN_NEXT  27

#define OLED_ADDR 0x3C
#define SCREEN_W  128
#define SCREEN_H  64

// ---- volume settings ----
#define NEXT_HOLD_IS_VOL_DOWN 1   // 1 = NEXT hold -> volume KAM,  PREV hold -> volume ZYADA  (jaisa tune bola)
                                  // 0 = ulta: NEXT hold -> volume ZYADA, PREV hold -> volume KAM
#define LONG_PRESS_MS   500       // itni der dabaye rakho to long press maana jayega
#define VOL_REPEAT_MS   120       // volume har itne ms me ek step badlega (kam = tez)
#define VOL_STEP        2         // ek step me kitna % (1-5)
#define VOL_DEFAULT     60        // pehli baar ka volume (%)
#define DEBOUNCE_MS     30
// ==========================================

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
A2DPStream a2dp;
EncodedAudioStream decoder(&a2dp, new MP3DecoderHelix());
Preferences prefs;

std::vector<String> tracks;
int  current   = 0;
bool playing   = false;
bool btWasConn = false;
File mp3File;
uint8_t buf[1024];

int  vol = VOL_DEFAULT;          // 0..100 %
bool uiDirty = false;
uint32_t lastUI = 0;

// ---------- volume ----------
void applyVolume() {
  // quadratic curve: kam volume par zyada fine control milta hai
  uint8_t v = (uint8_t)(((uint32_t)vol * vol * 127UL) / 10000UL);   // 0..127
  a2dp.source().set_volume(v);
}
void saveVolume() {
  prefs.begin("player", false);
  prefs.putUChar("vol", (uint8_t)vol);
  prefs.end();
}
void changeVolume(int delta) {
  int nv = constrain(vol + delta, 0, 100);
  if (nv != vol) {
    vol = nv;
    applyVolume();
    uiDirty = true;
  }
}

// ---------- buttons ----------
enum { EV_NONE = 0, EV_SHORT, EV_LONG_TICK, EV_LONG_RELEASE };

struct Btn {
  uint8_t  pin;
  bool     lastRead;
  bool     stable;
  uint32_t tChange;
  uint32_t tDown;
  uint32_t tRepeat;
  bool     longFired;
};
Btn bPrev{BTN_PREV, HIGH, HIGH, 0, 0, 0, false};
Btn bPlay{BTN_PLAY, HIGH, HIGH, 0, 0, 0, false};
Btn bNext{BTN_NEXT, HIGH, HIGH, 0, 0, 0, false};

int btnUpdate(Btn &b) {
  uint32_t now = millis();
  bool r = digitalRead(b.pin);
  if (r != b.lastRead) { b.lastRead = r; b.tChange = now; }

  if (now - b.tChange >= DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if (r == LOW) {                       // pressed
      b.tDown = now; b.longFired = false;
    } else {                              // released
      return b.longFired ? EV_LONG_RELEASE : EV_SHORT;
    }
  }
  if (b.stable == LOW && now - b.tDown >= LONG_PRESS_MS) {
    if (!b.longFired) { b.longFired = true; b.tRepeat = now - VOL_REPEAT_MS; }
    if (now - b.tRepeat >= VOL_REPEAT_MS) { b.tRepeat = now; return EV_LONG_TICK; }
  }
  return EV_NONE;
}

// ---------- display ----------
bool btConnected() { return a2dp.source().is_connected(); }

void drawUI() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("BT: ");
  display.print(btConnected() ? "Connected" : "Searching...");

  display.setCursor(0, 14);
  if (tracks.empty()) {
    display.print("No MP3 found");
  } else {
    display.printf("Track %d/%d", current + 1, (int)tracks.size());
    String name = tracks[current];
    if (name.startsWith("/")) name = name.substring(1);
    display.setCursor(0, 28);
    display.print(name.substring(0, 21));
    if (name.length() > 21) {
      display.setCursor(0, 38);
      display.print(name.substring(21, 42));
    }
  }

  // bottom line: play state + volume bar
  display.setCursor(0, 54);
  display.print(playing ? ">Play" : "|Pause");
  display.setCursor(48, 54);
  display.print("V");
  display.drawRect(56, 54, 40, 8, SSD1306_WHITE);
  display.fillRect(56, 54, (40 * vol) / 100, 8, SSD1306_WHITE);
  display.setCursor(100, 54);
  display.print(vol);
  display.display();
}

// ---------- tracks ----------
void scanTracks() {
  tracks.clear();
  File root = SD.open("/");
  while (true) {
    File f = root.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String n = f.name();
      String low = n; low.toLowerCase();
      if (low.endsWith(".mp3") && !n.startsWith("._"))
        tracks.push_back(n.startsWith("/") ? n : "/" + n);
    }
    f.close();
    if (tracks.size() >= 100) break;
  }
  root.close();
}

void openTrack(int idx) {
  if (tracks.empty()) return;
  if (mp3File) mp3File.close();
  decoder.end();
  decoder.begin();
  current = (idx + tracks.size()) % tracks.size();
  mp3File = SD.open(tracks[current].c_str());
  Serial.printf("Opening: %s\n", tracks[current].c_str());
  drawUI();
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);

  prefs.begin("player", true);
  vol = constrain((int)prefs.getUChar("vol", VOL_DEFAULT), 0, 100);
  prefs.end();

  Wire.begin(21, 22);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("OLED not found");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Starting...");
  display.display();

  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed");
    display.println("SD card error!");
    display.display();
  } else {
    scanTracks();
    Serial.printf("%d tracks found\n", (int)tracks.size());
  }

  auto cfg = a2dp.defaultConfig(TX_MODE);
  cfg.name = BUDS_NAME;
  cfg.auto_reconnect = true;
  cfg.silence_on_nodata = true;
  a2dp.begin(cfg);
  applyVolume();

  decoder.begin();
  if (!tracks.empty()) openTrack(0);
  drawUI();
}

// ---------- loop ----------
void loop() {
  const int dirNext = NEXT_HOLD_IS_VOL_DOWN ? -1 : +1;   // NEXT hold ki direction
  const int dirPrev = -dirNext;                          // PREV hold ki direction

  // PLAY: short press = play/pause
  if (btnUpdate(bPlay) == EV_SHORT) {
    if (!tracks.empty()) playing = !playing;
    drawUI();
  }

  // NEXT: short = next track | long = volume
  int e = btnUpdate(bNext);
  if      (e == EV_SHORT)        openTrack(current + 1);
  else if (e == EV_LONG_TICK)    changeVolume(dirNext * VOL_STEP);
  else if (e == EV_LONG_RELEASE) saveVolume();

  // PREV: short = previous track | long = volume
  e = btnUpdate(bPrev);
  if      (e == EV_SHORT)        openTrack(current - 1);
  else if (e == EV_LONG_TICK)    changeVolume(dirPrev * VOL_STEP);
  else if (e == EV_LONG_RELEASE) saveVolume();

  // OLED ko volume change par thoda slow refresh (audio na atke)
  if (uiDirty && millis() - lastUI > 150) {
    drawUI();
    lastUI = millis();
    uiDirty = false;
  }

  // BT status change
  bool conn = btConnected();
  if (conn != btWasConn) { btWasConn = conn; drawUI(); }

  // playback
  if (playing && conn && mp3File) {
    size_t got = mp3File.read(buf, sizeof(buf));
    if (got == 0) {
      openTrack(current + 1);
    } else {
      size_t off = 0;
      while (off < got) {
        size_t w = decoder.write(buf + off, got - off);
        off += w;
        if (w == 0) delay(1);
      }
    }
  } else {
    delay(5);
  }
}

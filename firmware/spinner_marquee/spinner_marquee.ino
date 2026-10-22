// cutiE spinner marquee: display test for the LilyGo T-Display-S3
// (ESP32-S3, 1.9" ST7789 170x320 on the 8-bit parallel bus).
//
// Shows a random Claude Code spinner verb, like "✻ Pondering...", with the
// cycling glyph and a shimmer sweeping across the letters. The word changes
// after a random 3-15 s. Words too wide for the screen scroll like a marquee.
//
// Buttons:  BOOT (GPIO0)  short press: cycle brightness
//           KEY  (GPIO14) short press: next word now; hold 1 s: screen off/on
//
// Arduino IDE settings: Board "ESP32S3 Dev Module", USB CDC On Boot: Enabled,
// PSRAM: OPI PSRAM, Flash Size: 16MB. Library: TFT_eSPI, with
// User_Setup_Select.h set to #include <User_Setups/Setup206_LilyGo_T_Display_S3.h>
// (PlatformIO does this for you, see platformio.ini).

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include "verbs.h"

// ---- Pins ----
#define PIN_POWER_ON  15  // must be HIGH for the screen to work on battery
#define PIN_BACKLIGHT 38
#define PIN_BUTTON_1   0  // BOOT
#define PIN_BUTTON_2  14  // KEY

// ---- Tunables ----
const uint32_t HOLD_MIN_MS = 3000;       // random time each word stays up
const uint32_t HOLD_MAX_MS = 15000;
const uint32_t GLYPH_STEP_MS = 120;      // glyph animation speed
const float SHIMMER_CHARS_PER_S = 12.0f; // shimmer sweep speed
const float SHIMMER_HALF_WIDTH = 2.5f;   // shimmer band, in characters
const float SHIMMER_REST_CHARS = 6.0f;   // pause between sweeps, in characters
const float SCROLL_PX_PER_S = 70.0f;     // marquee speed
const uint32_t SCROLL_PAUSE_MS = 1200;   // pause at the start of each loop
const int SCROLL_GAP_PX = 70;            // space between the end and the repeat
const uint32_t FRAME_MS = 25;            // ~40 fps
const uint8_t BRIGHTNESS_LEVELS[] = {16, 40, 90, 160, 255};
const int BRIGHTNESS_DEFAULT = 2;

// ---- Colors (RGB565) ----
#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t COLOR_BG = TFT_BLACK;
const uint16_t COLOR_TEXT = RGB565(215, 119, 87);     // Claude orange
const uint16_t COLOR_SHIMMER = RGB565(255, 200, 175); // peak of the shimmer

// ---- Layout (landscape 320x170) ----
const int SCREEN_W = 320;
const int SCREEN_H = 170;
const GFXfont *TEXT_FONT = &FreeSansBold18pt7b;
const int GLYPH_COL_W = 44;  // glyph column, including the gap before the word
const int GLYPH_R = 13;      // glyph radius
const int TEXT_H = 48;       // height of the text band
const int TEXT_BASELINE = 36;  // baseline inside the text band
const int TEXT_TOP = (SCREEN_H - TEXT_H) / 2;
const int GLYPH_CY = TEXT_TOP + TEXT_BASELINE - 12;  // lines up with lowercase letters
const int FIT_W = SCREEN_W - GLYPH_COL_W - 8;        // widest word drawn static
const int SCROLL_X = GLYPH_COL_W;                    // marquee runs to the right edge
const int SCROLL_W = SCREEN_W - SCROLL_X;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);     // whole screen, pushed once per frame
TFT_eSprite textBand = TFT_eSprite(&tft);  // clip area for the marquee

// ---- State ----
char wordText[32];       // current verb + "..."
int wordLen = 0;
int wordW = 0;
bool scrolling = false;
int lastVerb = -1;
uint32_t wordStartMs = 0;
uint32_t holdMs = 0;
float scrollX = 0;
uint32_t pauseUntilMs = 0;
uint32_t lastFrameMs = 0;
int brightness = BRIGHTNESS_DEFAULT;
bool screenOn = true;

// Declared up here, before any function, because the Arduino IDE adds function
// prototypes above the first function and they need to know this type.
struct Button {
  uint8_t pin;
  bool down;
  uint32_t downAt;
  bool longFired;
};
Button button1 = {PIN_BUTTON_1, false, 0, false};
Button button2 = {PIN_BUTTON_2, false, 0, false};

// ---------------------------------------------------------------------------
// Backlight (PWM). The LEDC API changed in ESP32 Arduino core 3.x.

void setBacklight(uint8_t level) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BACKLIGHT, level);
#else
  ledcWrite(0, level);
#endif
}

void initBacklight() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BACKLIGHT, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BACKLIGHT, 0);
#endif
  setBacklight(BRIGHTNESS_LEVELS[brightness]);
}

// ---------------------------------------------------------------------------
// Word selection

uint32_t randomBetween(uint32_t lo, uint32_t hi) {
  return lo + esp_random() % (hi - lo + 1);
}

void nextWord() {
  int i;
  do {
    i = esp_random() % VERB_COUNT;
  } while (i == lastVerb && VERB_COUNT > 1);
  lastVerb = i;

  snprintf(wordText, sizeof(wordText), "%s...", VERBS[i]);
  wordLen = strlen(wordText);
  frame.setFreeFont(TEXT_FONT);
  wordW = frame.textWidth(wordText);
  scrolling = wordW > FIT_W;

  wordStartMs = millis();
  holdMs = randomBetween(HOLD_MIN_MS, HOLD_MAX_MS);
  scrollX = 0;
  pauseUntilMs = wordStartMs + SCROLL_PAUSE_MS;

  Serial.printf("%-24s %3d px%s, hold %lu ms\n", wordText, wordW,
                scrolling ? " (marquee)" : "", (unsigned long)holdMs);
}

// ---------------------------------------------------------------------------
// The spinner glyph: · ✢ ✳ ✶ ✻ ✽ played forward then backward, like Claude Code.
// The font has no such characters, so each one is drawn as a shape.

// One teardrop spoke: thin at the center, round at the tip.
void teardrop(TFT_eSprite &s, float cx, float cy, float angle, float len, float tipR, uint16_t c) {
  float dx = cosf(angle), dy = sinf(angle);
  float tx = cx + dx * (len - tipR), ty = cy + dy * (len - tipR);
  float px = -dy * tipR, py = dx * tipR;  // perpendicular, tip-radius long
  s.fillTriangle(cx, cy, tx + px, ty + py, tx - px, ty - py, c);
  s.fillSmoothCircle(tx, ty, tipR, c, COLOR_BG);
}

void drawGlyph(TFT_eSprite &s, int cx, int cy, int shape, uint16_t c) {
  const float R = GLYPH_R;
  const float PI_F = 3.14159265f;
  switch (shape) {
    case 0:  // · middle dot
      s.fillSmoothCircle(cx, cy, 3, c, COLOR_BG);
      break;
    case 1:  // ✢ four teardrop spokes
      for (int k = 0; k < 4; k++) teardrop(s, cx, cy, k * PI_F / 2, R * 0.75f, 3, c);
      break;
    case 2:  // ✳ eight thin spokes
      for (int k = 0; k < 8; k++) {
        float a = k * PI_F / 4;
        s.drawWideLine(cx, cy, cx + cosf(a) * R, cy + sinf(a) * R, 3, c, COLOR_BG);
      }
      break;
    case 3: {  // ✶ six-pointed star
      for (int k = 0; k < 6; k++) {
        float a = -PI_F / 2 + k * PI_F / 3;
        float ox = cx + cosf(a) * R, oy = cy + sinf(a) * R;
        float l = a - PI_F / 6, r = a + PI_F / 6, ri = R * 0.45f;
        s.fillTriangle(cx, cy, ox, oy, cx + cosf(l) * ri, cy + sinf(l) * ri, c);
        s.fillTriangle(cx, cy, ox, oy, cx + cosf(r) * ri, cy + sinf(r) * ri, c);
      }
      break;
    }
    case 4:  // ✻ six teardrop spokes
      for (int k = 0; k < 6; k++) teardrop(s, cx, cy, -PI_F / 2 + k * PI_F / 3, R, 3.5f, c);
      break;
    default:  // ✽ eight heavy teardrop spokes
      for (int k = 0; k < 8; k++) teardrop(s, cx, cy, k * PI_F / 4, R, 3.5f, c);
      break;
  }
}

int glyphShape(uint32_t now) {
  const int N = 6;
  int step = (now / GLYPH_STEP_MS) % (2 * N);
  return step < N ? step : 2 * N - 1 - step;
}

// ---------------------------------------------------------------------------
// Text with a shimmer: a bright band that sweeps across the letters.

// 0..255: how strongly character i is lit at time `now`.
uint8_t shimmerAt(int i, uint32_t now) {
  float cycle = wordLen + 2 * SHIMMER_HALF_WIDTH + SHIMMER_REST_CHARS;
  float pos = fmodf(now / 1000.0f * SHIMMER_CHARS_PER_S, cycle) - SHIMMER_HALF_WIDTH;
  float d = fabsf(i - pos) / SHIMMER_HALF_WIDTH;
  if (d >= 1) return 0;
  float t = 1 - d;
  return (uint8_t)(255 * t * t * (3 - 2 * t));  // smoothstep
}

// Draws the word with its left edge at x and baseline at y. Returns its width.
int drawShimmerText(TFT_eSprite &s, int x, int y, uint32_t now) {
  s.setFreeFont(TEXT_FONT);
  s.setTextDatum(L_BASELINE);
  char ch[2] = {0, 0};
  int startX = x;
  for (int i = 0; i < wordLen; i++) {
    ch[0] = wordText[i];
    int w = s.textWidth(ch);
    if (x + w > 0 && x < s.width()) {  // skip letters that are off-screen
      s.setTextColor(s.alphaBlend(shimmerAt(i, now), COLOR_SHIMMER, COLOR_TEXT));
      s.drawString(ch, x, y);
    }
    x += w;
  }
  return x - startX;
}

// ---------------------------------------------------------------------------
// One frame

void render(uint32_t now) {
  frame.fillSprite(COLOR_BG);
  int shape = glyphShape(now);

  if (!scrolling) {
    // Center the glyph + word as a group.
    int groupX = (SCREEN_W - (GLYPH_COL_W + wordW)) / 2;
    drawGlyph(frame, groupX + GLYPH_R + 4, GLYPH_CY, shape, COLOR_TEXT);
    drawShimmerText(frame, groupX + GLYPH_COL_W, TEXT_TOP + TEXT_BASELINE, now);
  } else {
    // Glyph stays put at the left; the word loops through the band to its right.
    drawGlyph(frame, GLYPH_R + 8, GLYPH_CY, shape, COLOR_TEXT);
    textBand.fillSprite(COLOR_BG);
    int loopW = wordW + SCROLL_GAP_PX;
    int x = -(int)scrollX;
    drawShimmerText(textBand, x, TEXT_BASELINE, now);
    drawShimmerText(textBand, x + loopW, TEXT_BASELINE, now);
    textBand.pushToSprite(&frame, SCROLL_X, TEXT_TOP);
  }

  frame.pushSprite(0, 0);
}

// Advances the marquee and decides when to change words.
void update(uint32_t now, uint32_t dtMs) {
  bool holdDone = now - wordStartMs >= holdMs;
  if (!scrolling) {
    if (holdDone) nextWord();
    return;
  }
  if (now < pauseUntilMs) {
    // Only change a scrolling word while it is paused at the start of a loop,
    // so it is never cut off halfway through.
    if (holdDone) nextWord();
    return;
  }
  scrollX += SCROLL_PX_PER_S * dtMs / 1000.0f;
  int loopW = wordW + SCROLL_GAP_PX;
  if (scrollX >= loopW) {
    scrollX = 0;
    pauseUntilMs = now + SCROLL_PAUSE_MS;
  }
}

// ---------------------------------------------------------------------------
// Buttons (active LOW). Short press fires on release; long press fires at 1 s.

// Returns 1 for a short press, 2 for a long press, 0 otherwise.
int pollButton(Button &b, uint32_t now) {
  bool pressed = digitalRead(b.pin) == LOW;
  if (pressed && !b.down) {
    b.down = true;
    b.downAt = now;
    b.longFired = false;
  } else if (pressed && b.down && !b.longFired && now - b.downAt >= 1000) {
    b.longFired = true;
    return 2;
  } else if (!pressed && b.down) {
    b.down = false;
    if (!b.longFired && now - b.downAt >= 30) return 1;  // 30 ms debounce
  }
  return 0;
}

void setScreen(bool on) {
  screenOn = on;
  if (on) {
    tft.writecommand(TFT_DISPON);
    setBacklight(BRIGHTNESS_LEVELS[brightness]);
  } else {
    setBacklight(0);
    tft.writecommand(TFT_DISPOFF);
  }
}

void handleButtons(uint32_t now) {
  int b1 = pollButton(button1, now);
  int b2 = pollButton(button2, now);
  if (!screenOn) {
    if (b1 || b2) setScreen(true);  // any button wakes it
    return;
  }
  if (b1 == 1) {
    brightness = (brightness + 1) % sizeof(BRIGHTNESS_LEVELS);
    setBacklight(BRIGHTNESS_LEVELS[brightness]);
    Serial.printf("Brightness %d/255\n", BRIGHTNESS_LEVELS[brightness]);
  }
  if (b2 == 1) nextWord();
  if (b2 == 2) setScreen(false);
}

// ---------------------------------------------------------------------------

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_BUTTON_1, INPUT_PULLUP);
  pinMode(PIN_BUTTON_2, INPUT_PULLUP);

  Serial.begin(115200);

  // Nothing here uses the radios; make sure they stay off to save battery.
  WiFi.mode(WIFI_OFF);
  btStop();

  tft.init();
  tft.setRotation(1);  // landscape, USB port on the right
  tft.fillScreen(COLOR_BG);
  initBacklight();

  frame.setColorDepth(16);
  textBand.setColorDepth(16);
  if (!frame.createSprite(SCREEN_W, SCREEN_H) || !textBand.createSprite(SCROLL_W, TEXT_H)) {
    // Should not happen with 8 MB PSRAM. Check Tools > PSRAM is "OPI PSRAM".
    Serial.println("Sprite allocation failed");
    tft.setTextColor(TFT_RED, COLOR_BG);
    tft.drawString("Sprite allocation failed", 10, 10, 2);
    while (true) delay(1000);
  }

  nextWord();
  lastFrameMs = millis();
}

void loop() {
  uint32_t now = millis();
  handleButtons(now);

  if (!screenOn) {
    delay(20);
    lastFrameMs = millis();
    return;
  }

  uint32_t dt = now - lastFrameMs;
  if (dt < FRAME_MS) {
    delay(FRAME_MS - dt);
    return;
  }
  lastFrameMs = now;
  update(now, dt);
  render(now);
}

// cutiE spinner, full-height variant, for the LilyGo T-Display-S3
// (ESP32-S3, 1.9" ST7789 170x320 on the 8-bit parallel bus).
//
// The words fill the whole screen height and run past as one continuous
// ticker: "✻ Pondering… ✻ Pondering… ✽ Brewing… ". Long words scroll faster
// so they don't take ages to pass; short words move at the base speed.
// Each randomly picked word scrolls past 3 times, then the next word follows
// in the stream, so changes happen as part of the scroll. A word never comes
// up again until at least 4 other words have been picked.
// The spinner glyph rides along before each word and keeps cycling.
//
// Buttons (the two either side of the USB-C port):
//   BOOT (GPIO0)  short press: switch word at the next gap; hold 1 s: cycle brightness
//   IO14 (GPIO14) press: turn off (deep sleep); press again to turn back on
//
// Arduino IDE settings: Board "ESP32S3 Dev Module", USB CDC On Boot: Enabled,
// PSRAM: OPI PSRAM, Flash Size: 16MB, esp32 core 2.0.14 (LilyGo's tested version).
// The screen is driven with ESP-IDF's esp_lcd parallel driver using DMA, set up
// the same way as LilyGo's factory example; it is much faster than TFT_eSPI's
// own push. TFT_eSPI is still needed, but only to draw the glyph into a sprite.
// The big font lives in big_font.h (made by tools/make_font.py).
//
// HOW IT WORKS, IN ONE PARAGRAPH
// Think of the ticker as a very long strip of paper sliding left past a
// window the size of the screen. The strip is made of "segments", each one
// "✻ Word…" plus a gap. Every frame (many times a second) the program:
//   1. slides the window a little to the right along the strip (scrollX),
//   2. glues new segments onto the right end of the strip when needed,
//   3. paints whatever part of the strip is inside the window into a picture
//      in memory (the "frame"),
//   4. sends that picture to the screen.
// Arduino calls setup() once at power-on, then calls loop() over and over.
// Reading order that makes sense: setup() and loop() at the bottom first,
// then render(), then the ticker functions.

// ---- Libraries ----
// #include pulls in code written elsewhere so we can use it here.
#include <Arduino.h>      // the basics: pinMode, digitalRead, millis, Serial...
#include <TFT_eSPI.h>     // graphics library; we use it to draw the spinner glyph shapes
#include <WiFi.h>         // only so we can switch Wi-Fi off
#include "esp_lcd_panel_io.h"      // ESP32's own fast screen driver (3 parts)
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "verbs.h"        // the list of 179 spinner words (in this sketch's folder)
#include "big_font.h"     // the big letter shapes (in this sketch's folder)

// ---- Pins ----
// A "pin" (GPIO) is one of the chip's metal legs. Each #define gives a pin
// number a readable name. These numbers come from LilyGo's board wiring.
#define PIN_POWER_ON  15  // must be HIGH for the screen to work on battery
#define PIN_BACKLIGHT 38  // the light behind the screen; we dim it with PWM
#define PIN_BUTTON_1   0  // BOOT button (beside the USB-C port)
#define PIN_BUTTON_2  14  // IO14 button (other side of the USB-C port)
// The screen is connected by 8 data wires (D0-D7) that each carry one bit,
// so one byte goes over at a time, plus a few control wires:
#define PIN_LCD_D0    39
#define PIN_LCD_D1    40
#define PIN_LCD_D2    41
#define PIN_LCD_D3    42
#define PIN_LCD_D4    45
#define PIN_LCD_D5    46
#define PIN_LCD_D6    47
#define PIN_LCD_D7    48
#define PIN_LCD_RES    5  // reset: restarts the screen chip
#define PIN_LCD_CS     6  // chip select: "I'm talking to you, screen"
#define PIN_LCD_DC     7  // data/command: is this byte a command or pixel data?
#define PIN_LCD_WR     8  // write: each tick sends the byte on D0-D7
#define PIN_LCD_RD     9  // read: unused, held HIGH
// How many bytes per second go to the screen: 16 million.
#define LCD_PIXEL_CLOCK_HZ (16 * 1000 * 1000)  // LilyGo's value; too high garbles the image

// ---- Tunables ----
// Change these numbers to change how it looks. "const" means it never changes
// while running. "f" after a number means it's a decimal (float).
// ms = milliseconds (1000 ms = 1 second), px = pixels.
const float SCROLL_PX_PER_S = 150.0f;    // base ticker speed, used for short words
// Longer words speed up: a word this many px wide or less uses the base
// speed; a word twice as wide goes twice as fast, up to SPEED_MAX_FACTOR.
const float SPEED_REF_WORD_PX = 650.0f;
const float SPEED_MAX_FACTOR = 2.2f;     // never faster than 2.2 x the base speed
const float SPEED_EASE_S = 0.6f;         // how gently it speeds up / slows down (seconds)
const int REPEATS_PER_WORD = 3;          // how many times each word scrolls past
const int NO_REPEAT_WITHIN = 5;          // a word can appear at most once in this many picks
const uint32_t GLYPH_STEP_MS = 120;      // glyph animation speed (time per shape)
const int GLYPH_R = 44;                  // glyph radius (half its width)
const int GLYPH_CELL_W = 2 * GLYPH_R + 36;  // glyph plus the space before the word
const int WORD_GAP_PX = 100;             // space after "…" before the next glyph
const uint32_t FRAME_MS = 5;             // frame cap; really limited by how fast a frame can be sent
const bool UPSIDE_DOWN = false;          // set true if the picture shows upside down
const uint8_t BRIGHTNESS_LEVELS[] = {5, 20, 40};  // 0 = off, 255 = full
const int BRIGHTNESS_DEFAULT = 1;        // which level to start on (counting from 0, so 40)

// ---- Colors (RGB565) ----
// The screen stores each pixel's color in 16 bits: 5 bits red, 6 bits green,
// 5 bits blue ("RGB565"). This macro turns normal 0-255 red/green/blue values
// into that format.
#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t COLOR_BG = TFT_BLACK;
const uint16_t COLOR_TEXT = RGB565(254, 1, 154);     // text and glyph color

// ---- Layout (landscape 320x170) ----
const int SCREEN_W = 320;
const int SCREEN_H = 170;
// The "baseline" is the invisible line letters sit on (descenders like g and
// y hang below it). This centers the font's full height on the screen.
const int BASELINE_Y = (SCREEN_H - (BIG_FONT_ASCENT + BIG_FONT_DESCENT)) / 2 + BIG_FONT_ASCENT;
const int GLYPH_CY = BASELINE_Y - 48;  // glyph's vertical center: middle of the letters

// One "✻ Word…" unit in the ticker. x is in ticker coordinates, which grow
// forever to the right; the screen shows scrollX .. scrollX + SCREEN_W.
// A "struct" bundles several values into one thing, like a row in a table.
const int MAX_WORD_GLYPHS = 24;
struct Segment {
  int32_t x;      // where this segment starts on the long strip
  int width;      // glyph cell + word + gap
  int wordW;      // the word alone
  int glyphCount; // how many letters
  uint8_t glyphs[MAX_WORD_GLYPHS];  // which letters, as indexes into BIG_FONT_GLYPHS
};

// Declared before any function: the Arduino IDE puts its generated function
// prototypes above the first function, and they need these types.
// Tracks one button so we can tell a short press from a long hold.
struct Button {
  uint8_t pin;        // which pin it's on
  bool down;          // is it being held right now?
  uint32_t downAt;    // when it was pressed (ms since power-on)
  bool longFired;     // already counted as a long hold?
};

// A "sprite" is a picture kept in memory instead of on the screen. We draw
// each frame into it, then send the finished picture all at once, so you never
// see a half-drawn frame.
TFT_eSPI tft = TFT_eSPI();               // never started; only the sprite uses it
TFT_eSprite frame = TFT_eSprite(&tft);  // landscape image everything is drawn into

// The same image turned to portrait, in internal RAM so DMA can send it while
// the next frame is drawn. Why portrait: the panel refreshes itself along its
// 320 px side. Sending a landscape image writes across that direction, so
// while text moves sideways the refresh catches half-old, half-new pixels at
// a slanting line in every column, which looks wavy. Sending in portrait
// makes each write run the same way as the refresh.
// (DMA = "direct memory access": a helper inside the chip that copies the
// picture to the screen by itself, so the main program can keep working.)
uint16_t *panelBuf = nullptr;              // the portrait picture (filled in setup)
esp_lcd_panel_handle_t lcdPanel = nullptr; // handles = the driver's name tags for
esp_lcd_panel_io_handle_t lcdIo = nullptr; // the screen and its wiring
// "volatile" warns the compiler this can change behind its back (the driver
// changes it when sending finishes), so it must re-check it every time.
volatile bool lcdBusy = false;  // a frame is still being sent

// ---- State ----
// Variables that change while running: the program's memory of where it is.
const int MAX_SEGMENTS = 4;  // a segment is always wider than the screen, so 3 are ever visible
Segment segments[MAX_SEGMENTS];  // the part of the strip we're keeping, oldest first
int segmentCount = 0;            // how many of those slots are in use
float scrollX = 0;               // how far along the strip the screen's left edge is
float scrollSpeed = SCROLL_PX_PER_S;  // current speed in px per second (eases toward the target)
int currentVerb = -1;            // which word (index into VERBS); -1 = none yet
int timesShown = 0;              // how many times the current word has been added to the strip
int recentVerbs[NO_REPEAT_WITHIN - 1];  // the last few words picked, so they aren't picked again yet
int recentCount = 0;             // how many of those slots are filled so far
bool skipRequested = false;      // BOOT was pressed: change word at the next segment
uint32_t lastFrameMs = 0;        // when the last frame was drawn
uint32_t fpsFrames = 0, fpsStartMs = 0;  // for the frames-per-second printout
int brightness = BRIGHTNESS_DEFAULT;     // current spot in BRIGHTNESS_LEVELS
Button button1 = {PIN_BUTTON_1, false, 0, false};
Button button2 = {PIN_BUTTON_2, false, 0, false};

// ---------------------------------------------------------------------------
// Backlight (PWM). The LEDC API changed in ESP32 Arduino core 3.x.
// PWM switches the light on and off 2000 times a second; the share of time
// it's on sets how bright it looks. "ledc" is the ESP32's PWM hardware.
// The #if lines pick the right code for whichever ESP32 core version you have.

void setBacklight(uint8_t level) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BACKLIGHT, level);
#else
  ledcWrite(0, level);
#endif
}

void initBacklight() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BACKLIGHT, 2000, 8);  // 2000 Hz, 8 bits (levels 0-255)
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BACKLIGHT, 0);
#endif
  setBacklight(BRIGHTNESS_LEVELS[brightness]);
}

// ---------------------------------------------------------------------------
// Font lookup
// Every character has a number (its "codepoint"): 'A' is 65, 'é' is 233,
// '…' is 8230. big_font.h holds a table of the letters we need; this finds a
// letter's row in that table. Returns -1 if the font doesn't have it.

int findGlyph(uint16_t cp) {
  for (int i = 0; i < BIG_FONT_GLYPH_COUNT; i++)
    if (BIG_FONT_GLYPHS[i].codepoint == cp) return i;
  return -1;
}

// Reads one UTF-8 character from *s and advances *s past it.
// UTF-8 is how text is stored: plain letters take 1 byte, but 'é' takes 2
// and '…' takes 3. This reads however many bytes the next character uses
// and works out its codepoint.
uint16_t nextCodepoint(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  uint16_t cp;
  if (p[0] < 0x80) {                 // 1 byte: plain English letters
    cp = p[0];
    *s += 1;
  } else if ((p[0] & 0xE0) == 0xC0) { // 2 bytes: accented letters like é
    cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    *s += 2;
  } else {                           // 3 bytes: symbols like …
    cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    *s += 3;
  }
  return cp;
}

// ---------------------------------------------------------------------------
// The ticker

// Was word i one of the last few picks?
bool pickedRecently(int i) {
  for (int k = 0; k < recentCount; k++)
    if (recentVerbs[k] == i) return true;
  return false;
}

// Picks a random word that wasn't among the last 4 picks, so any 5 picks in a
// row are all different words. esp_random() uses the chip's hardware random
// number generator; % (remainder) squeezes it into the range 0..VERB_COUNT-1.
void pickNewWord() {
  int i;
  do {
    i = esp_random() % VERB_COUNT;
  } while (pickedRecently(i) && VERB_COUNT > recentCount);  // try again if it's too recent
  currentVerb = i;
  timesShown = 0;

  // Remember this pick: drop the oldest when the list is full, add the new one at the end.
  if (recentCount == NO_REPEAT_WITHIN - 1) {
    for (int k = 1; k < recentCount; k++) recentVerbs[k - 1] = recentVerbs[k];
    recentCount--;
  }
  recentVerbs[recentCount++] = i;

  Serial.printf("%s\n", VERBS[i]);  // shows in Serial Monitor
}

// Adds the next segment at the right end of the ticker. The word only changes
// here, so a new word always slides in from the right edge, never pops in.
void appendSegment() {
  // Time for a new word? (first time, BOOT pressed, or it has been shown 3 times)
  if (currentVerb < 0 || skipRequested || timesShown >= REPEATS_PER_WORD) {
    skipRequested = false;
    pickNewWord();
  }
  timesShown++;

  if (segmentCount == MAX_SEGMENTS) {  // drop the oldest (long gone off-screen)
    memmove(&segments[0], &segments[1], sizeof(Segment) * (MAX_SEGMENTS - 1));  // shift everyone left one slot
    segmentCount--;
  }
  Segment &seg = segments[segmentCount];  // "&" = work on the slot itself, not a copy
  // Start right where the previous segment ends (or at the screen's edge if
  // this is the very first one).
  seg.x = segmentCount ? segments[segmentCount - 1].x + segments[segmentCount - 1].width
                       : (int32_t)scrollX;
  seg.glyphCount = 0;
  seg.wordW = 0;

  // Turn the word into a list of letters from the font, adding up their widths.
  const char *s = VERBS[currentVerb];
  while (*s && seg.glyphCount < MAX_WORD_GLYPHS - 1) {  // until the end of the text
    int g = findGlyph(nextCodepoint(&s));
    if (g < 0) continue;  // character missing from the font: skip it
    seg.glyphs[seg.glyphCount++] = g;
    seg.wordW += BIG_FONT_GLYPHS[g].advance;  // advance = how far to move before the next letter
  }
  int ellipsis = findGlyph(0x2026);  // "…"
  if (ellipsis >= 0) {
    seg.glyphs[seg.glyphCount++] = ellipsis;
    seg.wordW += BIG_FONT_GLYPHS[ellipsis].advance;
  }
  seg.width = GLYPH_CELL_W + seg.wordW + WORD_GAP_PX;
  segmentCount++;
}

// How fast the ticker should go right now, based on the word in the middle
// of the screen: longer words get a faster speed.
float targetSpeed() {
  float middle = scrollX + SCREEN_W / 2;  // the strip position at the screen's center
  for (int i = 0; i < segmentCount; i++) {
    if (middle >= segments[i].x && middle < segments[i].x + segments[i].width) {
      float factor = segments[i].wordW / SPEED_REF_WORD_PX;  // how much longer than "short"
      if (factor < 1) factor = 1;                            // short words: base speed
      if (factor > SPEED_MAX_FACTOR) factor = SPEED_MAX_FACTOR;
      return SCROLL_PX_PER_S * factor;
    }
  }
  return SCROLL_PX_PER_S;
}

// Keeps the ticker filled past the right edge of the screen.
void fillTicker() {
  // Shift everything back toward 0 now and then, so the float scroll position
  // never gets big enough to lose precision on a long run.
  if (segmentCount && segments[0].x > 100000) {
    int32_t base = segments[0].x;
    for (int i = 0; i < segmentCount; i++) segments[i].x -= base;
    scrollX -= base;
  }
  // Keep adding segments until the strip reaches past the screen's right edge.
  while (segmentCount == 0 ||
         segments[segmentCount - 1].x + segments[segmentCount - 1].width < scrollX + SCREEN_W + 1) {
    appendSegment();
  }
}

// ---------------------------------------------------------------------------
// Drawing into the frame buffer directly. TFT_eSPI stores 16-bit sprite
// pixels byte-swapped, so swap on the way in and out.
// (The frame is just a long list of 320 x 170 = 54,400 colors, row by row.
// The pixel at column x, row y is number y * 320 + x.)

// Swaps the two bytes of a 16-bit color.
inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

// Mixes fg over bg; alpha 0..255.
// alpha 255 = all foreground, 0 = all background, in between = a mix.
// Used for the soft edges of letters, which is what makes them look smooth.
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  uint32_t a = alpha, ia = 255 - alpha;
  // Pull out red, green and blue, mix each one, then pack them back together.
  uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * ia) / 255;
  uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * ia) / 255;
  uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * ia) / 255;
  return (r << 11) | (g << 5) | b;
}

// Paints one big letter into the frame, with its left edge near penX.
// Each letter in big_font.h is a small grid of how-much-ink values from 0
// (none) to 15 (solid), two per byte. We walk the grid and color each pixel.
void drawBigChar(uint16_t *buf, const BigGlyph &g, int penX) {
  int x0 = penX + g.xOffset;      // top-left corner of the letter's grid on screen
  int y0 = BASELINE_Y + g.yOffset;
  if (x0 >= SCREEN_W || x0 + g.width <= 0) return;  // entirely off-screen: skip
  const uint8_t *bits = BIG_FONT_BITMAPS + g.offset; // where this letter's grid starts
  uint32_t n = 0;                                    // which grid cell we're on
  for (int yy = 0; yy < g.height; yy++) {            // each row of the grid...
    int y = y0 + yy;
    for (int xx = 0; xx < g.width; xx++, n++) {      // ...each cell in the row
      uint8_t byte = pgm_read_byte(bits + (n >> 1)); // two cells per byte
      uint8_t a4 = (n & 1) ? (byte & 0x0F) : (byte >> 4);  // pick the right half: ink 0-15
      int x = x0 + xx;
      if (a4 == 0 || x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) continue;  // no ink, or off-screen
      uint16_t *p = &buf[y * SCREEN_W + x];
      // Solid ink: plain orange. Partial ink (letter edges): mix orange with
      // what's already there. a4 * 17 turns 0-15 into 0-255.
      *p = swap16(a4 == 15 ? COLOR_TEXT : blend565(COLOR_TEXT, swap16(*p), a4 * 17));
    }
  }
}

// ---------------------------------------------------------------------------
// The spinner glyph: · ✢ ✳ ✶ ✻ ✽ played forward then backward, like Claude Code.
// The fonts don't have these symbols, so each is drawn from simple shapes
// (triangles, circles, thick lines) placed around a center point with a
// little trigonometry: cos/sin of an angle give the direction of each spoke.
// Angles are in radians: PI is half a turn, PI/2 a quarter turn.

// One teardrop spoke: thin at the center, round at the tip.
// Drawn as a triangle from the center out to a circle at the tip.
void teardrop(float cx, float cy, float angle, float len, float tipR, uint16_t c) {
  float dx = cosf(angle), dy = sinf(angle);                       // direction of the spoke
  float tx = cx + dx * (len - tipR), ty = cy + dy * (len - tipR); // center of the tip circle
  float px = -dy * tipR, py = dx * tipR;                          // sideways, tip-radius long
  frame.fillTriangle(cx, cy, tx + px, ty + py, tx - px, ty - py, c);
  frame.fillSmoothCircle(tx, ty, tipR, c, COLOR_BG);
}

// Draws glyph number `shape` (0-5) centered at (cx, cy).
void drawSpinnerGlyph(int cx, int cy, int shape, uint16_t c) {
  const float R = GLYPH_R;
  const float PI_F = 3.14159265f;
  switch (shape) {  // "switch" runs the case matching shape
    case 0:  // · middle dot
      frame.fillSmoothCircle(cx, cy, R * 0.22f, c, COLOR_BG);
      break;
    case 1:  // ✢ four teardrop spokes
      for (int k = 0; k < 4; k++) teardrop(cx, cy, k * PI_F / 2, R * 0.75f, R * 0.22f, c);
      break;
    case 2:  // ✳ eight thin spokes
      for (int k = 0; k < 8; k++) {
        float a = k * PI_F / 4;  // 8 spokes, an eighth of a turn apart
        frame.drawWideLine(cx, cy, cx + cosf(a) * R, cy + sinf(a) * R, R * 0.2f, c, COLOR_BG);
      }
      break;
    case 3: {  // ✶ six-pointed star
      // Each point is two thin triangles from the center to the tip,
      // one on each side.
      for (int k = 0; k < 6; k++) {
        float a = -PI_F / 2 + k * PI_F / 3;                 // this point's direction (first one straight up)
        float ox = cx + cosf(a) * R, oy = cy + sinf(a) * R; // the point's tip
        float l = a - PI_F / 6, r = a + PI_F / 6, ri = R * 0.45f;  // inner corners either side
        frame.fillTriangle(cx, cy, ox, oy, cx + cosf(l) * ri, cy + sinf(l) * ri, c);
        frame.fillTriangle(cx, cy, ox, oy, cx + cosf(r) * ri, cy + sinf(r) * ri, c);
      }
      break;
    }
    case 4:  // ✻ six teardrop spokes
      for (int k = 0; k < 6; k++) teardrop(cx, cy, -PI_F / 2 + k * PI_F / 3, R, R * 0.26f, c);
      break;
    default:  // ✽ eight heavy teardrop spokes
      for (int k = 0; k < 8; k++) teardrop(cx, cy, k * PI_F / 4, R, R * 0.26f, c);
      break;
  }
}

// Which glyph to show right now. The clock is split into 120 ms steps that
// count 0,1,2,3,4,5,5,4,3,2,1,0 and repeat, so the glyph grows then shrinks.
int glyphShape(uint32_t now) {
  const int N = 6;  // number of shapes
  int step = (now / GLYPH_STEP_MS) % (2 * N);   // 0..11
  return step < N ? step : 2 * N - 1 - step;    // fold 6..11 back down to 5..0
}

// Rotates the landscape frame into the portrait buffer and sends it.
// Landscape (x, y) with the USB port on the right is portrait (169 - y, x).
// (Like turning a photo a quarter turn: each landscape column becomes a
// portrait row.)
void pushToPanel() {
  while (lcdBusy) {}  // the previous frame must finish sending before we overwrite it
  const uint16_t *src = (const uint16_t *)frame.getPointer();
  uint16_t *dst = panelBuf;
  for (int x = 0; x < SCREEN_W; x++) {
    uint16_t *row = dst + x * SCREEN_H;  // landscape column x becomes portrait row x
    if (!UPSIDE_DOWN) {
      for (int px = 0; px < SCREEN_H; px++) row[px] = src[(SCREEN_H - 1 - px) * SCREEN_W + x];
    } else {
      for (int px = 0; px < SCREEN_H; px++) row[px] = src[px * SCREEN_W + (SCREEN_W - 1 - x)];
    }
  }
  lcdBusy = true;
  // Hand the picture to the DMA helper. This returns at once; the helper
  // sends it in the background and calls onFrameSent() when done.
  esp_lcd_panel_draw_bitmap(lcdPanel, 0, 0, SCREEN_H, SCREEN_W, panelBuf);  // returns at once
}

// ---------------------------------------------------------------------------
// One frame

void render(uint32_t now) {
  frame.fillSprite(COLOR_BG);  // start from a black picture
  uint16_t *buf = (uint16_t *)frame.getPointer();  // direct access to the picture's pixels
  int shape = glyphShape(now);  // every glyph on screen shows the same shape
  int scroll = (int)scrollX;    // whole pixels only

  for (int i = 0; i < segmentCount; i++) {
    const Segment &seg = segments[i];
    int sx = seg.x - scroll;  // where this segment starts on the screen (can be negative = off the left)
    if (sx >= SCREEN_W || sx + seg.width <= 0) continue;  // not on screen: skip

    // The glyph, if any of it is on screen.
    int gx = sx + GLYPH_R + 8;
    if (gx + GLYPH_R > 0 && gx - GLYPH_R < SCREEN_W) drawSpinnerGlyph(gx, GLYPH_CY, shape, COLOR_TEXT);

    // Then the word, one letter at a time, moving right by each letter's width.
    int wordX = sx + GLYPH_CELL_W;
    int pen = wordX;
    for (int k = 0; k < seg.glyphCount; k++) {
      const BigGlyph &g = BIG_FONT_GLYPHS[seg.glyphs[k]];
      drawBigChar(buf, g, pen);
      pen += g.advance;
    }
  }

  pushToPanel();  // send the finished picture to the screen
}

// ---------------------------------------------------------------------------
// Buttons (active LOW). Short press fires on release; long press fires at 1 s.
// "Active LOW": the pin reads HIGH normally and LOW while pressed.

// Returns 1 for a short press, 2 for a long press, 0 otherwise.
int pollButton(Button &b, uint32_t now) {
  bool pressed = digitalRead(b.pin) == LOW;
  if (pressed && !b.down) {                 // just pressed: note the time
    b.down = true;
    b.downAt = now;
    b.longFired = false;
  } else if (pressed && b.down && !b.longFired && now - b.downAt >= 1000) {  // held 1 s
    b.longFired = true;
    return 2;
  } else if (!pressed && b.down) {          // just let go
    b.down = false;
    // Ignore presses shorter than 30 ms: buttons "bounce" (flicker on/off)
    // for a moment when pressed, and that shouldn't count.
    if (!b.longFired && now - b.downAt >= 30) return 1;  // 30 ms debounce
  }
  return 0;
}

// Turns the whole device off, as far as software can: the screen sleeps,
// its power and light are cut, and the chip goes into "deep sleep", where it
// uses very little battery and the program stops. Pressing IO14 wakes it,
// which restarts the program from setup() with a fresh word.
// (This is how LilyGo's own factory example sleeps the board.)
void powerOff() {
  Serial.println("Turning off. Press IO14 to turn back on.");
  esp_lcd_panel_io_tx_param(lcdIo, 0x10, NULL, 0);  // screen chip: go to sleep
  pinMode(PIN_POWER_ON, OUTPUT);
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_POWER_ON, LOW);    // cut the screen's power
  digitalWrite(PIN_BACKLIGHT, LOW);   // backlight off
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON_2, 0);  // wake when IO14 reads LOW (pressed)
  esp_deep_sleep_start();             // stops here; the next thing that runs is setup()
}

// Checks both buttons and does what they ask.
void handleButtons(uint32_t now) {
  int b1 = pollButton(button1, now);
  int b2 = pollButton(button2, now);
  if (b1 == 1) skipRequested = true;  // BOOT tap: new word, takes effect at the next segment
  if (b1 == 2) {  // BOOT held 1 s: next brightness level, wrapping back to the first
    brightness = (brightness + 1) % sizeof(BRIGHTNESS_LEVELS);
    setBacklight(BRIGHTNESS_LEVELS[brightness]);
    Serial.printf("Brightness %d/255\n", BRIGHTNESS_LEVELS[brightness]);
  }
  // IO14: turn off. pollButton reports a press when the button is let go,
  // so it isn't still held down (which would wake the board straight away).
  if (b2 == 1 || b2 == 2) {
    while (digitalRead(PIN_BUTTON_2) == LOW) delay(10);  // after a long hold, wait for release
    powerOff();
  }
}

// ---------------------------------------------------------------------------
// Screen setup, following LilyGo's factory example for this board.
// You shouldn't need to change anything in this section.

// The driver calls this by itself when a frame has finished sending.
bool onFrameSent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  lcdBusy = false;
  return false;
}

// One setup command for the screen chip: a command number plus its settings.
struct LcdInitCmd {
  uint8_t cmd;
  uint8_t data[14];
  uint8_t len;  // 0x80 bit: wait 120 ms afterwards
};

// LilyGo's extra init commands for the current T-Display-S3 screen
// (LCD_MODULE_CMD_1 in their factory example). They tune the screen's voltages
// and color curves (gamma). Copied as-is from LilyGo.
const LcdInitCmd LCD_INIT_CMDS[] = {
  {0x11, {0}, 0 | 0x80},  // wake up from sleep
  {0x3A, {0X05}, 1},      // 16 bits per pixel
  {0xB2, {0X0B, 0X0B, 0X00, 0X33, 0X33}, 5},
  {0xB7, {0X75}, 1},
  {0xBB, {0X28}, 1},
  {0xC0, {0X2C}, 1},
  {0xC2, {0X01}, 1},
  {0xC3, {0X1F}, 1},
  {0xC6, {0X13}, 1},      // refresh rate
  {0xD0, {0XA7}, 1},
  {0xD0, {0XA4, 0XA1}, 2},
  {0xD6, {0XA1}, 1},
  {0xE0, {0XF0, 0X05, 0X0A, 0X06, 0X06, 0X03, 0X2B, 0X32, 0X43, 0X36, 0X11, 0X10, 0X2B, 0X32}, 14},
  {0xE1, {0XF0, 0X08, 0X0C, 0X0B, 0X09, 0X24, 0X2B, 0X22, 0X43, 0X38, 0X15, 0X16, 0X2F, 0X37}, 14},
};

// Starts the screen: tells the ESP32 which pins the screen is on, how fast to
// talk to it, and which screen chip it is, then sends the setup commands.
void initLcd() {
  pinMode(PIN_LCD_RD, OUTPUT);
  digitalWrite(PIN_LCD_RD, HIGH);

  // 1. The wires ("bus"): 8 data pins plus the control pins.
  esp_lcd_i80_bus_handle_t bus = nullptr;
  esp_lcd_i80_bus_config_t busConfig = {
    .dc_gpio_num = PIN_LCD_DC,
    .wr_gpio_num = PIN_LCD_WR,
    .clk_src = LCD_CLK_SRC_PLL160M,
    .data_gpio_nums = {PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
                       PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7},
    .bus_width = 8,
    .max_transfer_bytes = SCREEN_W * SCREEN_H * sizeof(uint16_t),  // a whole frame at once
    .psram_trans_align = 0,
    .sram_trans_align = 0,
  };
  ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&busConfig, &bus));  // ESP_ERROR_CHECK stops with a message if this fails

  // 2. How to talk over those wires: speed, and which function to call
  //    when a frame finishes sending.
  esp_lcd_panel_io_i80_config_t ioConfig = {
    .cs_gpio_num = PIN_LCD_CS,
    .pclk_hz = LCD_PIXEL_CLOCK_HZ,
    .trans_queue_depth = 20,
    .on_color_trans_done = onFrameSent,
    .user_ctx = nullptr,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .dc_levels = {
      .dc_idle_level = 0,
      .dc_cmd_level = 0,
      .dc_dummy_level = 0,
      .dc_data_level = 1,
    },
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &ioConfig, &lcdIo));

  // 3. The screen chip itself (an ST7789).
  esp_lcd_panel_dev_config_t panelConfig = {
    .reset_gpio_num = PIN_LCD_RES,
    .color_space = ESP_LCD_COLOR_SPACE_RGB,
    .bits_per_pixel = 16,
    .vendor_config = nullptr,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(lcdIo, &panelConfig, &lcdPanel));
  esp_lcd_panel_reset(lcdPanel);
  esp_lcd_panel_init(lcdPanel);
  esp_lcd_panel_invert_color(lcdPanel, true);  // this panel shows inverted colors without it
  // Portrait, no swap or mirror. The 170 px wide panel sits 35 px into the
  // controller's 240 px memory.
  esp_lcd_panel_set_gap(lcdPanel, 35, 0);
  esp_lcd_panel_disp_on_off(lcdPanel, true);
  // 4. LilyGo's extra setup commands, from the table above.
  for (const LcdInitCmd &c : LCD_INIT_CMDS) {
    esp_lcd_panel_io_tx_param(lcdIo, c.cmd, c.data, c.len & 0x7F);
    if (c.len & 0x80) delay(120);
  }
}

// ---------------------------------------------------------------------------
// setup() runs once when the board powers on or resets.

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);       // OUTPUT: we control this pin
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_BUTTON_1, INPUT_PULLUP); // INPUT_PULLUP: we read it; reads HIGH unless pressed
  pinMode(PIN_BUTTON_2, INPUT_PULLUP);

  Serial.begin(115200);  // start the Serial Monitor connection at 115200 baud

  // Nothing here uses the radios; make sure they stay off to save battery.
  WiFi.mode(WIFI_OFF);
  btStop();

  initLcd();
  initBacklight();

  // Make room in memory for the two pictures (landscape and portrait).
  frame.setColorDepth(16);
  panelBuf = (uint16_t *)heap_caps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!frame.createSprite(SCREEN_W, SCREEN_H) || !panelBuf) {
    // Should not happen. Check Tools > PSRAM is "OPI PSRAM".
    Serial.println("Frame buffer allocation failed");
    while (true) delay(1000);  // stop here forever
  }

  fillTicker();  // lay out the first words
  lastFrameMs = millis();
}

// loop() runs over and over, forever, after setup() finishes.
// Each pass draws at most one frame.

void loop() {
  uint32_t now = millis();
  handleButtons(now);

  uint32_t dt = now - lastFrameMs;  // ms since the last frame
  if (dt < FRAME_MS) {              // too soon: wait a bit
    delay(FRAME_MS - dt);
    return;  // "return" ends this pass of loop() early
  }
  lastFrameMs = now;

  // Ease the speed toward the target for the word on screen, a little each
  // frame, so speed changes are smooth instead of sudden.
  float ease = (dt / 1000.0f) / SPEED_EASE_S;
  if (ease > 1) ease = 1;
  scrollSpeed += (targetSpeed() - scrollSpeed) * ease;

  // Distance comes from elapsed time, so the pace is right even if a frame
  // takes longer to draw.
  // (px per second x seconds since last frame = how far to move.)
  scrollX += scrollSpeed * dt / 1000.0f;
  fillTicker();
  render(now);

  // Frame rate on the Serial Monitor: the higher it is, the smaller each step
  // and the less visible any tearing.
  fpsFrames++;
  if (now - fpsStartMs >= 5000) {  // every 5 seconds
    Serial.printf("%.1f fps\n", fpsFrames * 1000.0f / (now - fpsStartMs));
    fpsFrames = 0;
    fpsStartMs = now;
  }
}

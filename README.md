# cutiE Hair Clip

A hair clip with a tiny screen that cycles through Claude Code's spinner words ("✻ Pondering…", "✽ Brewing…"). It runs on a LilyGo T-Display-S3 (ESP32-S3, 1.9" ST7789, 170×320) with no Wi-Fi or Bluetooth.

![cutiE Hair Clip](media/hero.gif)

## Display variants

| Variant | Folder | What it looks like |
|---|---|---|
| **Marquee** | `firmware/spinner_marquee/` | One word with the cycling glyph and a shimmer, changing every 3–15 s. Words too wide for the screen scroll like a marquee. |
| **Full height** | `firmware/spinner_fullheight/` | Words fill the screen height and run past as a continuous ticker. Long words scroll faster, and words don't repeat until four others have shown. |

## Flash it

1. Open the variant's `.ino` file in the Arduino IDE. Keep each sketch in its own folder; the IDE needs the folder name to match the file name.
2. **Tools** menu: Board **ESP32S3 Dev Module**, USB CDC On Boot **Enabled**, PSRAM **OPI PSRAM**, Flash Size **16MB**.
3. Install **TFT_eSPI**, and in its `User_Setup_Select.h` enable `#include <User_Setups/Setup206_LilyGo_T_Display_S3.h>`.
4. Full height only: use esp32 core **2.0.14** (LilyGo's tested version).
5. Upload. If it won't start, hold BOOT, tap RST, release BOOT.

## Buttons

| Button | Marquee | Full height |
|---|---|---|
| BOOT (GPIO0) | Press: cycle brightness | Press: next word at the next gap · Hold 1 s: cycle brightness |
| IO14 (GPIO14) | Press: next word now · Hold 1 s: screen off/on | Press: turn off (deep sleep) · Press again: turn on |

## Layout

| Path | Purpose |
|---|---|
| `firmware/` | One Arduino sketch per display variant |
| `portfolio.yaml`, `docs/portfolio/` | Portfolio write-up |
| `media/` | Photos and video of the clip |

Word list: 179 verbs from [claude-code-best-practice](https://github.com/shanraisshan/claude-code-best-practice/blob/main/reports/claude-spinner-verbs-and-tips.md), extracted from Claude Code 2.1.121.

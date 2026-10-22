<!-- PROCESS: how it works and why you built it this way. 3–6 short paragraphs.
     - Overview: the main pieces and how they connect (link media/architecture.png)
     - Key decisions: what you chose, what you rejected, and WHY
     - Mobile/web: client ↔ API ↔ data flow, auth, offline, deployment
     - AI: model choice, prompting/RAG/agent design, how you measured quality
     - Embedded/IoT: board choice, sensors, power budget, device ↔ cloud ↔ app flow -->

## How it works

The clip is a LilyGo T-Display-S3: an ESP32-S3 board with a 1.9" 170×320 screen, two buttons and a battery connector on a board about 62 × 26 mm. It runs entirely offline. The 179 spinner words are stored on the chip, and the hardware random number generator picks the next one, so there's no Wi-Fi to drain the battery.

I built two display styles as separate Arduino sketches:

- **Marquee:** one word at a time with the cycling spinner glyph and a shimmer sweeping across the letters, like the real spinner. Words change every 3–15 seconds. Words too wide for the screen scroll like a marquee sign instead of shrinking.
- **Full height:** the words fill the whole screen and run past as a continuous ticker. Each word scrolls past three times before the next one joins the stream, long words speed up so they don't drag, and a word can't repeat until at least four others have shown.

## Key decisions

- **The board:** a single ~$9 board with the screen, buttons, USB-C and a battery connector built in, so there's almost no wiring and little to snag on hair.
- **No networking:** keeping the word list on the chip means Wi-Fi and Bluetooth stay off, which saves power and makes the clip work anywhere.
- **Drawing the glyph in code:** the spinner shapes (✢ ✻ ✽ and others) are drawn as teardrop "spokes" rather than font characters, so they scale cleanly and animate smoothly at any size.
- **A custom large font for full height:** a built-in font at full screen height would look blocky, so I generated a smooth bitmap font sized to fill the 170 px height.
- **Low brightness by default:** the backlight is the biggest power draw, so the full-height version defaults to a dim setting and a button cycles brightness. The other button puts the board into deep sleep.

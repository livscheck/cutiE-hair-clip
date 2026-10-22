<!-- CHALLENGES: 1–3 real problems. This is where you show how you think.
     Use the same shape for each one. Failed attempts count. -->

## Smooth scrolling at full screen size

**Problem:** At full screen height, every frame changes almost every pixel. Sending frames through the TFT_eSPI graphics library was too slow for a smooth ticker.

**What I tried:** The marquee version uses TFT_eSPI for both drawing and sending frames, which works at its size. For full-screen scrolling, its send step was the bottleneck.

**Fix:** I kept TFT_eSPI only for drawing the glyph into an in-memory picture, and sent finished frames with ESP-IDF's `esp_lcd` parallel driver using DMA. DMA lets a helper inside the chip copy the frame to the screen while the processor draws the next one. I set it up the same way as LilyGo's factory example.

## Words that don't fit

**Problem:** The longest spinner words are 18 letters ("Flibbertigibbeting", "Whatchamacalliting"), far wider than a 320 px screen at a readable size.

**What I decided against:** Shrinking or cutting off long words, which would make them unreadable on such a small screen.

**Fix:** In the marquee version, I measure each word before drawing it. If it fits, it stays still. If not, it scrolls with a short pause at the start of each loop so it's readable. In the full-height version everything scrolls, and longer words move faster, up to 2.2× the base speed, with the change eased in so it doesn't jump.

## Accented words

**Problem:** A few words have accents ("Flambéing", "Sautéing"), and the display fonts only cover plain letters.

**Fix:** The marquee version folds them to plain letters ("Flambeing"). For the full-height version I generated a custom font that includes the accented letters, and the sketch reads the text as UTF-8, so "Flambéing" shows correctly.

<!-- LEARNED / NEXT: 3–5 honest bullets. What surprised you, what you'd change in v2. -->

- How much the "plumbing" matters on small devices: drawing a frame was easy, but getting it onto the screen fast enough took a lower-level driver and DMA.
- Designing for a wearable is mostly about power and weight. The backlight decides battery life more than the code does.
- Building two variants of the same idea made it easy to compare styles on the real hardware instead of guessing.
- Next: measure real current draw and runtime, design an enclosure so the board is safe to wear, and merge both styles into one firmware with a button to switch.

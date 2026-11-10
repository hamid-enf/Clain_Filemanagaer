# DSP / Processing Time Budget

This documents how much CPU time the DSP layer has inside each callback, so an
FFT / filter bank / feature-extraction pipeline can be sized safely. It also
explains the new track + frame callbacks on this branch.

## Audio (music) — `SimpleAudio` / `Ex13_AudioDma`

The PCM taps (`SimpleAudio_SetPrePcmTap`, `SimpleAudio_SetPostPcmTap`) fire once
per DMA half-buffer refill, in the super-loop (`Ex13_AudioDmaPoll`), NOT in the
I2S ISR. The refill happens when the DMA has consumed the other half.

Constants (e.g. `examples/simple_audio.h`):

| quantity | value | meaning |
|----------|-------|---------|
| `SIMPLE_AUDIO_HALF_SAMPLES` | `32766` | int16 channel samples per half |
| forced stereo | on | mono is upmixed → 16383 frames / half |
| sample rate | `48000 Hz` (typical) | |
| half-buffer period | `16383 / 48000 ≈ 341 ms` | time before the other half runs out |

So the CPU has up to **≈ 341 ms** to fill one half. The measured read+decode
cost for a WAV/MP3 from SD is on the order of ~22 ms (see
`docs/REGRESSION_AUDIO_STUTTER.md`), and software volume (SIMD) is a fraction
of a ms.

Budget inside a tap callback:

```
available  ≈ 341 ms  (half-buffer period)
minus read+decode    ≈  22 ms
minus volume/copy    ≈   <1 ms
───────────────────────────────
DSP headroom         ≈ 319 ms  per tap call
```

**FFT feasibility:** a windowed `arm_rfft_fast`/`arm_cfft` over the ~16383
frames (≈0.34 s at 48 kHz) on a Cortex‑M7 @ ~400 MHz with CMSIS‑DSP is well
under ~2 ms. A full 32766-sample FFT plus windowing/zero-pad is still under
~5 ms. This is **~60–150× headroom**, so even a multi-stage pipeline (window →
FFT → magnitude → peak/onset detection) fits with room to spare. The taps are
editable (`samples` is writable), so you can also process in place.

> The preceding fix (`e294bf9`, cache order in `sd_read_blocks`) removed the
> long staller that used to eat this budget, so the numbers above now hold.

## Video (SimpleVideo) — frame data callback

The frame callback (`SimpleVideo_SetFrameCallback`) fires **per decoded frame**,
already converted to `sv.format` (RGB565 by default), and it fires even when no
`SimpleVideo_SetPresent()` is installed (data-only mode). This is the path used
when you do not want to show the video on an LCD (~60% of the time per the user's
requirement).

Frame timing for 320×240 @ 15 fps (typical):

```
frame period               ≈  66.7 ms
decode (measured EMA)      ≈  11 ms
convert to output format   ≈   2 ms
present (LCD, when on)     ≈  36 ms   (SPI/LTDC; overlaps decode with the
                                       async-present recipe, so effective
                                       cost is lower)
```

Budget inside the frame callback:

| mode | present cost | free within the frame budget |
|------|--------------|------------------------------|
| data-only (`present == NULL`) | 0 ms | `66.7 − 11 − 2 ≈ 53.7 ms` |
| displayed, async present | mostly overlapped | `66.7 − 11 − 2 ≈ 53.7 ms` |
| displayed, sync present | 36 ms | `66.7 − 11 − 2 − 36 ≈ 17.7 ms` |

So in the common data-only case there is ~53 ms of CPU budget per frame — enough
for a 320×240 per-pixel operator (brightness/mask/edge) and a modest feature
extraction. Keep the callback light and copy into your own buffer if you need
the frame past the return (the driver double-buffers, so the pointer is valid
only until the next call).

## Track lifecycle callbacks (this branch)

Both players now report which item is being processed:

* **Audio** — `SimpleAudio_SetTrackStartCallback` / `SimpleAudio_SetTrackEndCallback`
  (backed by `Ex13_AudioDmaSetTrackStart`/`SetTrackEnd`). Start fires once when a
  track begins; end fires when it finishes (EOF) or is stopped. `SetTrackStart`
  also reports the already-playing track if installed after `Begin()`.
* **Video** — `SimpleVideo_SetTrackStartCallback` / `SimpleVideo_SetTrackEndCallback`.
  Start fires once when an item starts; end fires when it ends (EOF) or is
  stopped. `SimpleVideo_SetTrackEndCallback` is the natural point to drive the
  next item manually when auto-advance is off
  (`SimpleVideo_SetPlayMode(FM_PLAY_MODE_STOP)` + `SimpleVideo_Next()`), which is
  the "فیلم بعدی دست خودم باشه" requirement.

These callbacks run on the super-loop (never an ISR), so light work/flag setting
is fine. Heavy DSP belongs in the PCM-tap / frame-data callbacks above.

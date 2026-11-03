# Opal's video effects, reverse-engineered for the Windows port

The Windows app (`tools/win/app/effects.cpp`, `framing.hpp`) reproduces the Mac app's video chain from Opal 0.26's
`OpalCameraVideoService` (MetalPetal + Core Image). Static analysis only. The detailed notes, with addresses and
OBSERVED / INFERRED tags, are in `specs/`:

- `specs/bokeh-segmenter.md`: the person mask (two joint-bilateral "surface blur" passes at 256x144 on the raw
  2-channel network output, then guided by the camera image) and the hexagonal bokeh (`MTIBokehFilter`).
- `specs/touchup-skin-highlights.md`: touch-up (high-pass tone-curve blend), skin tone (extractSkin + CIKMeans +
  CIFaceBalance), highlights & shadows (darktable operator).
- `specs/chain-framing.md`: the order of everything, YUV conversions, Face Lock (Kalman filter), Zoom, Spotlight,
  Pixellate, mirror, look blending.

## Checking the transcriptions

`shader-check.sh` runs Opal's own Metal shaders (from `Opal Backup.app`'s metallib) on a synthetic pattern and compares
each pass with a CPU transcription of the spec (the same math as the HLSL). Results 2026-11-01:

| pass | max difference |
|---|---|
| surface blur gray / colour (mask) | 0 |
| bokeh pre | 0.2/255 |
| bokeh alpha / bravo-charlie | equal on 99.999 % of pixels (a few highlight-edge pixels differ by GPU sampling precision) |
| touch-up overlay / composite | 0 / 1/255 |
| extractSkin | equal except pixels exactly at the 0.3 threshold |
| shadows & highlights (4 settings) | 1/255 |

Also measured on this Mac: `MPSImageGaussianBlur` (touch-up sigma 8, highlights sigma 30) is a normalized Gaussian over
about +-3.5 sigma (impulse response within 2e-4 per tap), and `CIKMeans` with k = 1 returns the mean of the opaque
pixels with alpha = their share, which 8-bit output clamps per channel (min(mean, share)).

## Known differences on Windows

- Highlights & shadows' sigma-30 blur is computed at half size (box 2x2 + sigma 15): within ~1 level.
- Skin warmth: Opal sets it from a Kelvin value over its 2500..7000 K white-balance range; the trigger wasn't traced,
  so Windows uses the manual white balance, and 0.5 under auto white balance.
- Not ported yet: CLAHE (Opal's Contrast), vibrance, the user .cube LUT, intro/outro blur, low-power mode.
- Overlay (logo / footer text) geometry is ours, not Opal's (not transcribed).
- Output: Opal writes full-range BT.709 code values into a buffer labelled video range ('420v'); Windows writes the
  same values into NV12 (Windows apps also assume video range), so meeting apps see the same picture.

Deliberate difference (owner's request, 2026-11-02): the bokeh radius is scaled by frame height / 1080, so 4K and 1440p
blur as much of the picture as Opal does at 1080p (Opal uses the radius in camera pixels, which makes bokeh at 4K half as
strong). At 1080p it is identical to Opal. Touch-up (sigma 8) and highlights/shadows (sigma 30) are still in camera
pixels like Opal's.

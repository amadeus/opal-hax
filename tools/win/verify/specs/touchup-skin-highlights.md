# Opal 0.26 video effects spec: Touch-up, Skin tone, Highlights & Shadows

Source of truth: `Opal Backup.app` (v0.26 build 25.992).
- VS = `OpalCameraVideoService.xpc/.../OpalCameraVideoService` (arm64 slice; addresses are VM addresses in that slice).
- APP = `Opal.app/Contents/MacOS/Opal` (arm64).
- IR = the video service metallib, split into `scratchpad/bokeh/bc/mNNN.ll`.
- MP = `MetalPetal.framework` (binary + `Resources/default.metallib`, split into `scratchpad/mp/lib/mNNN.ll`).
- CI = macOS CoreImage. Read from `CoreImage.framework/Resources/ci_filters.metallib` and `ci_stdlib.metallib`, and from the dyld shared cache with `dyld_info -disassemble` (arm64e, this Mac's macOS 27.2).

Tags:
- **[OBS]**: seen directly in the IR or disassembly (address or function given).
- **[INF]**: inferred.
- **[UNK]**: not resolved.

All hex float constants are decoded to the float32 values the GPU uses.

---

## 0. Common facts (all three effects)

- **Where the effects live.** All three are Mac-side, in the video service. The class `FiltersTabPipeline` holds them (fields: `claheFilter`, `HPSFFilter`, `skinAdjustmentFilter`, `vibranceFilter`, `highlightsAndShadowsFilter`, `pixellateFilter`, `pipelineList`).
- **Pipeline order [OBS].** `pipelineList` is built at VS `0x10007685c` in this order:
  1. HPSF (touch-up)
  2. SkinAdjustment
  3. HighlightShadow
  4. CLAHE
  5. Vibrance
  6. Pixellate

  So Touch-up runs first, then Skin tone, then Highlights/Shadows, each on the previous output.
- **Order relative to other stages [UNK].** Where `FiltersTabPipeline` sits relative to the look LUT (`colorCubeFilter`), the bokeh `cameraPipeline` and the appearance (zoom/mirror) stages inside `MainPipeline` was not traced.
- **Output format [OBS].** Every one of these filters is set to `outputPixelFormat = 0x50` = `MTLPixelFormatBGRA8Unorm`:
  - HPSF: `0x10007c028`
  - Skin extract: `0x100048dd4`
  - Highlight/shadow: `0x100099828`

  So all intermediates and outputs are **8-bit UNORM**. The input frame is also 8-bit RGB.
- **Default sampler [INF].** A MetalPetal default sampler is linear min/mag with clamp-to-edge. No effect overrides it, except the segmenter (see 0.2), which explicitly uses address mode 0 = clampToEdge.
- **Fragment vertex shader.** All fragment kernels use MetalPetal's passthrough vertex shader. `textureCoordinate` is in [0,1] with the origin at the top-left. Output size = input size unless stated.

### 0.1 VS-side parameter routing [OBS]

The XPC call `-[OpalCameraVideoService setXxx:]` reaches `VideoService` (vtable), then `mainPipeline` (+0x68), then `filtersTabPipeline` (+0x28), then the filter. The value is passed through **unchanged** (Float).

| XPC selector | FiltersTabPipeline vtable | Filter field |
|---|---|---|
| `toggleHPSFEnabled` | 0xc0 | HPSF.isActive (+0x10) |
| `setHPSFAmount` | 0xc8 | HPSF.amount (+0x20) |
| `toggleSkinAdjustmentEnabled` | 0xd8 | Skin.isActive, and also sets needsReload = true |
| `setSkinAdjustmentAmount` | 0xe0 | Skin.amount (+0x24) |
| `setSkinAdjustmentWarmth` | 0xe8 | Skin.warmth (+0x28) |
| `setSkinAdjustmentReload` | 0xf0 | Skin.needsReload = true |
| `toggleHighlightsEnabled(e)` | 0xa8 | HS.isActive = e \|\| isShadowsOn (VS `0x100026fdc`) |
| `toggleShadowsEnabled(e)` | 0xa8 | HS.isActive = e \|\| isHighlightsOn (VS `0x1000270ac`) |
| `setHighlightsAmount` | 0xb0 | HS.highlights (+0x20) |
| `setShadowsAmount` | 0xb8 | HS.shadows (+0x24) |

- **Toggle-off behaviour [OBS].** When the user turns Highlights off, APP sends `setHighlightsAmount(0.0)` (`movi d0,#0` at APP `0x10012cf2c`). On, it sends the stored value. Shadows is [INF] symmetric.
- **Feature gates [INF].** Each `isActive` setter is ANDed with a global feature flag. All flags are 1 in the shipped data (`0x1000d9fb8`, `0x1000dda20`, `0x1000dc278`).

### 0.2 Person mask used by Touch-up and Skin tone (SegmenterFilter) [OBS]

Both Touch-up and Skin tone consume the **refined person-segmentation mask**. Neither uses a raw mask.

**When the segmenter runs**
- `MainPipeline` at `0x100083094`: if `segmenter.isActive`, then `mask = segmenter.process(frame)`. The mask is pushed to `cameraPipeline` (bokeh), `filtersTabPipeline.vt[0xd0]` (HPSF.depthMask) and `vt[0xf8]` (Skin.depthMask).
- `segmenter.isActive` is set at `0x10003fcd4` as `(bokehOn && gate) || (touchUpOn && gate) || (skinOn && gate)`.
- So the Google-Meet segmentation network must run whenever touch-up or skin tone is on.

**Raw mask format**
- Filled by `+[ImageHelpers fillSurfaceBuffer:fromGoogleMeetBuffer:]` (`0x10000acec`): width 256, height **288**, 512 bytes/row, so 16-bit per pixel (fp16 [INF]).
- It is two 256×144 planes stacked vertically:
  - Plane A = rows 0..143.
  - Plane B = rows 144..287.
- [INF] These are the network's 2 output channels in CHW order (background, person). Whether they are logits or probabilities is [UNK]; that depends on the camera-side network output. The Windows port already has this buffer from the camera.

**`SegmenterFilter.process(frame)`** (`0x10003fe18`):

1. **Pass S1: `MTISurfaceBlurFilter(radius 8, gray YES, threshold 100)`** (built at `0x10003fa40`; values from data `0x1000db4cc`/`0x1000db4d0`).
   - `inputImage` = `rawMask.cropped(to: (0,0,256,144), pixelFormat: R8Unorm(10))`, with sampler clampToEdge.
   - `inputMask` = `rawMask.cropped(to: (0,144,256,144), R8Unorm)`.
   - Output: 256×144, R8Unorm.
   - Shader `mtiSurfaceBlurGray`: *guide* = plane A (texture 0), *averaged value* = plane B (texture 1).
2. **Pass S2: `MTISurfaceBlurFilter(radius 8, gray NO, threshold 75)`** (`0x10003fb18`).
   - `inputImage` = `frame.resized(to: 256×144)` (MetalPetal `resized` is a bilinear draw [INF]), with sampler clampToEdge.
   - `inputMask` = S1 output.
   - Output: R8Unorm 256×144.
   - Shader `mtiSurfaceBlur`: guide = downscaled colour frame, averaged = S1.
3. The S2 output is the mask (value 1 = person [INF], consistent with `extractSkin`'s `mask >= 0.5` test).

**`MTISurfaceBlurFilter -initWithRadius:r andGray:g`** (VS `0x10000b184`) [OBS]:

```
threshold default = 10.0 (overwritten: 100 / 75 above)
// 1-D Gaussian, sigma = r, normalised by 1/sqrt(2*pi*r*r)  (double), computed per tap in float:
for k in 0..r-1:  w = float( (1.0/sqrt(2*pi*r*r)) * (double)expf( -(float)(k*k) / (2.0f*r*r) ) )
                  weights[r-1-k] = weights[r-1+k] = w          // 16-float array, 2r-1 used (r<=8)
function constant  mtiSurfaceBlurKernelRadius (int, index 0) = r
outputImage: kernel(inputs=[inputImage, inputMask], weights, bsigma = float(2.0*(threshold/255.0)) (double math)),
             outputDimensions = inputImage.dimensions, pixelFormat as set
```

So `bsigma` = 0.78431374 for S1 and 0.58823532 for S2.

**Shader `mtiSurfaceBlur`** (IR m014). Texture 0 = `sourceTexture` (guide), texture 1 = `depthTexture` (value, `.r`).

```
R = mtiSurfaceBlurKernelRadius            // 8
c0 = src.sample(s, uv)
negHalfInvB2 = -0.5 / (bsigma*bsigma)
norm = (0.39894 / bsigma) * (bsigma / 0.39894)   // == 1.0 (unfolded in IR); 0.39894 = 0x3FD9883BA0000000
sumW = 0; acc = 0
for i in (1-R) .. (R-1):          // outer = x offset
  wi = norm * weights[i + R - 1]
  for j in (1-R) .. (R-1):        // inner = y offset
    p   = uv + float2(i * 0.00390625, j * 0.0069444445)   // hard-coded 1/256, 1/144 texel steps
    d   = src.sample(s, p).rgb - c0.rgb
    w   = wi * exp(negHalfInvB2 * dot(d,d)) * weights[j + R - 1]
    sumW += w
    acc  += w * depth.sample(s, p)      // scalar, added to all three rgb lanes
return float4(acc/sumW, acc/sumW, acc/sumW, 1)
```

**`mtiSurfaceBlurGray`** (m015) is identical except:
- `c0 = src.sample(uv)` is a depth2d sample (scalar).
- `d = src(p) - c0` (scalar), squared.
- Returns the scalar `acc/sumW`.

Notes:
- Offsets are fixed texel steps for a 256×144 target. Both passes render at 256×144.
- The hardware sampler is linear, but the samples land on texel centres, so point and linear sampling are equivalent.
- `exp` is `fast_exp`.

**Default when the segmenter is off [OBS].** The HPSF and Skin filters initialise `depthMask = MTIImage(color: MTIColorBlack, …)` (`0x10007c344`, `0x10004c6a4`).
- A black mask makes Touch-up a no-op.
- It makes Skin extraction return all zeros, so no I/Q is measured and Skin tone is a no-op.
- [INF] The last pushed mask lingers while the segmenter stays on.

---

## 1. Touch-up (skin smoothing): `HighPassSkinSmoothingFilter` → ObjC `MTITouchUpFilter`

### 1.1 Settings → uniforms

**OCTouchUp [OBS]**
- Settings wrapper at APP `0x105618`. Default = global `0x10056fae4` = **0.152f**. Min = `0x10056faf0` = **0.1**. Max = `0x10056faf8` = **0.5** (doubles).
- Slider step = (max−min)/100 = 0.004 (TouchUpPanelView, APP `0x10039abb8`).
- The value is sent as Float to `setHPSFAmount`, then becomes `MTITouchUpFilter.amount` with no remap (VS `0x10007bfd0`).
- Example: "touch-up 0.3" → `amount = 0.3`.

**radius [OBS]**
- Swift field +0x24, initialised from global `0x1000d9fc0` = **8.0**.
- Applied each frame with `filter.setRadius:` (`0x10007c000`), which becomes `MTIMPSGaussianBlurFilter.radius`.
- Never changed at runtime [INF]: no setter path was found from XPC.
- **No resolution scaling.** Unlike upstream MetalPetal, there is no `width/750` factor.

**Tone curve control points [OBS]** (`+[MTITouchUpFilter defaultToneCurveControlPoints]`, `0x10000a140`):
- (0,0), (0.47058824, 0.57254905) = (120/255, 146/255), (1,1), as float32.
- Set on an `MTIRGBToneCurveFilter` as `RGBCompositeControlPoints`. Red/green/blue curves stay at identity.

**Mask [OBS]**
- `MTIMask(content: depthMask, component: 0, mode: 1)` (`0x10007c094`).
- `MTITouchUpFilter` passes only `mask.content`, so component and mode are ignored. The shader reads `.r` of the 256×144 refined mask, bilinear, at full-frame uv.
- Face detections are **not** used.

### 1.2 Pass graph [OBS] (`-[MTITouchUpFilter outputImage]`, VS `0x10000a688`)

| # | Pass | Inputs | Output |
|---|---|---|---|
| T1 | `metalpetal::touchup::highPassSkinSmoothingGBChannelOverlay` (IR m000) | src | src size, BGRA8 |
| T2 | `MTIMPSGaussianBlurFilter` (MPSImageGaussianBlur) | T1 | src size, format unspecified → 8-bit [INF] |
| T3 | tone-curve LUT image (`MTIRGBToneCurveFilter.toneCurveColorLookupImage`) | (control points) | 256×1 BGRA8 |
| T4 | `metalpetal::touchup::highPassSkinSmoothingMaskProcessAndComposite` (IR m001) | src, T1, T2, T3, mask.content | src size, BGRA8 |

**T2 details [OBS, MP `0x21acc`/`0x21a64`]:**
- `MPSImageGaussianBlur initWithDevice:sigma:` with `sigma = (float)ceil(radius)` = 8.0.
- `edgeMode = 1` (MPSImageEdgeModeClamp).
- If `ceil(radius) <= 0` the blur is skipped.
- [UNK] MPS's internal kernel extent and quantisation. Port as a separable Gaussian, sigma 8 px, clamp edges, about ±3σ taps (25 taps) [INF].

### 1.3 Shaders

**T1: GB-channel overlay** (m000):

```
c  = src.sample(s, uv).rgb * 0.5            // pow(2,-1) literally in IR
g  = c.g;  b = c.b
v  = (g < 0.5) ? 2*g*b : 1 - 2*(1-g)*(1-b)  // computed 3x identically
out = float4(saturate(float3(v)) / max(1.0, 1e-5), 1.0)   // divide by 1.0
```

Because `g <= 0.5`, this is effectively `v = g*b/2` for g<1. Opal differs from stock MetalPetal here: stock has no ×0.5.

**T4: mask process & composite** (m001). Texture order: `source`, `bgChannelOverlay` (T1), `blurredBGChannelOverlay` (T2), `toneCurveLUT` (T3), `mask` (depth2d), and buffer `float amount`.

```
img  = src.sample(s, uv)
m    = maskTex.sample(s, uv)                 // .r of refined person mask
lut(x) = LUT.sample(s, float2(x * 0.99609375 + 0.001953125, 0.5))   // = (255x+0.5)/256
tc   = float4(lut(img.r).r, lut(img.g).g, lut(img.b).b, img.a)
mixed = mix(img, tc, amount)                 // all 4 lanes
x    = (ov.sample(uv) - blurOv.sample(uv)).rgb + 0.5      // high pass, all lanes equal
repeat 3 times:                               // "hard light with itself"
    x = (x < 0.5) ? 2*x*x : 1 - 2*(1-x)*(1-x)     // per lane
    x = saturate(x) / max(1.0, 1e-5)
k    = clamp((x.r - 0.29411766) * 2.8651686, 0, 1)    // (x - 75/255) * 255/89
smoothed = mix(mixed, img, k)                // detail areas (k->1) keep original
out  = mix(img, smoothed, m)                 // only inside person mask
```

Constants: `0x3FEFE00000000000` = 0.99609375, `0x3F60000000000000` = 0.001953125, `0xBFD2D2D2E0000000` = −0.29411766, `0x4006EBDD80000000` = 2.8651686, `0x3EE4F8B580000000` = 1e-5.

### 1.4 Tone-curve LUT (T3), exact [OBS MP `0x2db68`, `0x2e204`]

The algorithm is GPUImage's natural cubic spline, in double:
1. Points are scaled ×255 (`(double)(float)px*255`).
2. Second derivatives come from a tridiagonal solve with `sd[0] = sd[n-1] = 0`.
3. For each segment, x runs over ints from `(int)cur.x` to `(int)next.x - 1`:
   - t = (x−cur.x)/h
   - y = a·cur.y + t·next.y + h²/6·((a³−a)·sd[i] + (t³−t)·sd[i+1])
   - y is clamped to [0,255].
4. The last point is appended.
5. `curve[(int)x] = (float)±|x−y|` (negative when x > y).

LUT build:
- Byte for channel c: `i = trunc(clamp(x + chanCurve[x], 0, 255))`, then `out = trunc(clamp((float)i + rgbCurve[i], 0, 255))`. This is **truncation, not rounding**.
- Bytes are stored B,G,R,A=255 in a 256×1 BGRA8 texture. All three channels are equal here.

Resulting LUT (index = input 8-bit value), computed with that algorithm (`specs/lut.py`):

```
0,1,2,3,5,6,7,9,10,11,13,14,15,17,18,19,20,22,23,24,26,27,28,30,31,32,34,35,36,37,39,40,
41,43,44,45,46,48,49,50,52,53,54,55,57,58,59,61,62,63,64,66,67,68,69,71,72,73,74,76,77,78,
79,81,82,83,84,85,87,88,89,90,92,93,94,95,96,98,99,100,101,102,103,105,106,107,108,109,110,112,113,114,
115,116,117,118,120,121,122,123,124,125,126,127,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,
146,147,148,149,150,151,152,153,154,155,156,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,
172,173,174,175,176,177,178,179,180,180,181,182,183,184,185,186,186,187,188,189,190,191,191,192,193,194,195,196,
196,197,198,199,200,200,201,202,203,204,204,205,206,207,207,208,209,210,211,211,212,213,214,214,215,216,217,217,
218,219,220,220,221,222,223,223,224,225,225,226,227,228,228,229,230,230,231,232,233,233,234,235,236,236,237,238,
238,239,240,240,241,242,243,243,244,245,245,246,247,248,248,249,250,250,251,252,252,253,254,255
```

- Second derivative at the middle point = −0.004814816.
- LUT[120] = 146 (passes through the control point).
- With 8-bit input, `(255x+0.5)/256` lands on texel centres, so `lut(k/255) = LUT[k]/255`.

---

## 2. Skin tone adjustment: `SkinAdjustmentFilter` → `MTISkinToneAdjustmentFilter` + Core Image `CIFaceBalance`

### 2.1 Settings → uniforms

**OCSkinAdjustment [OBS]** (APP wrapper `0x105ee8`)
- Default **0.2f** (`0x100571a14`), min **0.0** (`0x100597c08` initialiser returns 0), max **0.5** (`0x100571a18`).
- Sent as Float to `amount` (VS +0x24; VS default also 0.2 at `0x1000dda24`).
- **`CIFaceBalance.inputStrength = 2.0 * amount`** (`fmov s1,#2; fmul` at VS `0x10004beac`). So strength ∈ [0,1].

**OCSkinWarmth [OBS]** (wrapper `0x10010639c`)
- Default **0.5f** (`0x100571a20`), min **0.0**, max **1.0** (`0x100571a28`).
- Sent as Float to `warmth` (VS default 0.5 at `0x1000dda30`). `CIFaceBalance.inputWarmth = warmth`, unchanged.
- Not a user slider. APP `0x1002d9c1c` sets it from the camera's colour temperature:
  ```
  warmth = (kelvin(UInt32) - 2500) / (7000 - 2500)
  ```
  where 2500 and 7000 are the white-balance slider limits at `0x100574f90`/`f98`. The setter shares field offset `0x100597ee0` with the property sent through `setSkinAdjustmentWarmth`.
  - [INF] This happens whenever the camera reports its white balance. The owner's 0.368444 = 4158 K.
  - [UNK] The exact trigger (auto-WB report vs manual WB).
- APP sends the default (0.2 / 0.5) instead of the setting in some disabled or mode states (`0x10024d4b0`, `0x10024d5bc`, the latter gated by an enum compare). That is harmless for porting.

**Fixed parameters [OBS]**
- `averageMethod` = 0 → **K-means** path (init `strb wzr,[x20,#0x20]` at `0x10004c648`; enum `==` at `0x100047f38`). CIAreaAverage is the alternative (not used).

**Inputs used**
- The **face detection** box (normalised xmin, ymin, xmax, ymax; Skin.face struct +0x44..).
- The refined **person mask** (0.2).

### 2.2 Measurement phase (async, only when `needsReload`) [OBS] (`0x10004ac78`, dispatched on a global queue from `0x10004b274`)

1. **Face rect in pixels** (`0x100048bc8`), with W,H = `frame.size`:
   ```
   r = (xmin*W, ymin*H, (xmax-xmin)*W, (ymax-ymin)*H)
   ```
2. **Skin image** = `extractSkin(frame, mask)` (`MTISkinToneAdjustmentFilter`, VS `0x100006f50`).
   - Output = frame size, **RGBA8Unorm (0x46, hard-coded)**.
   - Mask = `MTIMask(depthMask, comp 0, mode 1)`. Only `.content` is passed to the shader.
3. **Sampling region** = `CGRectInset(r, 0.15*r.w/2, 0.3*r.h/2)` (`0x10004aea4`..`0x10004aef0`), i.e. the central 85 % × 70 % of the face box. The skin image is cropped to it (`MTIImage.cropped(to:outputPixelFormat: .unspecified)`).
4. **Mean colour** via `MTICoreImageKernel.image(byProcessing:[crop], colorSpace: DeviceRGB, outputDimensions: 1×1×1, outputPixelFormat: BGRA8, alphaType: 3 = alphaIsOne)` (`0x10004a4b0`). The CI block is `CIKMeans` with:
   - `inputExtent` = crop extent
   - `inputCount` = **1**
   - `inputPasses` = **10**
   - `inputPerceptual` = **false**
   - `inputMeans` = 1-px image of CIColor(**0.6, 0.5, 0.4, 1**)

   It is rendered into a 1×1 `BGRA` CVPixelBuffer; bytes read: R = b[2], G = b[1], B = b[0] (`0x10004a564`).
   - With k = 1 this is the mean colour of the region.
   - [UNK] Whether CIKMeans weights pixels by alpha. Non-skin pixels are (0,0,0,0) from extractSkin. Recommended port: average only pixels with alpha > 0, i.e. skin pixels [INF].
   - The result is 8-bit quantised.
5. **Reference I/Q** (`0x10004b0dc`..), with R,G,B as doubles 0..255:
   ```
   origI = 0.5959*R/255 + (-0.2746)*G/255 + (-0.3213)*B/255
   origQ = 0.2115*R/255 + (-0.5227)*G/255 + 0.3112*B/255
   ```
6. In the apply phase, `needsReload` is cleared only if origI ≠ 0 && origQ ≠ 0 (`0x10004bd34`..`0x10004bda8`). Otherwise it retries on the next frame.

**When `needsReload` is set**
- At init.
- When the effect is toggled on.
- On the XPC call `setSkinAdjustmentReload`.

I/Q are therefore measured once (per enable/reload), not per frame.

**Shader `extractSkin`** (IR m009). Textures: `sourceTexture`, `maskTexture` (depth2d).

```
if (mask.sample(uv) < 0.5) return 0;
c   = src.sample(uv)
cc  = clamp(c, 0.0001, 0.9999)                       // all 4 lanes
sum = cc.r + cc.g + cc.b
mean    = sum * 0.33333                               // 0x3FD5554760000000 = 0.33333000
invMean = 3.00003 / sum                               // 0x4008000FC0000000 = 3.00003004
s3  = max((cc.rgb - mean) * (1/(1-mean)), (mean - cc.rgb) * invMean)
sat = s3.r                                            // only red lane is used
lk  = clamp(4 * min(cc.r - cc.g, 2*cc.g - cc.b) * (1 - sat) * invMean, 0, 1)
return (lk > 0.3) ? c : 0                             // 0.3 = 0x3FD3333340000000; original c, not clamped
```

### 2.3 Apply phase (every frame) [OBS] (`0x10004b274`)

- If `!isActive`, or `origI == 0`, or `origQ == 0`: output = input.
- Otherwise:

| # | Pass | Inputs | Output |
|---|---|---|---|
| K1 | `MTICoreImageKernel(byProcessing:[src], colorSpace: CGColorSpaceCreateDeviceRGB(), outputDimensions: src.dimensions, pixelFormat: .unspecified, alphaType: 1 = nonPremultiplied)` running `CIFaceBalance` (built at `0x10004bdc8`) | src | src size |
| K2 | `MTIBlendWithMaskFilter`: background = src, image = K1, mask = `MTIMask(depthMask, component 0 (R), mode 0 (normal))` (`0x10004b7ac`..`0x10004b880`) | src, K1, mask | src size |

**CIFaceBalance parameters [OBS]**
- inputStrength = 2·amount
- inputWarmth = warmth
- inputOrigI, inputOrigQ = measured values

**Core Image context [OBS]** (VS `0x100043558`):
- `kCIContextWorkingFormat = kCIFormatBGRA8`
- `kCIContextWorkingColorSpace = DeviceRGB`
- `kCIContextOutputColorSpace = DeviceRGB`
- software renderer = false

Input, working and output spaces are all DeviceRGB, so **no linearisation**: CI operates on the same gamma-encoded 0..1 values the Metal pipeline sees [INF].

**`-[CIFaceBalance outputImage]`** (CoreImage `0x19775E54C`, ivars: warmth `0xe14`, strength `0xe18`, origI `0xe1c`, origQ `0xe20`):

```
indoorIQ  = (0.07451, 0.02745)     // _faceBalanceIndoorIQ  (double)
outdoorIQ = (0.1333,  0.007843)    // _faceBalanceOutdoorIQ (double)
tI = (1-warmth)*0.07451 + warmth*0.1333
tQ = (1-warmth)*0.02745 + warmth*0.007843
dI = strength * (tI - origI)
dQ = strength * (tQ - origQ)
if (|dI| < 0.001 && |dQ| < 0.001) return inputImage;        // identity
img = inputImage._imageByApplyingGamma(0.25).imageByUnpremultiplyingAlpha
img = _facebalance(img, delta = float2(dI, dQ)).imageByPremultiplyingAlpha._imageByApplyingGamma(4.0)
```

Selectors are resolved from the dyld cache stubs: `_imageByApplyingGamma:`, `imageByUnpremultiplyingAlpha`, `_kernel`, `vectorWithX:Y:`, `applyWithExtent:arguments:`, `imageByPremultiplyingAlpha`.

**CI kernels** (`ci_stdlib.metallib` `_ci_gamma`/`_ci_unpremul`/`_ci_premul`; `ci_filters.metallib` `_facebalance`):

```
ci_gamma(s, p)   = float4(pow(max(0, s.rgb), p), s.a)
ci_unpremul(s)   = float4(s.rgb / max(s.a, 1e-5), s.a)
ci_premul(s)     = float4(s.rgb * s.a, s.a)

_facebalance(pix, delta):
  Y = 0.299*r      + 0.587*g      + 0.114*b
  I = 0.595716*r   - 0.274453*g   - 0.321263*b
  Q = 0.211456*r   - 0.522591*g   + 0.311135*b
  s = min(1.0, 4.0*(I*I + Q*Q))
  w = pow(s, 0.2) * (1 - s*s)
  I' = I + w*delta.x ;  Q' = Q + w*delta.y
  r = Y + 0.956296*I' + 0.621024*Q'
  g = Y - 0.272122*I' - 0.647381*Q'
  b = Y - 1.10699 *I' + 1.70461 *Q'
  return float4(r, g, b, pix.a)
```

Exact float32 values: 0.298999995, 0.595716, 0.211456001, 0.587000012, −0.274453014, −0.522590995, 0.114, −0.321262985, 0.311134994, 0.200000003, 0.956296027, −0.272121996, −1.10698998, 0.621024013, −0.647381008, 1.70460999.

For an opaque frame (a = 1) the K1 pixel function is:
```
p = pow(max(0,rgb), 0.25) → facebalance → pow(max(0,·), 4.0)
```
CI fuses colour kernels, so [INF] there is no 8-bit quantisation between these steps, only at the BGRA8 output.

Note: Opal measures origI/origQ with **rounded** YIQ coefficients on **un-gamma'd** 8-bit RGB. The kernel works on rgb^0.25 with the precise FCC matrix. Keep that mismatch.

**K2 `metalpetal::blendWithMask`** (MP m050). Textures: overlay = K1, mask, base = src; `maskComponent = 0`, `usesOneMinusMaskValue = false`.

```
ov = overlay(uv); mk = mask(uv)[0]; bs = base(uv)
m  = mk * ov.a
o  = float4(ov.rgb*m, m) + (1-m) * float4(bs.rgb*bs.a, bs.a)
return float4(o.rgb / max(o.a, 1e-5), o.a)          // opaque: mix(bs, ov, mk)
```

---

## 3. Highlights & Shadows: `HighlightShadowFilter` → ObjC `MTIHighlightShadowFilter`

### 3.1 Settings → uniforms [OBS]

| Setting | APP wrapper | Default | Range | Shader uniform | VS apply |
|---|---|---|---|---|---|
| OCHighlights | `0x102d84` | 0 (bss `0x100597568`) | **−1..1** (`0x10056e408`/`410`); slider step 0.02 | `highlight` (buffer 1) | `0x1000997d0` |
| OCShadows | `0x103664` | 0 | **−1..1** (`0x10056e488`/`490`) | `shadow` (buffer 0) | `0x100099800` |

- The value is passed raw; there is no remap.
- **radius** = **30.0** (`-[MTIHighlightShadowFilter init]`, `mov w8,#0x41f00000` at `0x100009140`). The Swift code never changes it. It is the MPS Gaussian sigma.
- No mask and no face are used.

### 3.2 Pass graph [OBS] (VS `0x100009184`)

| # | Pass | Inputs | Output |
|---|---|---|---|
| H1 | `MTIMPSGaussianBlurFilter` (radius 30 → MPSImageGaussianBlur sigma = ceil(30) = 30, edge clamp) | full-res src | src size, 8-bit [INF] |
| H2 | `metalpetal::shadowhighlight::shadowHighlightAdjust` (IR m023) | `sourceTexture` = src, `blurTexture` = H1; buffers `shadow`, `highlight` | src dimensions, BGRA8 |

### 3.3 Shader (m023)

This is the darktable "shadows & highlights" algorithm (compress fixed at 50 %, low_approximation 0.01), in a sqrt-gamma YIQ space:

```
src = sourceTexture.sample(uv); bl = blurTexture.sample(uv)
s  = sqrt(max(src.rgb, 0));  t = sqrt(max(bl.rgb, 0))
ta = float3(dot(s,(0.299,0.587,0.114)),
            dot(s,(0.596,-0.2755,-0.321)),
            dot(s,(0.212,-0.523,0.311)))
Yb  = dot(t,(0.299,0.587,0.114))
tb0 = 1 - Yb
sgnH = copysign(1, -highlight)       // -0.0 counts as negative
sgnS = copysign(1,  shadow)

if (tb0 < 0.5) {                         // ---- highlights (bright surroundings)
  h2 = highlight*highlight
  xform = min(1 - 2*tb0, 1)
  if (h2 > 0) { lb0 = (0.5 - Yb) * sgnH
    do {
      la = ta.x; li = 1 - la
      lb = lb0 * sign(li)                       // sign() returns -1/0/+1
      href = copysign(|li| > 0.01 ? 1/|li| : 100, li)
      optrans = min(h2, 1) * xform;  h2 -= 1      // "h2 > 1 ? 1 : h2"
      keep = 1 - optrans
      v = (la > 0.5) ? 1 - (1 - 2*(la-0.5)) * (0.5 - lb) : 2*la*(lb + 0.5)
      ta.x = v*optrans + keep*la
      k = (1 - ta.x) * href * optrans + keep
      ta.y *= k; ta.z *= k
    } while (h2 > 0);
  }
}
if (tb0 > 0.5) {                         // ---- shadows (dark surroundings)
  s2 = shadow*shadow
  xform = min(2*tb0 - 1, 1)
  if (s2 > 0) { lb0 = (0.5 - Yb) * sgnS
    do {
      la = ta.x
      lb = lb0 * sign(1 - la)
      lref = copysign(|la| > 0.01 ? 1/|la| : 100, la)
      optrans = min(s2, 1) * xform;  s2 -= 1
      keep = 1 - optrans
      v = (la > 0.5) ? 1 - (1 - 2*(la-0.5)) * (0.5 - lb) : 2*la*(lb + 0.5)
      ta.x = v*optrans + keep*la
      k = ta.x * lref * optrans + keep
      ta.y *= k; ta.z *= k
    } while (s2 > 0);
  }
}
rgb = float3(dot(ta,(1.00048006, 0.955558002, 0.619548976)),
             dot(ta,(0.999863982,-0.271544993,-0.646785975)),
             dot(ta,(0.999445975,-1.10802996,  1.70542002)))
return float4(max(rgb,0)^2, src.a)
```

- With |amount| ≤ 1 each loop runs exactly once, with `optrans = amount² · xform`.
- Effect strength is therefore **quadratic** in the slider. The sign selects brighten or darken.
- At tb0 == 0.5 neither branch runs.

---

## 4. Differences vs the Windows port (`tools/win/app/effects.cpp`, `effects.hpp`, `settings.hpp`)

**Touch-up**

| Item | Port | Opal | Notes |
|---|---|---|---|
| `touchUpAmount` default | 0.3 | **0.152** | |
| `touchUpAmount` range | — | **0.1 … 0.5** (step 0.004) | |
| Algorithm | YCbCr skin box × edge × lerp toward a 4×4 box-blur | see section 1 | port is not the same algorithm |
| Mask | `smoothstep(0.35,0.75,mask)` | linear refined mask | |

Opal's algorithm:
- GB-overlay image.
- High-pass against a sigma-8 Gaussian, put through 3× self-hard-light and remapped by (x−75/255)·255/89.
- Blend between `mix(img, toneCurveLUT(img), amount)` and img by that detail mask.
- Then blend by the **linear** refined person mask.

There is no skin-colour detection and no blur of the colour image. "Smoothing" is really brightening low-detail areas through the tone curve.

**Skin tone**

| Item | Port | Opal | Notes |
|---|---|---|---|
| `skinAmount` default | settings.hpp 0.25 (effects.hpp 0.2) | **0.2** | |
| `skinAmount` range | — | **0 … 0.5** | the UI value is doubled into CI strength |
| `skinWarmth` default | 0.5 | 0.5 | |
| `skinWarmth` source | user value | **camera colour temperature**: `(K−2500)/4500` | not a user slider; port should drive it from the camera's AWB Kelvin if available |
| Algorithm | additive RGB tint × YCbCr skin likelihood | CIFaceBalance | see below |

Opal's algorithm:
- One-time measurement of the face's mean skin colour (extractSkin ∩ person mask ∩ inset face box) gives origI/origQ.
- Per frame, a global YIQ shift toward an indoor/outdoor target, weighted by saturation `pow(s,.2)(1−s²)`, in rgb^0.25 space.
- Then a blend by the person mask.

It needs the **face box** and the **segmentation mask**. Without a detected face it does nothing.

**Highlights / Shadows**

| Item | Port | Opal | Notes |
|---|---|---|---|
| Ranges | −1..1 | −1..1 | |
| Defaults | 0 | 0 | |
| Algorithm | cubic luma ramps ±0.3 | section 3 | needs a full-res sigma-30 Gaussian of the frame; strength ∝ amount² |

Opal's algorithm is the darktable shadows/highlights local-contrast operator, driven by a σ=30 Gaussian of the frame in sqrt-YIQ.

**Mask pipeline**
- The port should rebuild Opal's two 256×144 surface-blur passes (0.2) to get the same mask Opal feeds touch-up and skin.

**Order**
- Opal applies Touch-up → Skin → Highlights/Shadows before CLAHE / Vibrance / Pixellate. This matches the port's comment order for these three.

---

## 5. Unknowns / caveats

1. **MPSImageGaussianBlur exact taps** (σ=8 for touch-up, σ=30 for H/S): kernel width, weight quantisation, and whether intermediate textures are BGRA8 [INF] or match the input.
2. **CIKMeans semantics** with transparent (non-skin) pixels: alpha weighting or not. Also the exact convergence with 10 passes and k = 1 (k=1 should equal the mean).
3. **Raw segmentation planes**: whether plane A/B are logits or probabilities, and which one is "person". The code uses plane A as the guide and plane B as the averaged value.
4. **Pipeline position** of `FiltersTabPipeline` relative to looks (colour cube), bokeh, zoom and mirror inside `MainPipeline` was not traced.
5. **`skinWarmth` update trigger** in APP (`0x1002d9c1c`): its caller (camera WB report vs manual WB change) was not traced.
6. **Resolution dependence.** Blur sigmas (8, 30) are in pixels of the frame the video service processes (1080p by default per AGENTS.md; possibly 4K when the 4K output is selected). The surface-blur pass is fixed at 256×144.
7. **MTIImage.resized** sampling (bilinear without prefilter) for the frame → 256×144 guide [INF].
8. Wherever this spec says "is" for gamma-encoded inputs: Metal kernels see the frame values as stored (8-bit, non-sRGB texture formats; no `_sRGB` formats appear here) [INF].

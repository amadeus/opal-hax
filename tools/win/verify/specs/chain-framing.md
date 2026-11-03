# Opal video chain, framing, pixellate, spotlight, mirror: spec for the Windows/HLSL port

Source: `Opal Backup.app` v0.26, `XPCServices/OpalCameraVideoService.xpc` (arm64 slice), static analysis only.
Shader IR: `scratchpad/bokeh/bc/mNNN.ll` (Opal's custom metallib) and `scratchpad/mp/bc/mNNN.ll` (MetalPetal's
`default.metallib`, split the same way). Host code is unoptimised (-Onone) Swift, so every read and write is visible.

Tags:
- **OBSERVED @addr**: read directly from the code or IR at that address or module.
- **INFERRED**: deduced from names, MetalPetal conventions or strong context, not proven instruction by instruction.
- **UNKNOWN**: open; listed at the end.

Addresses are arm64 VM addresses in `OpalCameraVideoService` unless they say "main app" (`Opal.arm64`) or
"MetalPetal" (thin arm64 slice `scratchpad/mp/mp.arm64`).

Reusable analysis artefacts are in `scratchpad/chain/`:
- `ann.s`: annotated disassembly with selectors, strings, constants and Swift stub names.
- `swvt.txt` and `vtmap.txt`: Swift class vtables, mapping vtable slots to implementations and fields.
- `fn.sh ADDR`: prints one function.
- `py/track.py`: follows object paths through vtable calls.
- `fieldmd.txt`: Swift field and enum names.

---------------------------------------------------------------------------------------------------------------------

## 0. How the host code is organised (for orientation)

`VideoService` holds these renderers and pipelines (OBSERVED, ivars and `vt19_init` call sites @0x1000231d4…0x10002626c):

| field | renderer pixel format (MTL / CV) |
|---|---|
| `imageRenderer` (main pipeline output) | 80 BGRA8Unorm / 'BGRA' |
| `imageRendererForEffectsFilter` (final output, after the look) | 80 BGRA8Unorm / 'BGRA' |
| `imageRendererForRGBAToY` | 10 R8Unorm / 'L008' |
| `imageRendererForRGBAToUV` | 30 RG8Unorm / '2C08' |
| `imageRendererForPreview` | 80 BGRA8Unorm / 'BGRA' |
| `yuvConversion` | `MTIImageConversionYUVToRGBA` (shader m003) |
| `mainPipeline` | `MainPipeline` |
| `previewPipeline` | `PreviewPipeline` |

`MainPipeline` fields:
- `colorCubeFilter` (user .cube LUT)
- `filtersTabPipeline`
- `cameraPipeline` (bokeh)
- `cameraModulePipeline` (FXAA)
- `appearancePipeline`
- `segmenterFilter`
- `watermarkFilter`
- `footerTextFilter`
- `bubbleChatFilter`
- `introOutroFilter`

Each sub-pipeline exposes a computed `pipelineList: [FilterProtocol]` and applies the filters in that order. Every
filter checks its own `isActive` and passes the image through unchanged when it is off. The per-frame entry point
is `-[OpalCameraVideoService processFrameWithPreviewBuffer:depthBufferOrNil:face:completion:]` → Swift
"redacted function 1153" @0x100028404 (16.6 KB).

## 1. The per-frame chain (Mac → what Windows must reproduce)

All steps run at the camera stream resolution W×H (the NV12 frame the device service hands over: 1080p per
`OCResolution`, or 4K, 1440p or 720p) until the final Y/UV conversion. Intermediate images are **8-bit unorm**:
- MetalPetal's default working format, BGRA8 (INFERRED).
- Explicit `outputPixelFormat = 80` (BGRA8Unorm) on the YUV→RGB conversion, crop/resize, pixellate, blends and
  renderers (OBSERVED).
- The circle mask filter hard-codes 70 (RGBA8Unorm) (OBSERVED @0x100006c84).

No 16-bit float intermediates were found anywhere in this chain.

| # | step | where | resolution / format |
|---|---|---|---|
| 1 | NV12 Y plane (R8) and CbCr plane (RG8) wrapped as textures from the device service's IOSurface. That surface is '420f', full range: OBSERVED in the device service, `PreviewQueueManager createYUVIOSurfaceForPreview…` @0x10001381c sets `kIOSurfacePixelFormat='420f'`. | frame fn @0x100028740…0x10002894c | W×H and W/2×H/2 |
| 2 | **YUV→RGB**, shader m003 (§2.1), `outputPixelFormat = 80`. OBSERVED @0x100028a3c…0x100028b08. | `yuvConversion` | W×H BGRA8 |
| 3 | **Custom .cube LUT**: `ColorCubeLUTFilter`, MTIColorLookupFilter. Only when `toggleLUTOn` is set; this is NOT the 13 looks. OBSERVED `MainPipeline.vt44` @0x1000831dc, called @0x10002a2b4. | `colorCube` | W×H |
| 4 | **CameraModulePipeline: [FXAA]** (m017, §2.8). Each frame `fxaa.isActive = (imxModuleCapabilities.fxaaEnable == case#1)` (OBSERVED @0x100029060…0x1000290e8). The video service's IMX378 and IMX582 constant tables both store `fxaaEnable` as case #0 (@0x1000519c8, @0x100051720), so **FXAA is off for this camera** (INFERRED: the app passes the capabilities in over XPC). | `MainPipeline.vt45` list[0] | W×H |
| 5 | **FiltersTabPipeline**, in order (OBSERVED, list getter @0x10007685c): **touch-up** (HighPassSkinSmoothing) → **skin tone** → **highlights/shadows** → **CLAHE** → **vibrance** → **pixellate** (§2.4, whole frame). | list[1] | W×H |
| 6 | **CameraPipeline: [bokeh]**. Internals belong to the other spec. | list[2] | W×H |
| 7 | **AppearancePipeline**, in order (OBSERVED, list getter @0x100067df4): **faceZoom** (Face Lock, ZoomFilter type 1) → **zoom** (type 0) → **spotlight** (type 2) → **mirror**. | list[3] | each crop is resized back to W×H |
| 8 | **Overlays**, in order (OBSERVED, `MainPipeline.vt22` getter @0x10008194c): **watermark (logo)** → **footer text** → **bubble chat** → **intro/outro** (video on/off blur). | `MainPipeline.vt46` @0x100083470, called @0x10002a304 | W×H |
| 9 | Rendered into a BGRA8 pixel buffer (8-bit quantisation point). OBSERVED, closure 1246 → fn 1161 @0x10002c7b0 → `imageRenderer.vt24`. | `imageRenderer` | W×H BGRA8 |
| 10 | *(Effects tab only, thumbnails):* the SIMO "post effects" pass renders looks for the effects window. It is not on the output path (INFERRED @0x10002a5cc…0x10002a770). | | |
| 11 | **Look (13 looks)** then **gesture drawing**: `VideoService.vt135` @0x10002d638 = vt136 (look, §2.6) then vt134 (sketch, §2.7). | | W×H |
| 12 | Rendered to BGRA8 by `imageRendererForEffectsFilter` (second 8-bit quantisation). OBSERVED, closure 1164 @0x10002ca9c, `vt25` call @0x10002cc10. | | W×H BGRA8 |
| 13 | **RGB→Y**: m004 fragment, output R8 at W×H. **RGB→CbCr**: m002 compute at **full** W×H into RG8, then `MTIImage.resized(to: (W/2, H/2), outputPixelFormat: RG8)`. OBSERVED @0x10002ac50…0x10002af38 (`sdiv` by 2 @0x10002ae14 and 0x10002af0c, `resized` stub @0x10002af38). | `imageRendererForRGBAToY` / `UV` | Y W×H R8, UV W/2×H/2 RG8 |
| 14 | Y and UV IOSurfaces go back over XPC. The **main app** copies them with `vImageCopyBuffer` (no range change) into an IOSurface it created with **pixelFormat '420v'** (main app @0x1002de63c). That surface is published to the CMIO extension, which wraps it with `CVPixelBufferCreateWithIOSurface` and declares '420v' (`CMVideoFormatDescriptionCreate`, extension @0x100006248…). OBSERVED. | | |

**Output range: full-range values in a buffer labelled video range.** The shaders write full-range BT.709 (Y = 0…1 →
0…255, chroma centred at 128, no 16/235 scaling: OBSERVED in m004 and m002). The buffer is labelled '420v' (video
range). Meeting apps on the Mac therefore read Opal's full-range code values as video range. Blacks below 16 are
crushed and whites above 235 clip, and the picture gains a little contrast and saturation. For bit-exact parity,
Windows must write the same **full-range** code values into NV12. The labelling (MF_MT_VIDEO_NOMINAL_RANGE) is a
separate product decision.

**Preview (Opal popover, not sent to meeting apps):**
- The preview starts from the step-11 image and runs `PreviewPipeline`, in order: zebra stripes (m011) →
  histogram (m005 + m006) → magnifier lens (m024, §2.5) → focus rect.
- It is rendered to BGRA8 by `imageRendererForPreview`.
- OBSERVED: list getter @0x10001509c; `previewPipeline.vt31` called @0x10002a864; preview render in closure 1164.
- Histogram, zebra and magnifier therefore **never** reach the meeting-app output.

**Masks:** each frame `MainPipeline.vt43` @0x100083094 runs `SegmenterFilter` (surface blur on the NN mask) when it
is active, and hands the result to bokeh (`cameraPipe.vt17`) and to touch-up and skin (`filtersTab.vt26` and
`vt31`). Pixellate, zoom, spotlight and mirror use **no** person mask (OBSERVED: their classes have no mask field
and none is set).

**Per-frame order of side effects** in fn 1153 (OBSERVED):
1. YUV→RGB.
2. FXAA enable.
3. Masks (`mainPipeline.vt26` and `vt43`).
4. Zoom-in animation of the three ZoomFilters (§3.1).
5. `spotlight.currentFaces = faces`, `faceZoom.currentFaces = faces`, `skin.face = face`.
6. Overlay frame size.
7. `vt44` (LUT) → `vt45` (pipelines) → `vt46` (overlays).
8. Render.
9. Look and sketch.
10. Preview, then output render.
11. Y/UV.

## 2. Shaders (exact)

The fragment shaders use MetalPetal's passthrough vertex stage: `uv` is the interpolated texture coordinate at
pixel centres, `(x+0.5)/W, (y+0.5)/H`, with the origin at the top left. The constant sampler in m002/m003/m004 is
`0x807bff0000080a49`, the same constant MetalPetal's own yuv2rgb uses (normalised coords, linear filter,
clamp-to-edge; INFERRED). MTIImage samplers default to linear filtering and clamp-to-edge (INFERRED, MetalPetal
default; ZoomFilter sets clamp-to-edge explicitly @0x10001b14c).

### 2.1 YUV→RGB, m003 `metalpetal::yuv2rgbconvert_opal_custom::colorConversionFragment` (OBSERVED)
No uniforms: the constants are baked into the shader; the host passes only two textures.
```
float4 frag(float2 uv, texture2d Y /*R8*/, texture2d CbCr /*RG8*/) {
    float  y  = Y.sample(s, uv).r;                       // bilinear
    float2 c  = CbCr.sample(s, uv).rg - 0.5;             // bilinear on the half-res plane, same normalized uv
    float3 rgb = float3(y) + c.x * float3(0.0, -0.187324002, 1.855599999)   // Cb
                           + c.y * float3(1.574800014, -0.468124002, 0.0);  // Cr
    return float4(rgb, 1.0);   // no clamp in shader; the BGRA8 render target clamps to [0,1]
}
```
This is BT.709 full range, with the chroma planes sampled bilinearly at the same normalised coordinate. On the
GPU's convention that means chroma sits at the centre of each 2×2 block. The Windows `yuvToRgb` matches exactly.

### 2.2 RGB→Y, m004 `colorConversionFragmentY` (OBSERVED)
```
float fragY(float2 uv, texture2d rgb) { return dot(rgb.sample(s, uv).rgb, float3(0.2125999928, 0.7152000070, 0.0722000003)); }
```
Output is an R8Unorm render target at W×H. Sample positions coincide with texel centres, so it is per-pixel.

### 2.3 RGB→CbCr, m002 `colorConversionFragmentCbCr` (compute; OBSERVED)
```
kernel void cbcr(texture2d<float,read> in, texture2d<float,write> out, uint2 gid) {
    float3 c = in.read(gid).rgb;
    float cb = dot(float3(-0.1145720035, -0.3854230046, 0.5), c) + 0.5;
    float cr = dot(float3( 0.5, -0.4541499913, -0.0458469987), c) + 0.5;
    out.write(float4(cb, cr, 0, 0), gid);        // dispatched at FULL W×H into RG8Unorm (quantised here)
}
```
The host then resizes this RG8 image to (W/2, H/2) with a bilinear passthrough. The output pixel centre (i+0.5)
maps exactly onto the corner shared by 4 source texels, so the result is the **mean of the 2×2 quantised CbCr
values**, quantised again to RG8. Bilinear on unorm8 has finite filter precision on Apple GPUs, so expect ±1 LSB.

HLSL equivalent:
```
cb8 = round(255 * cb) per pixel
Cb  = round(mean of the 4 cb8 values)
```
Alternatively average the float CbCr and round once; the two differ by at most 1 LSB.

### 2.4 Pixellate: MetalPetal `metalpetal::pixellate` (MetalPetal m055, OBSERVED)
```
float4 pixellate(float2 uv, texture2d src, sampler s /*linear, clamp*/, constant float2 &scale /*block size in px*/) {
    float2 size = float2(src.get_width(), src.get_height());
    float2 p    = size * uv;                                   // pixel-space position (= x+0.5)
    float2 q    = (p - fmod(p, scale) + 0.5 * scale) / size;   // centre of the block, grid anchored at (0,0)
    return src.sample(s, q);                                   // bilinear
}
```
With an even block size the block centre falls on a texel corner, so the sample is the average of the 2×2 texels
around the block centre. With an odd block size it is the single centre texel. The kernel uses fast-math `fmod`.

### 2.5 Magnifier lens, m024 `metalpetal::zoomlens::applyZoomFilter` (OBSERVED; PREVIEW ONLY)
Uniforms: `radius` (float, set to 0.5 @0x100041d80) and `position` (float2 = mouse position, normalised).
`magnifiedTexture` is a crop around the mouse, resized ×8 (@0x100041b28…0x100041b44).
```
float4 lens(float2 uv, tex original, tex magnified, float radius, float2 position) {
    float3 orig   = original.sample(s0, uv).rgb;
    float3 mag    = magnified.sample(s1, uv).rgb;
    float3 centre = original.sample(s0, float2(0.5)).rgb;            // colour at the image centre
    float  D = dot(uv - position, uv - position);                    // squared distance, no aspect correction
    float a = 1 - smoothstep01(clamp(D * 79.99999237 - 0.4999999702, 0, 1));
    float3 c1 = (a < 0.5) ? mag : centre;
    float b = 1 - smoothstep01(clamp(D * 4000.000244 - 0.5000000596, 0, 1));
    float3 c2 = (b < 0.5) ? c1 : float3(1);                          // 1-px-ish white ring/dot
    float t = (4*D - radius*0.8500000238) / (radius*0.2999999523);
    float c = 1 - smoothstep01(clamp(t, 0, 1));
    float3 outc = (c >= 0.9900000095) ? c2 : (c >= 0.75 ? float3(1) : (c < 0.0099999998 ? orig : centre));
    return float4(outc, 1);
}   // smoothstep01(t) = t*t*(3-2t)
```
The lens runs in the preview only, as a colour-picker aid. It is not needed for the meeting-app output.

### 2.6 Look strength: MetalPetal `metalpetal::normalBlend` (MetalPetal m064, OBSERVED); host `VideoService.vt136` @0x10002d7b8
Host logic (OBSERVED):
```
if filterNameToApply == nil || !areFiltersOn: return img
p = JSONPipelineFactory.shared.pipelines[filterNameToApply]        // the look (CI/Metal JSON filters)
looked = p.process(img)
blend = MTIBlendFilter(.normal); blend.inputBackgroundImage = img; blend.inputImage = looked
blend.intensity = filterIntensity                                  // = OCFilters, passed straight through
return blend.outputImage
```
normalBlend, with backdrop `b` and source `s` both opaque, reduces to `out = mix(b, s, intensity)`. The full form
also un-premultiplies, composites `s.a`-over and uses `max(a, 1e-5)`. So the look result is
`lerp(img, LUT(img), OCFilters)`, computed on the 8-bit image from step 9, **after** mirror and overlays.

### 2.7 Gesture drawing (sketch): `VideoService.vt134` @0x10002d418 (OBSERVED)
`MTIBlendFilter(.normal)`, `intensity = 1`, `outputAlphaType = 3`, `outputPixelFormat = BGRA8`. The background is
the look output and the source is the SketchRenderer canvas, i.e. standard "over" compositing. It is applied after
the look, so drawings are not tinted.

### 2.8 FXAA, m017 `metalpetal::antialiasing::fxaa` (OBSERVED; inactive for IMX378, see step 4)
This is classic FXAA 3.x "simple".
```
rcp = 1/size; L = float3(0.2989999950, 0.5870000124, 0.1140000001)
lNW = dot(S(uv+(-1,-1)*rcp),L); lNE = dot(S(uv+(1,-1)*rcp),L); lSW = dot(S(uv+(-1,1)*rcp),L); lSE = dot(S(uv+(1,1)*rcp),L); lM = dot(S(uv),L)
lMin = min(lM, min(min(lNW,lNE), min(lSW,lSE)));  lMax = max(lM, max(max(lNW,lNE), max(lSW,lSE)))
dir = float2((lSW+lSE)-(lNW+lNE), (lNW+lSW)-(lNE+lSE))
red = max((lNW+lNE+lSW+lSE)*0.03125, 0.0078125)
dir = clamp(dir / (min(abs(dir.x),abs(dir.y)) + red), -8, 8) * rcp
A = 0.5*(S(uv - dir*0.1666666567) + S(uv + dir*0.1666666865))
B = 0.5*A + 0.25*(S(uv - 0.5*dir) + S(uv + 0.5*dir))
lB = dot(B,L);  rgb = (lB < lMin || lB > lMax) ? A : B;  alpha = S(uv).a
```

### 2.9 Spotlight circle, m012 `metalpetal::circle::overlayMask` (OBSERVED)
Uniforms come from `MTICircleMaskFilter` (OBSERVED @0x100006b34):
- `radius` (float)
- `position` (float2) = (positionX, positionY)
- output dims = input size
- pixel format RGBA8
```
float4 overlayMask(float2 uv, tex src, sampler s, float radius, float2 position) {
    float3 c = src.sample(s, uv).rgb;
    float2 d = float2((uv.x - position.x) * 1.777777791, uv.y - position.y);   // 16:9 aspect correction
    float t  = clamp((4*dot(d,d) - radius*0.9900000095) / (radius*0.01999998093), 0, 1);
    float m  = 1 - t*t*(3 - 2*t);
    return float4(c * m, 1);          // outside the circle → black
}
```
With radius r the visible disc satisfies `4|d|² < 0.99 r`. Its edge is a smoothstep between 0.99r and 1.01r in
4|d|² (~1 % of the radius). At steady state r = 0.5, so the disc radius is sqrt(0.99·0.5)/2 = **0.3517 × frame
height**, about 380 px at 1080p, and the outside is pure black.

## 3. Framing: zoom, Face Lock, spotlight (ZoomFilter; OBSERVED unless marked)

`ZoomFilter` is a Swift class. Its fields, at offsets from the object:

| offset | field | type / notes |
|---|---|---|
| +0x10 | isActive | |
| +0x11 | currentTypeOfZoom | enum `ZoomEnum {normal=0 (Zoom), faceZoom=1 (Face Lock), spotlight=2}` (fieldmd) |
| +0x14 | currentZoomIn | Float, 0…1, animation progress |
| +0x18 | adjustedPadding | Double: the zoom amount |
| +0x20 | currentFaces | [Face] |
| +0x28 | circleMaskFilterOrNil | |
| +0x30 | kalmanFilter | `KalmanFilter<Matrix>` |

Three instances live in AppearancePipeline (init @0x100067fc4): faceZoom (type 1), zoom (type 0), spotlight (type 2).

`Face` layout (depthai ImgDetection): `{Int32 label; Float confidence; Float xmin, ymin, xmax, ymax}`, normalised
with the origin at the top left (OBSERVED, used @0x1000175c0, synthetic face @0x10001a3b4).

### 3.1 On/off animation (per frame, frame fn @0x100029334…0x10002a028)
```
for f in [faceZoom, zoom, spotlight]:
    z = f.currentZoomIn
    if (z < 1 && f.isActive) || (z > 0 && !f.isActive):
        f.currentZoomIn = f.isActive ? z + 0.05 : z - 0.05      // Float32 accumulate, 20 frames each way, frame-count based (no dt)
```
In `AppearancePipeline.process` (vt26 @0x10006847c): if a filter is inactive but `currentZoomIn > 0`, it is
temporarily set active for this frame's processing and switched off again afterwards, so it can zoom back out.
Then the list runs in order.

### 3.2 `ZoomFilter.process(image)` (vt45 @0x10001adb4)
```
if !isActive: return image
W,H = image.size (pixels)
R0 = targetRect(image)                                  // vt44, §3.3 (pixels, origin top-left)
if type == normal or type == faceZoom:
    R  = (type == normal) ? R0 : shiftLeft(R0)            // vt37: (max(R0.x - R0.w/4, 0), R0.y, R0.w, R0.h)
    k  = 1 + 1.5*adjustedPadding                          // vt39 @0x100017b48
    cw = W/k; ch = H/k
    R1 = (max(midX(R) - cw/2, 0), max(midY(R) - ch/2, 0), cw, ch)
    R2 = (max(min(R1.x, W - R1.w), 0), max(min(R1.y, H - R1.h), 0), R1.w, R1.h)     // vt38 @0x100017960: keep inside
    R3 = (max(R2.x,0), max(R2.y,0), min(R2.w,W), min(R2.h,H))                        // vt40 @0x100017db0
    t  = smoothstep(0, 1, currentZoomIn)    (float32; t*t*(3-2t) after clamp)        // vt42 @0x100018a98 (SIMD2<Float>)
    C  = ( t*R3.x, t*R3.y, W - (W - R3.w)*t, H - (H - R3.h)*t )                      // float32 math, lerp(full frame, R3, t)
    out = image.withSampler(clampToEdge).cropped(to: C, BGRA8).resized(to: (W,H), mode .scale, BGRA8)
    return out ?? image
else // spotlight
    cm.positionX = (R0.minX + R0.width/4) / W          // Float
    cm.positionY = R0.midY / H
    cm.radius    = 11 - 10.5*currentZoomIn             // 11 (off) → 0.5 (fully on)
    return cm.outputImage                              // §2.9
```
Crop and resize semantics (MetalPetal, OBSERVED in MetalPetal `-[MTICropFilter outputImage]`):
- Crop output size = (round(C.w), round(C.h)), using `round()` because roundingMode = 0.
- The quad's texture coordinates are exactly `C.x/W … (C.x+C.w)/W` and `C.y/H … (C.y+C.h)/H` (unrounded).
- Sampling is bilinear with clamp-to-edge.
- The resize back to W×H is a second bilinear passthrough.

The image is therefore resampled twice. One-step equivalent in HLSL, for output pixel (i, j) at W×H:
```
first texture : cw' = round(C.w), ch' = round(C.h);  src_uv(a,b) = (C.x + (a+0.5)/cw'*C.w)/W, (C.y + (b+0.5)/ch'*C.h)/H
second pass   : sample that cw'×ch' image bilinearly at ((i+0.5)/W, (j+0.5)/H)
```
Both passes are 8-bit. Face Lock and Zoom **stack**: faceZoom crops first, then zoom crops the result.

Zoom (type 0) target: `R0 = vt36(image, Face(xmin 0.4, ymin 0.4, xmax 0.6, ymax 0.6))` = (0.4W, 0.4H, 0.2W, 0.2H).
Only its centre (W/2, H/2) matters, so Zoom is a **centred** crop of size frame/(1+1.5·OCZoom).

### 3.3 Face Lock and spotlight target rect (vt44 @0x10001a338 → fn728 @0x10001a53c)
```
faces = currentFaces.filter { $0.confidence >= 0.75 }                    // closure @0x10001a524
if !faces.isEmpty:
    rects = faces.map { (xmin*W, ymin*H, (xmax-xmin)*W, (ymax-ymin)*H) }  // vt36 @0x1000175c0 (Float→Double)
    U = union(rects)                       // closure 730: min x, min y, max maxX, max maxY
    return kalmanStep(U)                   // vt41 @0x100017f54
else:
    s = kalman.x                           // last estimate, NO predict/update
    return (s[0], s[1], s[3]*16/9, s[3])
```
`kalmanStep(r)`: state x = [x, y, w, h]. All matrices are 4×4 doubles, using the public "KalmanFilter" Swift
package conventions (symbols `KalmanFilter.predict/update`, `Matrix`).
```
// init (fn709 @0x100017308): x0 = [0,0,0,0], P0 = 4x4 matrix of ALL ONES (not identity)
pred  = kf.predict(F = I4, B = zeros(4x4), u = [0,0,0,0], Q = 0.002 * I4)     // x' = F x + B u;  P' = F P Fᵀ + Q
z     = [r.minX, r.minY, r.width, r.height]
upd   = pred.update(z, H = I4, R = I4)         // K = P Hᵀ (H P Hᵀ + R)⁻¹;  x = x + K(z - Hx);  P = (I - K H) P
if |upd.x[2] - r.width| < 70.0:               // pixels (resolution dependent!)
    z2  = [r.minX, r.minY, r.width, pred.x[3]]   // height measurement replaced by the predicted height
    kf  = pred.update(z2, I4, I4)
    h   = pred.x[3]
    return (kf.x[0], kf.x[1], h*16/9, h)
else:
    kf  = upd
    return (upd.x[0], upd.x[1], upd.x[3]*16/9, upd.x[3])
```
- The predict and update formulas are INFERRED from the package; their call sequence is OBSERVED.
- Steady-state per-frame gain for x/y is ≈0.0437, an EMA time constant of ≈22 frames (≈0.75 s at 30 fps). The
  first frames are coupled through the all-ones P0, so port the full matrix math in double precision.
- The filter steps **every rendered frame** with whatever face list is current, re-feeding the last detection.
- Each ZoomFilter instance has its own Kalman filter: Face Lock and spotlight track independently.

Resulting framing:
- **Face Lock crop**: centre x = max(x − w/4, 0) + w/2 (with w = h·16/9, normally x + h·4/9); centre y = y + h/2.
  Size = frame/(1 + 1.5·OCFaceLock), **independent of face size**, clamped inside the frame.
- **Quirk**: before any face has ever been seen the Kalman state is all zero. The crop centre is then (0, 0) and the
  crop parks at the **top-left corner**.
- **Spotlight centre**: ((x + w/4)/W, (y + h/2)/H) from its own Kalman state, i.e. the same centre with no clamp.

### 3.4 Mirror (MirrorFilter.process @0x1000431e0)
`image.imageByApplyingCGOrientation(2 /* .upMirrored */)` when active: a horizontal flip only (exact pixel
reversal). It is the last appearance step: after zoom, Face Lock and spotlight, **before** the logo and text
overlays. Face coordinates stay in unmirrored camera space.

## 4. Pixellate (PixellateFilter, OBSERVED; init fn810 @0x10001de6c, process vt25 @0x10001d728)
```
lookupTableForDivisors           = [4, 8, 10, 20, 40]                       (UInt8)
lookupTableForResolutionMultiple = ["360p":1, "720p":2, "1080p":3, "1440p":4, "2160p":6]
i     = Int(amount) (truncation); if i >= 5 → i = 4
mult  = lookupTableForResolutionMultiple[currentResolutionStr]               // camera stream resolution string
block = mult * divisors[i]                                                   // UInt8 multiply (overflow-checked)
MTIPixellateFilter.scale = (block, block); outputPixelFormat = BGRA8          // §2.4
```
Pixellate covers the **whole frame**: it is the last FiltersTab step and runs before bokeh, zoom and mirror. Block
sizes at 1080p are 12/24/30/60/120 px, a constant fraction of the height (1/90, 1/45, 1/36, 1/18, 1/9).

## 5. Parameter mapping (user setting → uniform)

The app pushes each `@Published` setting straight over XPC as `Float(Double)` with no transform. OBSERVED: main
app startup sync @0x10001a9a0…0x10001acd0, getters @0x1003368fc / 0x100336e54 / 0x10033a044 / 0x10033852c. The
video service stores the values as-is: OBSERVED `VideoService.vt106/vt108/vt126/vt48` →
`AppearancePipeline.vt21/vt24`, `FiltersTabPipeline.vt36`, `filterIntensity`.

| setting | XPC method | lands in | use |
|---|---|---|---|
| OCZoom (e.g. 0.154) | setZoomAmountWithNewAmount | zoom.adjustedPadding | crop = frame/(1+1.5·v) |
| OCIsZoomOn | toggleZoomEnabled | zoom.isActive | 20-frame smoothstep ramp |
| OCFaceLock (e.g. 0.2375) | setFaceZoomAmountWithNewAmount | faceZoom.adjustedPadding | crop = frame/(1+1.5·v) |
| OCIsFaceLockOn | toggleFaceZoomEnabled | faceZoom.isActive | |
| OCIsSpotlightOn | toggleSpotlightEnabled | spotlight.isActive (AppearancePipeline.vt18) | radius 11−10.5·ramp; no amount |
| OCPixellate (e.g. 2) | setPixellateAmountWithNewAmount | pixellate.amount | Int(v) → divisor index |
| OCIsPixellateOn | togglePixellateEnabled | pixellate.isActive | |
| OCFilters (e.g. 0.71) | setFilterIntensityWithNewAmount | VideoService.filterIntensity | normalBlend intensity |
| OCSelectedFilter / OCAreFiltersOn | setFilterNameToApply / setAreFiltersOn | look selection / gate | |
| OCIsMirrorOn | toggleMirroringEnabled | mirror.isActive | |
| OCResolution | setResolution | pixellate.currentResolutionStr (INFERRED) | pixellate multiplier |

Slider ranges are INFERRED: 0…1 for zoom, Face Lock and filters, judging by stored values; integer steps 0…4 for
pixellate.

## 6. Where the Windows code differs (`tools/win/app/effects.cpp`, `framing.hpp`, `main.cpp`)

1. **Chain order and geometry.**
   - Windows: crop, zoom and mirror happen in `CSToRgb` at sampling time. Effects then run on the cropped image,
     background effects come after the tones, the LUT look comes before the overlay, and the overlay is last.
   - Opal: every effect runs on the full uncropped frame (§1 step 5 → bokeh). Only then come Face Lock crop → zoom
     crop → spotlight → mirror → logo/text overlays → *8-bit render* → look blend → drawing → *8-bit render* → Y/UV.
   - Consequences: the look also colours the logo and text; the overlays are not mirrored or zoomed; the pixellate
     grid and bokeh are computed before zoom, so they get magnified by it.
2. **YUV→RGB**: the constants match. Windows resamples NV12 once at the crop position. Opal converts at native
   resolution and then resamples twice (crop then resize, both bilinear, 8-bit).
3. **Output range**: Windows `rgbToYuv` writes video range (16–235/240). Opal writes **full range** (Y = dot709·255,
   C = dot·255+128) into a buffer labelled '420v'. Windows also averages float YUV over 2×2 and rounds once. Opal
   quantises per-pixel CbCr to 8-bit first and then box-averages (§2.3).
4. **Zoom**:
   - Windows scale `1/(1+2a)` plus exponential smoothing (0.4 s / 0.6 s).
   - Opal: crop `1/(1+1.5a)`, always centred, entering and leaving via a 20-frame Float `smoothstep` lerp from the
     full frame. No time constant: it is frame-count based.
5. **Face Lock**:
   - Windows: largest face; zoom depends on face height (`3.5−2a`); centre at 0.55·face height; exponential
     smoothing; falls back to centre when there is no face.
   - Opal: union of **all** faces with confidence ≥0.75 in pixels, then the 4-state Kalman filter (Q 0.002, R 1,
     P0 ones, 70-px width gate, height lock).
   - Opal: centre = (x + h·4/9, y + h/2), with the x−w/4 clamp at 0.
   - Opal: crop size = frame/(1+1.5·OCFaceLock), regardless of face size.
   - Opal: with no face it holds the last estimate (the top-left corner if none has ever been seen).
   - Opal: Face Lock and Zoom stack as two crops. Windows takes min(scale).
6. **Pixellate**:
   - Windows: background only (needs the mask), with block `8+56a` px, sampling the 4×-downsampled "soft" image.
   - Opal: the **entire frame**, block = mult(res)·{4,8,10,20,40}[Int(a)] px, sampled bilinearly at the block centre
     of the full-res image, with the grid anchored at the top left of the *unmirrored* image.
7. **Spotlight**:
   - Windows: darkens the background through the person mask by `0.8·amount`.
   - Opal: no mask and no amount. A **black-outside circle** of radius 0.3517·H around the face centre from
     spotlight's own Kalman, x aspect-corrected by 16/9, a ~1 % smoothstep edge, and an animated radius uniform
     11 → 0.5 over 20 frames.
8. **Mirror**: Windows flips at the start. Opal flips after zoom and spotlight and before the overlays. It is an
   exact flip either way, but overlay orientation and the pixellate grid anchor differ.
9. **Look strength**:
   - Same formula `lerp(c, look(c), OCFilters)`.
   - Opal applies it to the 8-bit-quantised, post-overlay image, after all effects and after mirror.
   - Windows applies it before the overlay and on unquantised floats.
10. **FXAA**: Windows has none. Opal has it, but it is off for the IMX378 (INFERRED), so nothing to add.
11. **Touch-up, skin, highlights/shadows, CLAHE, vibrance**: Opal's relative order is touch-up → skin →
    highlights/shadows → CLAHE → vibrance → pixellate → bokeh. Windows has touch-up → skin → shadows/highlights →
    background. The relative order is right, but Windows runs them after the crop rather than at full resolution.
12. **Intermediate precision**: Opal quantises to 8 bits after every MetalPetal pass. Windows keeps floats in one
    composite pass, so expect ±1–2 LSB differences unless the 8-bit stores are emulated at the same points: after
    YUV→RGB, after each filter, after each crop/resize, at steps 9 and 12, and in the CbCr pass.

## 7. Unknowns / caveats

- **Final output size.** Everything runs at the camera stream resolution, and the main app's '420v' surface size
  was not traced. Nothing in the video service scales 4K down to the 1920×1080 the CMIO extension offers. That
  scale happens elsewhere (main app or extension); UNKNOWN.
- **`FXAAEnable` enum case names** are not in the metadata. "Off for IMX378" rests on both sensor tables storing
  case 0 while the check compares with case 1, and on the app passing those tables in.
- **Face coordinate space.** The `face:` argument of `processFrameWithPreviewBuffer` comes from the device service.
  Whether its boxes are relative to the full camera frame, with no letterbox correction, was not verified. The
  70-px Kalman gate assumes pixel coordinates at camera resolution.
- **IntroFilter / video-off.** `VideoService.vt133` steps `opacity` by ±1/15 per frame. How opacity drives
  IntroFilter (Gaussian blur and fade) was not traced.
- **Watermark, footer and bubble overlay geometry** (MultilayerCompositingFilter layers) were not transcribed.
- **Zebra, histogram and focus-rect shaders** (m011, m005/m006) were not transcribed. They are preview-only.
- **CLAHE and vibrance parameters** are not covered here; see the touch-up/skin spec for the other FiltersTab
  filters.
- **The SIMD lane fill of the zoom vector in vt42** is INFERRED as `SIMD2(repeating: currentZoomIn)`. It must be,
  for the height to zoom; the store index comes from a loop counter.
- **Slider ranges** in the main app's SwiftUI views were not read. The ranges in §5 are inferred from stored values.

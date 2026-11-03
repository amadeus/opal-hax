# Opal bokeh + person mask (OpalCameraVideoService, Opal 0.26) — reverse-engineered spec

All OBSERVED in the arm64 slice of `OpalCameraVideoService` / its `default.metallib` (IR in ../bokeh/bc), unless marked.

## Mask source (device service → video service)
- Device service `+[ImageHelpers fillSurfaceBuffer:fromGoogleMeetBuffer:]` (0x1000252a4) copies the NN layer as raw
  FP16 into a 256 x 288 surface, 512 B/row: rows 0..143 = channel 0, rows 144..287 = channel 1 (CHW planes). No softmax.
- `SegmenterFilter` (video service, apply at 0x10003fe18, active when global 0x1000db4c8 == 1):
  1. `ch0 = mask.cropped(to: (0,0,256,144), outputPixelFormat: .r8Unorm)`  (MTIImage.cropped → 0x1000CC560)
     `ch1 = mask.cropped(to: (0,144,256,144), .r8Unorm)` → values clamped to [0,1] and quantized to 8 bit.
  2. gray surface blur (`surfaceBlurFilter`, radius 8, gray, threshold 100): inputImage(guide) = ch0, inputMask(values) = ch1,
     output R8Unorm 256x144.
  3. color surface blur (`surfaceBlurFilterColor`, radius 8, color, threshold 75): inputImage(guide) = camera RGB image
     `.resized(to: 256x144)` (MTIImage.resized → bilinear), inputMask = output of 2, output R8Unorm 256x144.
  Samplers: clamp-to-edge (defaultSamplerDescriptorWithAddressMode:0), linear.
- The result is passed to bokeh as `MTIMask(content: mask, component: 0 (red), mode: .oneMinusMaskValue)` (0x10001f06c passes 1).
  → channel 1 = person.

## MTISurfaceBlurFilter (ObjC, 0x10000b184 init, 0x10000b498 outputImage)
- function constant `mtiSurfaceBlurKernelRadius` = R (8).
- weights[16]: for k in 0..R-1: w = exp(-k^2 / (2 R^2)) / sqrt(2*pi*R^2); stored at index R-1±k (15 used, [15] = 0).
- bsigma = 2 * threshold / 255   (gray: 200/255 = 0.784314; color: 150/255 = 0.588235)
- outputImage = kernel.apply([inputImage, inputMask], {weights, bsigma}, dims = inputImage.dimensions, format = outputPixelFormat)
- shader (both variants; m014/m015):
  ```
  g0 = guide(uv)            // gray: .r ; color: .rgb
  for i in -(R-1)..(R-1)  (outer, x)    for j in -(R-1)..(R-1) (inner, y):
     p = uv + float2(i, j) * float2(1/256, 1/144)        // HARD-CODED texel size
     d = guide(p) - g0                                    // gray: scalar, used as float3(d,d,d) → dot = 3 d^2 ; color: rgb
     wgt = weights[i+R-1] * weights[j+R-1] * exp(-0.5 * dot(d,d) / bsigma^2)
     num += wgt * value(p).r ; den += wgt
  out = num / den  (color variant returns float4(out,out,out, ?) into an R8 target)
  ```
  (the leading normalisation constants in the IR cancel to 1.)

## MTIBokehFilter (ObjC, outputImage 0x100008744) — hexagonal lens blur
- radius <= 0 → return input. No mask → white mask.
- for i in 0,1,2: a_i = angle + 2*pi*i/3 (angle = 0 in Opal);  delta_i = (radius*sin(a_i)/W, radius*cos(a_i)/H), W/H = input size.
- power = 10^clamp(brightness, -1, 1).   Opal brightness = 0.5 (default, never changed) → power = 3.16228.
- Pass 1 `bokehBlurPre` → RGBA16F, input size: `rgb = pow(color.rgb, power) (half precision); a = abs(float(invert) - mask.r)`
  (invert = usesOneMinusMaskValue = true for the segmentation mask). Mask sampled with linear filtering (256x144 upsampled).
- Pass 2 `bokehBlurAlpha` → two RGBA16F outputs, input size, deltas (delta0, delta1); sampler = input image's (linear, clamp).
- Pass 3 `bokehBlurBravoCharlie` inputs (out0, out1), deltas (delta1, delta2), `power = 1/power` → outputPixelFormat
  (Opal sets BGRA8Unorm, 0x50).
- Shared 16-tap sampler (Alpha and BravoCharlie identical):
  ```
  float4 blurDir(tex, uv, float2 d /* = coc * delta */):
     rnd = fract(sin(dot(uv, d)) * 43758.5453)        // dot(float3(uv,0), float3(d,151.7182)) == dot(uv,d)
     step = d / 16
     s[k] = tex.sample(uv + step * (rnd + k)), k = 0..15
     total = sum over k in {3,7,11,15} of (s[k].r + s[k].g + s[k].b + s[k].a)
     if total < 0.01: return center sample
     acc = sum over blocks b=0..3: s[4b+3].x*s[4b] + s[4b+3].y*s[4b+1] + s[4b+3].z*s[4b+2] + s[4b+3].w*s[4b+3]
     return float4(acc.rgb / total, 1)
  ```
  (literal transcription of the IR — weights come from every 4th sample's components; looks like an upstream bug
  (float4x4 column vs row) but it's what Opal renders.)
  - Alpha: c = tex(uv); coc = c.a; v = blurDir(tex, uv, coc*delta0) [fallback c]; dg = blurDir(tex, uv, coc*delta1) [fallback c]
    out0 = float4(v.rgb, c.a); out1 = float4(v.rgb + dg.rgb, c.a)
  - BravoCharlie: c = texV(uv); coc = c.a; a = blurDir(texV, uv, coc*deltaA) [fallback c];
    b = blurDir(texD, uv, coc*deltaB) [fallback texD(uv)]; out = float4(pow((a + b).rgb / 3, power), 1)
    (deltaA/deltaB = delta1/delta2 from the host).
- Constants: 151.7182 (0x4062F6FB80000000), 43758.5453 (0x40E55DD180000000), 0.01 (0x3F847AE140000000), 1/3.

## Swift BokehFilter (OpalCameraVideoService/BokehFilter.swift)
- fields: isActive@16, isPassthrough@17, filter(MTIBokehFilter)@24, radius(Float)@32 default 7.6, brightness(Float)@36 default 0.5,
  depthPlane@40 default 0.5, depthMask@48, thresholdFilter@56, thresholdFStop@64.
- apply (0x10001f0b4): filter.inputImage = image; inputMask = MTIMask(mask, component 0, mode); brightness; radius;
  outputPixelFormat = BGRA8Unorm; outputImage.
- `setBokehRadiusWithRadius:` (XPC, 0x10005983c) → CameraPipeline (0x100091c5c) → bokehFilter.radius = value, unchanged.
- Main app sends `Float(OCBokeh)` unchanged (0x10001a978 / 0x100216684). The owner's OCBokeh = 11.
- UNKNOWN: OCBokeh slider min/max (OCSliderView range not located). Radius is in input-image pixels.
- Depth-based variant (thresholdFilter, adjustFocusPlaneToDepth) is for stereo depth cameras; not used on the C1.

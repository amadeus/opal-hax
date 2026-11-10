#include "effects.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace opal {

// All passes in one HLSL source, compiled at startup with D3DCompile (d3dcompiler_47.dll ships with Windows) and cached.
// Every pass is a transcription of the Opal / MetalPetal / Core Image shader named next to it; the reverse-engineered
// specs are summarised in tools/win/verify/README.md and checked against Opal's Metal shaders by shader-check.m.
static const char* kShaders = R"HLSL(
cbuffer P : register(b0) {
    float2 size;     // size of this pass's output
    float2 srcSize;  // size of the input texture where it differs
    float4 v0, v1, v2;
    float4 wts[4];   // surface-blur spatial weights
};
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);
#define GUARD if (id.x >= (uint)size.x || id.y >= (uint)size.y) return; float2 uv = (id.xy + 0.5) / size;

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float> val1 : register(t1);
Texture1D<float4> curve : register(t3);
Texture2D<float> mask4 : register(t4);
Texture3D<float4> lut3 : register(t5);
Texture2D<float> inY : register(t0);
Texture2D<float2> inUV : register(t1);
RWTexture2D<float4> o0 : register(u0);
RWTexture2D<float4> o1 : register(u1);
RWTexture2D<float> o0f : register(u0);
RWTexture2D<float2> o0uv : register(u0);

// ---- NV12 -> RGB: yuv2rgbconvert_opal_custom (BT.709 full range, chroma bilinear at the same uv)
[numthreads(8, 8, 1)]
void CSToRgb(uint3 id : SV_DispatchThreadID) {
    GUARD
    float y = inY.SampleLevel(linearClamp, uv, 0);
    float2 c = inUV.SampleLevel(linearClamp, uv, 0) - 0.5;
    float3 rgb = y + c.x * float3(0.0, -0.187324002, 1.855599999) + c.y * float3(1.574800014, -0.468124002, 0.0);
    o0[id.xy] = float4(saturate(rgb), 1);
}

// ---- separable gaussian, normalized, clamp to edge (stand-in for MPSImageGaussianBlur; measured: same weights)
// v0.xy: uv step, v0.z: taps each side, v0.w: sigma (texels)
[numthreads(8, 8, 1)]
void CSGauss(uint3 id : SV_DispatchThreadID) {
    GUARD
    int r = (int)v0.z;
    float k = -0.5 / (v0.w * v0.w);
    float4 acc = 0;
    float ws = 0;
    [loop] for (int i = -r; i <= r; i++) {
        float w = exp(k * i * i);
        acc += w * t0.SampleLevel(pointClamp, uv + v0.xy * i, 0);
        ws += w;
    }
    o0[id.xy] = acc / ws;
}

// ---- touch-up: highPassSkinSmoothingGBChannelOverlay
[numthreads(8, 8, 1)]
void CSTouchOverlay(uint3 id : SV_DispatchThreadID) {
    GUARD
    float3 c = t0[id.xy].rgb * 0.5;
    float v = c.g < 0.5 ? 2 * c.g * c.b : 1 - 2 * (1 - c.g) * (1 - c.b);
    o0[id.xy] = float4(saturate(v).xxx, 1);
}
// ---- touch-up: highPassSkinSmoothingMaskProcessAndComposite (t0 image, t1 overlay, t2 blurred overlay; v0.x amount)
[numthreads(8, 8, 1)]
void CSTouchComposite(uint3 id : SV_DispatchThreadID) {
    GUARD
    float4 img = t0[id.xy];
    float m = mask4.SampleLevel(linearClamp, uv, 0);
    float4 tc = float4(curve.SampleLevel(linearClamp, img.r * 0.99609375 + 0.001953125, 0).r,
                       curve.SampleLevel(linearClamp, img.g * 0.99609375 + 0.001953125, 0).g,
                       curve.SampleLevel(linearClamp, img.b * 0.99609375 + 0.001953125, 0).b, img.a);
    float4 mixed = lerp(img, tc, v0.x);
    float x = t1[id.xy].r - t2[id.xy].r + 0.5;
    [unroll] for (int k = 0; k < 3; k++) x = saturate(x < 0.5 ? 2 * x * x : 1 - 2 * (1 - x) * (1 - x));
    float kk = saturate((x - 0.29411766) * 2.8651686);
    o0[id.xy] = lerp(img, lerp(mixed, img, kk), m);
}

// ---- skin tone: skintoneadjustment::extractSkin (measurement source)
[numthreads(8, 8, 1)]
void CSExtractSkin(uint3 id : SV_DispatchThreadID) {
    GUARD
    float4 c = t0[id.xy];
    if (mask4.SampleLevel(linearClamp, uv, 0) < 0.5) { o0[id.xy] = 0; return; }
    float4 cc = clamp(c, 0.0001, 0.9999);
    float sum = cc.r + cc.g + cc.b;
    float mean = sum * 0.33333, invMean = 3.00003 / sum;
    float sat = max((cc.r - mean) * (1 / (1 - mean)), (mean - cc.r) * invMean);
    float lk = saturate(4 * min(cc.r - cc.g, 2 * cc.g - cc.b) * (1 - sat) * invMean);
    o0[id.xy] = lk > 0.3 ? c : 0;
}
// ---- skin tone: CIFaceBalance (rgb^0.25 space, v0.xy = strength * (target IQ - measured IQ)) blended by the mask
[numthreads(8, 8, 1)]
void CSFaceBalance(uint3 id : SV_DispatchThreadID) {
    GUARD
    float4 src = t0[id.xy];
    float3 p = pow(max(0, src.rgb), 0.25);
    float Y = 0.299 * p.r + 0.587 * p.g + 0.114 * p.b;
    float I = 0.595716 * p.r - 0.274453 * p.g - 0.321263 * p.b;
    float Q = 0.211456 * p.r - 0.522591 * p.g + 0.311135 * p.b;
    float s = min(1.0, 4.0 * (I * I + Q * Q));
    float w = pow(s, 0.2) * (1 - s * s);
    I += w * v0.x;
    Q += w * v0.y;
    float3 o = float3(Y + 0.956296 * I + 0.621024 * Q, Y - 0.272122 * I - 0.647381 * Q, Y - 1.10699 * I + 1.70461 * Q);
    o = round(saturate(pow(max(0, o), 4.0)) * 255) / 255;  // Core Image renders BGRA8
    o0[id.xy] = float4(lerp(src.rgb, o, mask4.SampleLevel(linearClamp, uv, 0)), src.a);
}

// ---- 2x2 box downsample (for the sigma-30 blur of highlights/shadows, done at half size)
[numthreads(8, 8, 1)]
void CSDown2(uint3 id : SV_DispatchThreadID) {
    GUARD
    uint2 p = min(id.xy * 2, (uint2)srcSize - 2);
    o0[id.xy] = (t0[p] + t0[p + uint2(1, 0)] + t0[p + uint2(0, 1)] + t0[p + uint2(1, 1)]) * 0.25;
}
// ---- highlights & shadows: shadowhighlight::shadowHighlightAdjust (t1 = sigma-30 blur; v0.x shadow, v0.y highlight)
float hsBlend(float la, float lb) { return la > 0.5 ? 1 - (1 - 2 * (la - 0.5)) * (0.5 - lb) : 2 * la * (lb + 0.5); }
float signOf(float x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }
[numthreads(8, 8, 1)]
void CSHighlightShadow(uint3 id : SV_DispatchThreadID) {
    GUARD
    float shadow = v0.x, highlight = v0.y;
    float4 src = t0[id.xy];
    float3 bl = t1.SampleLevel(linearClamp, uv, 0).rgb;
    float3 s = sqrt(max(src.rgb, 0)), t = sqrt(max(bl, 0));
    float3 ta = float3(dot(s, float3(0.299, 0.587, 0.114)), dot(s, float3(0.596, -0.2755, -0.321)), dot(s, float3(0.212, -0.523, 0.311)));
    float Yb = dot(t, float3(0.299, 0.587, 0.114));
    float tb0 = 1 - Yb;
    float sgnH = highlight > 0 ? -1 : 1, sgnS = shadow > 0 ? 1 : -1;
    if (tb0 < 0.5) {
        float h2 = highlight * highlight, xform = min(1 - 2 * tb0, 1), lb0 = (0.5 - Yb) * sgnH;
        [loop] while (h2 > 0) {
            float la = ta.x, li = 1 - la, lb = lb0 * signOf(li);
            float href = (abs(li) > 0.01 ? 1 / abs(li) : 100) * (li < 0 ? -1 : 1);
            float op = min(h2, 1) * xform;
            h2 -= 1;
            float keep = 1 - op;
            ta.x = hsBlend(la, lb) * op + keep * la;
            ta.yz *= (1 - ta.x) * href * op + keep;
        }
    }
    if (tb0 > 0.5) {
        float s2 = shadow * shadow, xform = min(2 * tb0 - 1, 1), lb0 = (0.5 - Yb) * sgnS;
        [loop] while (s2 > 0) {
            float la = ta.x, lb = lb0 * signOf(1 - la);
            float lref = (abs(la) > 0.01 ? 1 / abs(la) : 100) * (la < 0 ? -1 : 1);
            float op = min(s2, 1) * xform;
            s2 -= 1;
            float keep = 1 - op;
            ta.x = hsBlend(la, lb) * op + keep * la;
            ta.yz *= ta.x * lref * op + keep;
        }
    }
    float3 rgb = float3(dot(ta, float3(1.00048006, 0.955558002, 0.619548976)), dot(ta, float3(0.999863982, -0.271544993, -0.646785975)),
                        dot(ta, float3(0.999445975, -1.10802996, 1.70542002)));
    o0[id.xy] = float4(max(rgb, 0) * max(rgb, 0), src.a);
}

// ---- pixellate: MetalPetal pixellate (whole frame, v0.x = block size in pixels, grid anchored top-left)
[numthreads(8, 8, 1)]
void CSPixellate(uint3 id : SV_DispatchThreadID) {
    GUARD
    float2 p = size * uv;
    float2 q = (p - fmod(p, v0.xx) + 0.5 * v0.xx) / size;
    o0[id.xy] = t0.SampleLevel(linearClamp, q, 0);
}

// ---- person mask: SegmenterFilter. Camera frame -> 256x144 guide (MTIImage.resized: one bilinear tap per pixel)
[numthreads(8, 8, 1)]
void CSResizeGuide(uint3 id : SV_DispatchThreadID) {
    GUARD
    o0[id.xy] = t0.SampleLevel(linearClamp, uv, 0);
}
// surface blur (joint bilateral, radius 8, 15x15 taps on the 256x144 grid): t0 guide, t1 values; v0.x bsigma, v0.y 1 = colour guide
float sbW(int i) { return wts[i >> 2][i & 3]; }
[numthreads(8, 8, 1)]
void CSSurfaceBlur(uint3 id : SV_DispatchThreadID) {
    GUARD
    const int R = 8;
    float3 g0 = t0.SampleLevel(linearClamp, uv, 0).rgb;
    float num = 0, den = 0, k = -0.5 / (v0.x * v0.x);
    [loop] for (int i = 1 - R; i < R; i++) {
        [loop] for (int j = 1 - R; j < R; j++) {
            float2 p = uv + float2(i, j) * float2(1.0 / 256.0, 1.0 / 144.0);
            float3 g = t0.SampleLevel(linearClamp, p, 0).rgb;
            float3 d = v0.y > 0.5 ? g - g0 : (g.rrr - g0.rrr);
            float w = sbW(i + R - 1) * sbW(j + R - 1) * exp(k * dot(d, d));
            num += w * val1.SampleLevel(linearClamp, p, 0);
            den += w;
        }
    }
    o0f[id.xy] = num / den;
}

// ---- bokeh: MTIBokehFilter (hexagonal lens blur). Pre: v0.x power, v0.y invert mask
[numthreads(8, 8, 1)]
void CSBokehPre(uint3 id : SV_DispatchThreadID) {
    GUARD
    float3 c = t0.SampleLevel(linearClamp, uv, 0).rgb;
    o0[id.xy] = float4(pow(c, v0.x), abs(v0.y - mask4.SampleLevel(linearClamp, uv, 0)));
}
// 16-tap directional sampler; the weights are every 4th tap's components (Opal's shader does exactly this)
bool blurDir(Texture2D<float4> t, float2 uv, float2 d, out float3 res) {
    float rnd = frac(sin(dot(uv, d)) * 43758.5453);
    float2 st = d / 16.0;
    float4 acc = 0;
    float total = 0;
    [unroll] for (int b = 0; b < 4; b++) {
        float4 s0 = t.SampleLevel(linearClamp, uv + st * (rnd + 4 * b + 0), 0);
        float4 s1 = t.SampleLevel(linearClamp, uv + st * (rnd + 4 * b + 1), 0);
        float4 s2 = t.SampleLevel(linearClamp, uv + st * (rnd + 4 * b + 2), 0);
        float4 s3 = t.SampleLevel(linearClamp, uv + st * (rnd + 4 * b + 3), 0);
        acc += s3.x * s0 + s3.y * s1 + s3.z * s2 + s3.w * s3;
        total += dot(s3, 1);
    }
    res = acc.rgb / total;
    return total >= 0.01;
}
// Alpha: v1.xy delta0, v1.zw delta1 -> o0 vertical, o1 diagonal
[numthreads(8, 8, 1)]
void CSBokehAlpha(uint3 id : SV_DispatchThreadID) {
    GUARD
    float4 c = t0.SampleLevel(linearClamp, uv, 0);
    float3 a, b;
    if (!blurDir(t0, uv, c.a * v1.xy, a)) a = c.rgb;
    if (!blurDir(t0, uv, c.a * v1.zw, b)) b = c.rgb;
    o0[id.xy] = float4(a, c.a);
    o1[id.xy] = float4(a + b, c.a);
}
// BravoCharlie: t0 vertical, t1 diagonal; v1.xy delta1, v1.zw delta2; v0.x = 1/power
[numthreads(8, 8, 1)]
void CSBokehBravoCharlie(uint3 id : SV_DispatchThreadID) {
    GUARD
    float4 c = t0.SampleLevel(linearClamp, uv, 0);
    float3 a, b;
    if (!blurDir(t0, uv, c.a * v1.xy, a)) a = c.rgb;
    if (!blurDir(t1, uv, c.a * v1.zw, b)) b = t1.SampleLevel(linearClamp, uv, 0).rgb;
    o0[id.xy] = float4(pow((a + b) / 3.0, v0.x), 1);
}

// ---- Face Lock / Zoom: MetalPetal crop (size = round(C.wh), exact texture coords) then resize back (both bilinear)
// crop: v0 = C (pixels x, y, w, h), v1.xy = rounded size, srcSize = frame size
[numthreads(8, 8, 1)]
void CSCrop(uint3 id : SV_DispatchThreadID) {
    GUARD
    float2 src = (v0.xy + (id.xy + 0.5) / v1.xy * v0.zw) / srcSize;
    o0[id.xy] = t0.SampleLevel(linearClamp, src, 0);
}
// resize the v1.xy-sized top-left part of t0 to the frame (bilinear, clamped to that part)
[numthreads(8, 8, 1)]
void CSUncrop(uint3 id : SV_DispatchThreadID) {
    GUARD
    float2 p = uv * v1.xy - 0.5;
    float2 f = frac(p);
    int2 i0 = (int2)floor(p), mx = (int2)v1.xy - 1;
    int2 a = clamp(i0, 0, mx), b = clamp(i0 + 1, 0, mx);
    float4 top = lerp(t0[int2(a.x, a.y)], t0[int2(b.x, a.y)], f.x), bot = lerp(t0[int2(a.x, b.y)], t0[int2(b.x, b.y)], f.x);
    o0[id.xy] = lerp(top, bot, f.y);
}
// ---- spotlight: circle::overlayMask (v0.x radius, v0.yz centre); outside the circle goes black
[numthreads(8, 8, 1)]
void CSSpotlight(uint3 id : SV_DispatchThreadID) {
    GUARD
    float3 c = t0.SampleLevel(linearClamp, uv, 0).rgb;
    float2 d = float2((uv.x - v0.y) * 1.777777791, uv.y - v0.z);
    float t = saturate((4 * dot(d, d) - v0.x * 0.9900000095) / (v0.x * 0.01999998093));
    o0[id.xy] = float4(c * (1 - t * t * (3 - 2 * t)), 1);
}
// ---- mirror (CGImagePropertyOrientation.upMirrored)
[numthreads(8, 8, 1)]
void CSMirror(uint3 id : SV_DispatchThreadID) {
    GUARD
    o0[id.xy] = t0[uint2((uint)size.x - 1 - id.x, id.y)];
}
// ---- premultiplied overlay over the frame (t1 = 1920x1080 overlay)
[numthreads(8, 8, 1)]
void CSOverlay(uint3 id : SV_DispatchThreadID) {
    GUARD
    float4 o = t1.SampleLevel(linearClamp, uv, 0);
    o0[id.xy] = float4(o.rgb + t0[id.xy].rgb * (1 - o.a), 1);
}
// ---- look: lerp(img, look(img), OCFilters) (MetalPetal normal blend); v0.x strength, v0.y LUT size
[numthreads(8, 8, 1)]
void CSLook(uint3 id : SV_DispatchThreadID) {
    GUARD
    float3 c = t0[id.xy].rgb;
    float n = v0.y;
    float3 l = lut3.SampleLevel(linearClamp, c * ((n - 1) / n) + 0.5 / n, 0).rgb;
    l = round(saturate(l) * 255) / 255;
    o0[id.xy] = float4(lerp(c, l, v0.x), 1);
}
// ---- look thumbnails (the popover's Looks strip, like Opal's FilterImageListView: every look live on the current frame)
// CSThumbDown: box-filtered downscale of the frame before the look; v0.x bilinear taps per axis per output pixel
[numthreads(8, 8, 1)]
void CSThumbDown(uint3 id : SV_DispatchThreadID) {
    GUARD
    int n = (int)v0.x;
    float4 acc = 0;
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++)
        acc += t0.SampleLevel(linearClamp, (id.xy + (float2(x, y) + 0.5) / n) / size, 0);
    o0[id.xy] = float4(acc.rgb / (n * n), 1);
}
// CSLookThumb: one atlas tile at x offset v0.z; v0.w > 0: through the look (full strength, 8-bit like CSLook), else as is
[numthreads(8, 8, 1)]
void CSLookThumb(uint3 id : SV_DispatchThreadID) {
    GUARD
    float3 c = t0[id.xy].rgb;
    if (v0.w > 0) {
        float n = v0.y;
        c = round(saturate(lut3.SampleLevel(linearClamp, c * ((n - 1) / n) + 0.5 / n, 0).rgb) * 255) / 255;
    }
    o0[id.xy + uint2((uint)v0.z, 0)] = float4(c, 1);
}
// ---- frame -> 1920x1080 output (bilinear; a copy when the camera runs at 1080p)
[numthreads(8, 8, 1)]
void CSFinal(uint3 id : SV_DispatchThreadID) {
    GUARD
    o0[id.xy] = t0.SampleLevel(linearClamp, uv, 0);
}
// ---- RGB -> Y: rgb2yuvconvert_opal_custom::colorConversionFragmentY (full range)
[numthreads(8, 8, 1)]
void CSToY(uint3 id : SV_DispatchThreadID) {
    GUARD
    o0f[id.xy] = dot(t0[id.xy].rgb, float3(0.2125999928, 0.7152000070, 0.0722000003));
}
// ---- RGB -> CbCr: colorConversionFragmentCbCr per pixel (8-bit), then the 2x2 mean (MTIImage.resized to half size)
[numthreads(8, 8, 1)]
void CSToUV(uint3 id : SV_DispatchThreadID) {
    GUARD
    float2 s = 0;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
        float3 c = t0[id.xy * 2 + uint2(x, y)].rgb;
        float cb = dot(float3(-0.1145720035, -0.3854230046, 0.5), c) + 0.5;
        float cr = dot(float3(0.5, -0.4541499913, -0.0458469987), c) + 0.5;
        s += round(saturate(float2(cb, cr)) * 255);
    }
    o0uv[id.xy] = s / (4 * 255);
}
)HLSL";

struct alignas(16) CB {
    float size[2] = {}, srcSize[2] = {};
    float v0[4] = {}, v1[4] = {}, v2[4] = {};
    float wts[16] = {};
};

static std::string gCacheDir;

static uint64_t fnv1a(const char* s, size_t n, uint64_t h = 1469598103934665603ULL) {
    for(size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
    return h;
}

static bool compile(ID3D11Device* dev, const char* entry, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& cs, std::string& err) {
    // compiled bytecode is cached per (source, entry point)
    uint64_t h = fnv1a(entry, strlen(entry), fnv1a(kShaders, strlen(kShaders)));
    char name[96];
    snprintf(name, sizeof name, "%s-%016llx.cso", entry, (unsigned long long)h);
    std::string path = gCacheDir.empty() ? "" : gCacheDir + "\\" + name;
    if(!path.empty()) {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
        std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if(!b.empty() && SUCCEEDED(dev->CreateComputeShader(b.data(), b.size(), nullptr, &cs))) return true;
    }
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kShaders, strlen(kShaders), "opal_effects.hlsl", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &errors);
    if(FAILED(hr)) {
        err = std::string("shader ") + entry + ": " + (errors ? (const char*)errors->GetBufferPointer() : "compile failed");
        return false;
    }
    if(!path.empty()) {
        std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
        f.write((const char*)code->GetBufferPointer(), (std::streamsize)code->GetBufferSize());
    }
    return SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
}

bool Effects::makeTex(Tex& t, int w, int h, DXGI_FORMAT fmt, UINT bind) {
    D3D11_TEXTURE2D_DESC d = {};
    if(t.t) {
        t.t->GetDesc(&d);
        if((int)d.Width == w && (int)d.Height == h && d.Format == fmt) return true;
    }
    d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    t = Tex();
    if(FAILED(dev_->CreateTexture2D(&d, nullptr, &t.t))) return false;
    if((bind & D3D11_BIND_SHADER_RESOURCE) && FAILED(dev_->CreateShaderResourceView(t.t.Get(), nullptr, &t.srv))) return false;
    if((bind & D3D11_BIND_UNORDERED_ACCESS) && FAILED(dev_->CreateUnorderedAccessView(t.t.Get(), nullptr, &t.uav))) return false;
    t.w = w;
    t.h = h;
    return true;
}

// touch-up tone curve (MTIRGBToneCurveFilter with points (0,0) (120/255,146/255) (1,1); GPUImage spline, truncated)
static const uint8_t kTouchUpCurve[256] = {
    0,   1,   2,   3,   5,   6,   7,   9,   10,  11,  13,  14,  15,  17,  18,  19,  20,  22,  23,  24,  26,  27,  28,  30,  31,  32,
    34,  35,  36,  37,  39,  40,  41,  43,  44,  45,  46,  48,  49,  50,  52,  53,  54,  55,  57,  58,  59,  61,  62,  63,  64,  66,
    67,  68,  69,  71,  72,  73,  74,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,  92,  93,  94,  95,  96,  98,
    99,  100, 101, 102, 103, 105, 106, 107, 108, 109, 110, 112, 113, 114, 115, 116, 117, 118, 120, 121, 122, 123, 124, 125, 126, 127,
    129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155,
    156, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 180, 181, 182, 183, 184, 185, 186, 186, 187, 188, 189, 190, 191, 191, 192, 193, 194, 195, 196, 196, 197, 198, 199, 200, 200,
    201, 202, 203, 204, 204, 205, 206, 207, 207, 208, 209, 210, 211, 211, 212, 213, 214, 214, 215, 216, 217, 217, 218, 219, 220, 220,
    221, 222, 223, 223, 224, 225, 225, 226, 227, 228, 228, 229, 230, 230, 231, 232, 233, 233, 234, 235, 236, 236, 237, 238, 238, 239,
    240, 240, 241, 242, 243, 243, 244, 245, 245, 246, 247, 248, 248, 249, 250, 250, 251, 252, 252, 253, 254, 255};

bool Effects::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::string& lutDir, const std::string& cacheDir, std::string& err) {
    gCacheDir = cacheDir;
    if(!cacheDir.empty()) std::filesystem::create_directories(std::filesystem::u8path(cacheDir));
    dev_ = dev;
    ctx_ = ctx;
    struct {
        const char* name;
        Com<ID3D11ComputeShader>* cs;
    } shaders[] = {{"CSToRgb", &csToRgb_},
                   {"CSTouchOverlay", &csTouchOverlay_},
                   {"CSGauss", &csGauss_},
                   {"CSTouchComposite", &csTouchComposite_},
                   {"CSExtractSkin", &csExtractSkin_},
                   {"CSFaceBalance", &csFaceBalance_},
                   {"CSDown2", &csDown2_},
                   {"CSHighlightShadow", &csHS_},
                   {"CSPixellate", &csPixellate_},
                   {"CSResizeGuide", &csResizeGuide_},
                   {"CSSurfaceBlur", &csSurfaceBlur_},
                   {"CSBokehPre", &csBokehPre_},
                   {"CSBokehAlpha", &csBokehAlpha_},
                   {"CSBokehBravoCharlie", &csBokehBC_},
                   {"CSCrop", &csCrop_},
                   {"CSUncrop", &csUncrop_},
                   {"CSSpotlight", &csSpotlight_},
                   {"CSMirror", &csMirror_},
                   {"CSOverlay", &csOverlay_},
                   {"CSLook", &csLook_},
                   {"CSThumbDown", &csThumbDown_},
                   {"CSLookThumb", &csLookThumb_},
                   {"CSFinal", &csFinal_},
                   {"CSToY", &csToY_},
                   {"CSToUV", &csToUV_}};
    for(auto& s : shaders)
        if(!compile(dev, s.name, *s.cs, err)) return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if(FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) return err = "constant buffer", false;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &linear_);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &point_);

    UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if(!makeTex(out_, kOutW, kOutH, DXGI_FORMAT_R8G8B8A8_UNORM, rw) || !makeTex(outY_, kOutW, kOutH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS) ||
       !makeTex(outUV_, kOutW / 2, kOutH / 2, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_UNORDERED_ACCESS) ||
       !makeTex(overlay_[0], kOutW, kOutH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
       !makeTex(overlay_[1], kOutW, kOutH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
       !makeTex(guide_, 256, 144, DXGI_FORMAT_R8G8B8A8_UNORM, rw) || !makeTex(maskA_, 256, 144, DXGI_FORMAT_R8_UNORM, rw) ||
       !makeTex(mask_, 256, 144, DXGI_FORMAT_R8_UNORM, rw))
        return err = "texture allocation failed", false;
    outSRV_ = out_.srv;
    D3D11_TEXTURE2D_DESC st = {};
    st.Width = kOutW;
    st.Height = kOutH;
    st.MipLevels = st.ArraySize = 1;
    st.Format = DXGI_FORMAT_R8_UNORM;
    st.SampleDesc.Count = 1;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&st, nullptr, &stY_);
    st.Width = kOutW / 2;
    st.Height = kOutH / 2;
    st.Format = DXGI_FORMAT_R8G8_UNORM;
    dev->CreateTexture2D(&st, nullptr, &stUV_);
    // mask planes (uploaded per frame)
    D3D11_TEXTURE2D_DESC md = {};
    md.Width = 256;
    md.Height = 144;
    md.MipLevels = md.ArraySize = 1;
    md.Format = DXGI_FORMAT_R8_UNORM;
    md.SampleDesc.Count = 1;
    md.Usage = D3D11_USAGE_DYNAMIC;
    md.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    md.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if(FAILED(dev->CreateTexture2D(&md, nullptr, &inM0_)) || FAILED(dev->CreateTexture2D(&md, nullptr, &inM1_))) return err = "mask textures", false;
    dev->CreateShaderResourceView(inM0_.Get(), nullptr, &inM0SRV_);
    dev->CreateShaderResourceView(inM1_.Get(), nullptr, &inM1SRV_);
    // touch-up tone curve
    {
        uint8_t rgba[256 * 4];
        for(int i = 0; i < 256; i++) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = kTouchUpCurve[i], rgba[i * 4 + 3] = 255;
        D3D11_TEXTURE1D_DESC d = {};
        d.Width = 256;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {rgba, sizeof rgba, 0};
        Com<ID3D11Texture1D> t;
        if(FAILED(dev->CreateTexture1D(&d, &init, &t)) || FAILED(dev->CreateShaderResourceView(t.Get(), nullptr, &curveSRV_))) return err = "tone curve", false;
    }
    // clear the overlays
    for(auto& o : overlay_) {
        Com<ID3D11RenderTargetView> rtv;
        if(SUCCEEDED(dev->CreateRenderTargetView(o.t.Get(), nullptr, &rtv))) {
            float zero[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(rtv.Get(), zero);
        }
    }
    loadLuts(lutDir);
    ready_ = true;
    return true;
}

IDXGISurface* Effects::overlaySurface(int which) const {
    IDXGISurface* s = nullptr;
    if(overlay_[which].t) overlay_[which].t->QueryInterface(__uuidof(IDXGISurface), (void**)&s);
    return s;  // caller releases
}

// .lut (cube-to-lut.py): "OPALLUT1", uint32 N, N^3 RGBA float16 (red fastest). Loads straight into the texture.
static bool loadLutBinary(const std::string& path, int& n, std::vector<uint16_t>& rgba) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    char magic[8];
    uint32_t size = 0;
    if(!f.read(magic, 8) || memcmp(magic, "OPALLUT1", 8) != 0 || !f.read((char*)&size, 4) || size < 2 || size > 256) return false;
    n = (int)size;
    rgba.resize((size_t)n * n * n * 4);
    return (bool)f.read((char*)rgba.data(), (std::streamsize)(rgba.size() * 2));
}

// .cube text (Adobe): fallback, parsed from one buffer with strtof (line-by-line streams take tens of seconds)
static bool parseCube(const std::string& path, int& n, std::vector<float>& rgba) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if(!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    n = 0;
    rgba.clear();
    const char* p = text.c_str();
    while(*p) {
        const char* eol = strchr(p, '\n');
        if(!eol) eol = p + strlen(p);
        if(strncmp(p, "LUT_3D_SIZE", 11) == 0) {
            n = atoi(p + 11);
            rgba.reserve((size_t)n * n * n * 4);
        } else if(isdigit((unsigned char)*p) || *p == '-' || *p == '.') {
            char* e;
            float r = strtof(p, &e), g = strtof(e, &e), b = strtof(e, &e);
            rgba.push_back(r);
            rgba.push_back(g);
            rgba.push_back(b);
            rgba.push_back(1.f);
        }
        p = *eol ? eol + 1 : eol;
    }
    return n > 1 && rgba.size() == (size_t)n * n * n * 4;
}

bool Effects::loadLuts(const std::string& dir) {
    std::ifstream f(dir + "/index.json");
    if(!f) return false;
    nlohmann::json idx;
    try {
        f >> idx;
    } catch(...) {
        return false;
    }
    nlohmann::json list = idx.is_array() ? idx : (idx.contains("looks") ? idx["looks"] : nlohmann::json::array());
    // Opal's UI order (bake-luts.sh records it)
    std::vector<nlohmann::json> sorted(list.begin(), list.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a.value("order", 0) < b.value("order", 0); });
    list = sorted;
    for(auto& e : list) {
        std::string name = e.value("name", ""), file = e.value("file", "");
        if(file.empty()) continue;
        int n = 0;
        std::vector<uint16_t> half;
        std::vector<float> full;
        bool binary = file.size() > 4 && file.compare(file.size() - 4, 4, ".lut") == 0;
        if(binary ? !loadLutBinary(dir + "/" + file, n, half) : !parseCube(dir + "/" + file, n, full)) continue;
        D3D11_TEXTURE3D_DESC d = {};
        d.Width = d.Height = d.Depth = n;  // .cube order: red fastest, then green, then blue -> x, y, z
        d.MipLevels = 1;
        d.Format = binary ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        UINT texel = binary ? 8 : 16;
        D3D11_SUBRESOURCE_DATA init = {binary ? (const void*)half.data() : (const void*)full.data(), (UINT)(n * texel), (UINT)(n * n * texel)};
        Com<ID3D11Texture3D> tex;
        Look look;
        look.name = name;
        if(FAILED(dev_->CreateTexture3D(&d, &init, &tex)) || FAILED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &look.lut))) continue;
        looks_.push_back(look);
    }
    return !looks_.empty();
}

bool Effects::ensureFrame(int w, int h) {
    if(w == inW_ && h == inH_) return true;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if(FAILED(dev_->CreateTexture2D(&d, nullptr, inY_.ReleaseAndGetAddressOf()))) return false;
    d.Width = w / 2;
    d.Height = h / 2;
    d.Format = DXGI_FORMAT_R8G8_UNORM;
    if(FAILED(dev_->CreateTexture2D(&d, nullptr, inUV_.ReleaseAndGetAddressOf()))) return false;
    dev_->CreateShaderResourceView(inY_.Get(), nullptr, inYSRV_.ReleaseAndGetAddressOf());
    dev_->CreateShaderResourceView(inUV_.Get(), nullptr, inUVSRV_.ReleaseAndGetAddressOf());
    for(auto& t : fr_)
        if(!makeTex(t, w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    inW_ = w;
    inH_ = h;
    return true;
}

static void upload(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, const uint8_t* src, size_t rowBytes, int rows, size_t srcStride) {
    D3D11_MAPPED_SUBRESOURCE m;
    if(FAILED(ctx->Map(t, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for(int y = 0; y < rows; y++) memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, src + (size_t)y * srcStride, rowBytes);
    ctx->Unmap(t, 0);
}

// Skin tone measurement (SkinAdjustmentFilter): mean colour of the skin pixels inside the inset face box, as Opal gets it
// from CIKMeans (k = 1): the mean of the opaque pixels with alpha = their share of the box, rendered to 8-bit, which
// clamps each colour channel to that alpha (verified with Core Image). Then the YIQ reference values.
void Effects::measureSkin(const Tex& skinTex, const Face& f) {
    origI_ = origQ_ = 0;
    double W = skinTex.w, H = skinTex.h;
    double rx = f.xmin * W, ry = f.ymin * H, rw = (f.xmax - f.xmin) * W, rh = (f.ymax - f.ymin) * H;
    double dx = 0.15 * rw / 2, dy = 0.3 * rh / 2;
    int x0 = std::clamp((int)std::floor(rx + dx), 0, skinTex.w), y0 = std::clamp((int)std::floor(ry + dy), 0, skinTex.h);
    int x1 = std::clamp((int)std::ceil(rx + rw - dx), 0, skinTex.w), y1 = std::clamp((int)std::ceil(ry + rh - dy), 0, skinTex.h);
    if(x1 <= x0 || y1 <= y0) return;
    D3D11_TEXTURE2D_DESC st = {};
    st.Width = x1 - x0;
    st.Height = y1 - y0;
    st.MipLevels = st.ArraySize = 1;
    st.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    st.SampleDesc.Count = 1;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> stage;
    if(FAILED(dev_->CreateTexture2D(&st, nullptr, &stage))) return;
    D3D11_BOX box = {(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
    ctx_->CopySubresourceRegion(stage.Get(), 0, 0, 0, 0, skinTex.t.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if(FAILED(ctx_->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    double sum[3] = {0, 0, 0};
    size_t n = 0, total = (size_t)st.Width * st.Height;
    for(UINT y = 0; y < st.Height; y++) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        for(UINT x = 0; x < st.Width; x++)
            if(row[x * 4 + 3]) {
                for(int c = 0; c < 3; c++) sum[c] += row[x * 4 + c];
                n++;
            }
    }
    ctx_->Unmap(stage.Get(), 0);
    if(!n) return;
    double a = std::round((double)n / total * 255.0) / 255.0, rgb[3];
    for(int c = 0; c < 3; c++) rgb[c] = std::round(std::min(sum[c] / n / 255.0, a) * 255.0);
    origI_ = 0.5959 * rgb[0] / 255 + -0.2746 * rgb[1] / 255 + -0.3213 * rgb[2] / 255;
    origQ_ = 0.2115 * rgb[0] / 255 + -0.5227 * rgb[1] / 255 + 0.3112 * rgb[2] / 255;
}

bool Effects::process(const uint8_t* nv12, int w, int h, const uint8_t* mask, int mw, int mh, const EffectParams& p, bool wantNV12) {
    if(!ready_ || !ensureFrame(w, h)) return false;
    upload(ctx_, inY_.Get(), nv12, w, h, w);
    upload(ctx_, inUV_.Get(), nv12 + (size_t)w * h, w, h / 2, w);  // interleaved UV rows: w bytes = w/2 R8G8 texels
    bool hasMask = mask && mw == 256 && mh == 144;
    if(hasMask) {
        upload(ctx_, inM0_.Get(), mask, 256, 144, 256);
        upload(ctx_, inM1_.Get(), mask + 256 * 144, 256, 144, 256);
    }

    ID3D11SamplerState* samplers[] = {linear_.Get(), point_.Get()};
    ctx_->CSSetSamplers(0, 2, samplers);
    ID3D11Buffer* cbs[] = {cb_.Get()};
    ctx_->CSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* nullSRV[6] = {};
    ID3D11UnorderedAccessView* nullUAV[2] = {};
    auto run = [&](ID3D11ComputeShader* cs, int ow, int oh, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                   std::initializer_list<ID3D11UnorderedAccessView*> uavs, CB c) {
        c.size[0] = (float)ow;
        c.size[1] = (float)oh;
        D3D11_MAPPED_SUBRESOURCE m;
        if(SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &c, sizeof c);
            ctx_->Unmap(cb_.Get(), 0);
        }
        ctx_->CSSetShader(cs, nullptr, 0);
        std::vector<ID3D11ShaderResourceView*> s(srvs);
        std::vector<ID3D11UnorderedAccessView*> u(uavs);
        ctx_->CSSetShaderResources(0, (UINT)s.size(), s.data());
        ctx_->CSSetUnorderedAccessViews(0, (UINT)u.size(), u.data(), nullptr);
        ctx_->Dispatch((UINT)((ow + 7) / 8), (UINT)((oh + 7) / 8), 1);
        ctx_->CSSetShaderResources(0, 6, nullSRV);
        ctx_->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
    };
    int cur = 0;  // fr_[cur] holds the current image
    auto src = [&] { return fr_[cur].srv.Get(); };
    auto dst = [&] { return fr_[1 - cur].uav.Get(); };
    auto flip = [&] { cur = 1 - cur; };
    auto gauss = [&](Tex& in, Tex& tmp, Tex& out, float sigma) {
        CB c;
        c.v0[2] = std::ceil(3.5f * sigma);  // MPS reaches about +-3.5 sigma
        c.v0[3] = sigma;
        c.v0[0] = 1.f / in.w;
        run(csGauss_.Get(), in.w, in.h, {in.srv.Get()}, {tmp.uav.Get()}, c);
        c.v0[0] = 0;
        c.v0[1] = 1.f / in.h;
        run(csGauss_.Get(), in.w, in.h, {tmp.srv.Get()}, {out.uav.Get()}, c);
    };
    const DXGI_FORMAT rgba8 = DXGI_FORMAT_R8G8B8A8_UNORM, rgba16f = DXGI_FORMAT_R16G16B16A16_FLOAT;

    run(csToRgb_.Get(), w, h, {inYSRV_.Get(), inUVSRV_.Get()}, {fr_[cur].uav.Get()}, CB());

    // ---- person mask (SegmenterFilter): runs while bokeh, touch-up or skin tone is on
    bool seg = hasMask && (p.bokeh || p.touchUp || p.skin);
    if(seg) {
        CB c;
        const float R = 8, norm = (float)(1.0 / std::sqrt(2 * 3.14159265358979323846 * R * R));
        for(int k = 0; k < 8; k++) c.wts[7 - k] = c.wts[7 + k] = (float)(norm * (double)std::exp(-(float)(k * k) / (2.f * R * R)));
        run(csResizeGuide_.Get(), 256, 144, {src()}, {guide_.uav.Get()}, CB());
        c.v0[0] = (float)(2.0 * (100.0 / 255.0));  // gray pass: guide = channel 0, values = channel 1
        c.v0[1] = 0;
        run(csSurfaceBlur_.Get(), 256, 144, {inM0SRV_.Get(), inM1SRV_.Get()}, {maskA_.uav.Get()}, c);
        c.v0[0] = (float)(2.0 * (75.0 / 255.0));  // colour pass: guide = camera frame
        c.v0[1] = 1;
        run(csSurfaceBlur_.Get(), 256, 144, {guide_.srv.Get(), maskA_.srv.Get()}, {mask_.uav.Get()}, c);
    }
    ID3D11ShaderResourceView* maskSRV = mask_.srv.Get();

    // ---- touch-up (needs the mask: without it Opal's black default mask makes it a no-op)
    if(p.touchUp && seg && makeTex(aux_[0], w, h, rgba8) && makeTex(aux_[1], w, h, rgba8) && makeTex(aux_[2], w, h, rgba8)) {
        run(csTouchOverlay_.Get(), w, h, {src()}, {aux_[0].uav.Get()}, CB());
        gauss(aux_[0], aux_[1], aux_[2], 8.f);
        CB c;
        c.v0[0] = p.touchUpAmount;
        run(csTouchComposite_.Get(), w, h, {src(), aux_[0].srv.Get(), aux_[2].srv.Get(), curveSRV_.Get(), maskSRV}, {dst()}, c);
        flip();
    }
    // ---- skin tone
    if(p.skin && !skinWasOn_) skinNeedsReload_ = true;
    skinWasOn_ = p.skin;
    if(p.skin && seg) {
        if(skinNeedsReload_ && makeTex(skin_, w, h, rgba8)) {
            run(csExtractSkin_.Get(), w, h, {src(), nullptr, nullptr, nullptr, maskSRV}, {skin_.uav.Get()}, CB());
            if(p.hasFace) measureSkin(skin_, p.face);
            else origI_ = origQ_ = 0;
            if(origI_ != 0 && origQ_ != 0) skinNeedsReload_ = false;
        }
        double warmth = p.skinWarmth, strength = 2.0 * p.skinAmount;
        double tI = (1 - warmth) * 0.07451 + warmth * 0.1333, tQ = (1 - warmth) * 0.02745 + warmth * 0.007843;
        double dI = strength * (tI - origI_), dQ = strength * (tQ - origQ_);
        if(origI_ != 0 && origQ_ != 0 && (std::fabs(dI) >= 0.001 || std::fabs(dQ) >= 0.001)) {
            CB c;
            c.v0[0] = (float)dI;
            c.v0[1] = (float)dQ;
            run(csFaceBalance_.Get(), w, h, {src(), nullptr, nullptr, nullptr, maskSRV}, {dst()}, c);
            flip();
        }
    }
    // ---- highlights & shadows (sigma-30 blur computed at half size)
    if(p.highlightsShadows && makeTex(half_[0], w / 2, h / 2, rgba8) && makeTex(half_[1], w / 2, h / 2, rgba8) &&
       makeTex(half_[2], w / 2, h / 2, rgba8)) {
        CB c;
        c.srcSize[0] = (float)w;
        c.srcSize[1] = (float)h;
        run(csDown2_.Get(), w / 2, h / 2, {src()}, {half_[0].uav.Get()}, c);
        gauss(half_[0], half_[1], half_[2], 15.f);
        CB hs;
        hs.v0[0] = p.shadows;
        hs.v0[1] = p.highlights;
        run(csHS_.Get(), w, h, {src(), half_[2].srv.Get()}, {dst()}, hs);
        flip();
    }
    // ---- pixellate (whole frame)
    if(p.pixellate) {
        static const int divisors[5] = {4, 8, 10, 20, 40};
        CB c;
        c.v0[0] = (float)(p.resolutionMultiple * divisors[std::clamp(p.pixellateLevel, 0, 4)]);
        run(csPixellate_.Get(), w, h, {src()}, {dst()}, c);
        flip();
    }
    // ---- bokeh
    if(p.bokeh && seg && p.bokehRadius > 0 && makeTex(pre_, w, h, rgba16f) && makeTex(vert_, w, h, rgba16f) && makeTex(diag_, w, h, rgba16f)) {
        const float power = std::pow(10.f, 0.5f);  // 10^brightness, Opal's brightness is 0.5
        // Opal's radius is in camera pixels, so at 4K the same setting blurs half as much of the picture as at 1080p.
        // Scale it with the frame height: every resolution looks like Opal at 1080p (the blur's cost doesn't change).
        float radius = p.bokehRadius * (float)h / 1080.f;
        float d[3][2];
        for(int i = 0; i < 3; i++) {
            float a = (float)(i * 2 * 3.14159265358979323846 / 3);
            d[i][0] = radius * std::sin(a) / w;
            d[i][1] = radius * std::cos(a) / h;
        }
        CB c;
        c.v0[0] = power;
        c.v0[1] = 1;  // the person mask is used as one-minus
        run(csBokehPre_.Get(), w, h, {src(), nullptr, nullptr, nullptr, maskSRV}, {pre_.uav.Get()}, c);
        CB a;
        a.v1[0] = d[0][0], a.v1[1] = d[0][1], a.v1[2] = d[1][0], a.v1[3] = d[1][1];
        run(csBokehAlpha_.Get(), w, h, {pre_.srv.Get()}, {vert_.uav.Get(), diag_.uav.Get()}, a);
        CB b;
        b.v0[0] = 1 / power;
        b.v1[0] = d[1][0], b.v1[1] = d[1][1], b.v1[2] = d[2][0], b.v1[3] = d[2][1];
        run(csBokehBC_.Get(), w, h, {vert_.srv.Get(), diag_.srv.Get()}, {dst()}, b);
        flip();
    }
    // ---- Face Lock, then Zoom: crop + resize back
    for(const CropRect* cr : {&p.framing.face, &p.framing.zoom}) {
        if(!cr->on || !makeTex(crop_, w, h, rgba8)) continue;
        int cw = std::clamp((int)std::lround(cr->w), 1, w), ch = std::clamp((int)std::lround(cr->h), 1, h);
        CB c;
        c.srcSize[0] = (float)w, c.srcSize[1] = (float)h;
        c.v0[0] = (float)cr->x, c.v0[1] = (float)cr->y, c.v0[2] = (float)cr->w, c.v0[3] = (float)cr->h;
        c.v1[0] = (float)cw, c.v1[1] = (float)ch;
        run(csCrop_.Get(), cw, ch, {src()}, {crop_.uav.Get()}, c);
        run(csUncrop_.Get(), w, h, {crop_.srv.Get()}, {dst()}, c);
        flip();
    }
    // ---- spotlight
    if(p.framing.spotlight) {
        CB c;
        c.v0[0] = p.framing.spotRadius, c.v0[1] = p.framing.spotX, c.v0[2] = p.framing.spotY;
        run(csSpotlight_.Get(), w, h, {src()}, {dst()}, c);
        flip();
    }
    // ---- mirror
    if(p.mirror) {
        run(csMirror_.Get(), w, h, {src()}, {dst()}, CB());
        flip();
    }
    // ---- logo / footer text, look, gesture drawing
    if(overlayA_) {
        run(csOverlay_.Get(), w, h, {src(), overlay_[0].srv.Get()}, {dst()}, CB());
        flip();
    }
    auto lutSize = [&](int i) {
        Com<ID3D11Resource> r;
        looks_[i].lut->GetResource(&r);
        Com<ID3D11Texture3D> t3;
        r.As(&t3);
        D3D11_TEXTURE3D_DESC d;
        t3->GetDesc(&d);
        return (float)d.Width;
    };
    // ---- look thumbnails: the picture as it is here (before the look), small, then once per look. The last set stays
    // available while they aren't rendered (e.g. back on the Camera tab: no placeholder flash before the next frame).
    if(p.lookThumbs && !looks_.empty() && makeTex(thumbSmall_, kThumbW, kThumbH, rgba8) &&
       makeTex(thumbs_, kThumbW * (int)(looks_.size() + 1), kThumbH, rgba8)) {
        CB c;
        c.v0[0] = (float)std::clamp((int)std::ceil(std::max((float)w / kThumbW, (float)h / kThumbH) / 2), 1, 12);
        run(csThumbDown_.Get(), kThumbW, kThumbH, {src()}, {thumbSmall_.uav.Get()}, c);
        for(int i = 0; i <= (int)looks_.size(); i++) {
            CB t;
            t.v0[2] = (float)(i * kThumbW);
            if(i > 0) t.v0[1] = lutSize(i - 1), t.v0[3] = 1;
            run(csLookThumb_.Get(), kThumbW, kThumbH, {thumbSmall_.srv.Get(), nullptr, nullptr, nullptr, nullptr, i > 0 ? looks_[i - 1].lut.Get() : nullptr},
                {thumbs_.uav.Get()}, t);
        }
        thumbsReady_ = true;
    }
    if(p.look >= 0 && p.look < (int)looks_.size()) {
        CB c;
        c.v0[0] = p.lookStrength;
        c.v0[1] = lutSize(p.look);
        run(csLook_.Get(), w, h, {src(), nullptr, nullptr, nullptr, nullptr, looks_[p.look].lut.Get()}, {dst()}, c);
        flip();
    }
    if(overlayB_) {
        run(csOverlay_.Get(), w, h, {src(), overlay_[1].srv.Get()}, {dst()}, CB());
        flip();
    }
    // ---- output
    run(csFinal_.Get(), kOutW, kOutH, {src()}, {out_.uav.Get()}, CB());
    if(!wantNV12) return true;
    run(csToY_.Get(), kOutW, kOutH, {out_.srv.Get()}, {outY_.uav.Get()}, CB());
    run(csToUV_.Get(), kOutW / 2, kOutH / 2, {out_.srv.Get()}, {outUV_.uav.Get()}, CB());
    ctx_->CopyResource(stY_.Get(), outY_.t.Get());
    ctx_->CopyResource(stUV_.Get(), outUV_.t.Get());
    readbackPending_ = true;  // mapped by readNV12(), after the UI has been presented (Map waits for the GPU)
    return true;
}

bool Effects::readNV12(std::vector<uint8_t>& out) {
    if(!readbackPending_) return false;
    readbackPending_ = false;
    out.resize((size_t)kOutW * kOutH * 3 / 2);
    D3D11_MAPPED_SUBRESOURCE m;
    if(FAILED(ctx_->Map(stY_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    for(int y = 0; y < kOutH; y++) memcpy(out.data() + (size_t)y * kOutW, (uint8_t*)m.pData + (size_t)y * m.RowPitch, kOutW);
    ctx_->Unmap(stY_.Get(), 0);
    if(FAILED(ctx_->Map(stUV_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    uint8_t* uv = out.data() + (size_t)kOutW * kOutH;
    for(int y = 0; y < kOutH / 2; y++) memcpy(uv + (size_t)y * kOutW, (uint8_t*)m.pData + (size_t)y * m.RowPitch, kOutW);
    ctx_->Unmap(stUV_.Get(), 0);
    return true;
}

}  // namespace opal

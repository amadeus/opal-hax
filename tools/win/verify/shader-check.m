// shader-check: runs Opal's own Metal shaders (from Opal.app's video service default.metallib) on a synthetic test
// pattern and compares each pass with a CPU transcription of our reverse-engineered spec (the same math the Windows
// HLSL uses). No camera images involved.
//
// usage: tools/win/verify/shader-check.sh [Opal.app]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static id<MTLDevice> gDev;
static id<MTLCommandQueue> gQueue;
static id<MTLLibrary> gLib;

#define die(...) do { fprintf(stderr, "%s\n", [[NSString stringWithFormat:__VA_ARGS__] UTF8String]); exit(1); } while (0)

// ---------------- CPU images ----------------
typedef struct { int w, h, c; float *p; } Img;  // c channels, row-major
static Img mk(int w, int h, int c) { Img i = {w, h, c, calloc((size_t)w * h * c, sizeof(float))}; return i; }
static float *px(Img i, int x, int y) { return i.p + ((size_t)y * i.w + x) * i.c; }
static float q8(float v) { v = fminf(1.f, fmaxf(0.f, v)); return roundf(v * 255.f) / 255.f; }
static float qh(float v) { return (float)(__fp16)v; }
static void quant(Img i, float (*q)(float)) { for (size_t k = 0; k < (size_t)i.w * i.h * i.c; k++) i.p[k] = q(i.p[k]); }
// Metal-style bilinear, clamp to edge, normalized coords
static void sample(Img t, float u, float v, float *out) {
    float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    for (int c = 0; c < t.c; c++) out[c] = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xx = x0 + i, yy = y0 + j;
            xx = xx < 0 ? 0 : xx >= t.w ? t.w - 1 : xx;
            yy = yy < 0 ? 0 : yy >= t.h ? t.h - 1 : yy;
            float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            for (int c = 0; c < t.c; c++) out[c] += wgt * px(t, xx, yy)[c];
        }
}

// ---------------- GPU helpers ----------------
static NSString *kVertexSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct VertexOut { float4 position [[position]]; float2 textureCoordinate; };\n"
     "vertex VertexOut checkVertex(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2); VertexOut o;\n"
     "  o.position = float4(p * 2.0 - 1.0, 0.0, 1.0); o.textureCoordinate = float2(p.x, 1.0 - p.y); return o; }\n";

static MTLPixelFormat fmtFor(int c, BOOL half) {
    if (c == 1) return half ? MTLPixelFormatR16Float : MTLPixelFormatR8Unorm;
    return half ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA8Unorm;
}
static id<MTLTexture> upload(Img i, BOOL half) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmtFor(i.c, half) width:i.w height:i.h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [gDev newTextureWithDescriptor:d];
    int n = i.w * i.h * i.c;
    if (half) {
        __fp16 *b = malloc(n * 2);
        for (int k = 0; k < n; k++) b[k] = (__fp16)i.p[k];
        [t replaceRegion:MTLRegionMake2D(0, 0, i.w, i.h) mipmapLevel:0 withBytes:b bytesPerRow:i.w * i.c * 2];
        free(b);
    } else {
        uint8_t *b = malloc(n);
        for (int k = 0; k < n; k++) b[k] = (uint8_t)lroundf(fminf(1, fmaxf(0, i.p[k])) * 255);
        [t replaceRegion:MTLRegionMake2D(0, 0, i.w, i.h) mipmapLevel:0 withBytes:b bytesPerRow:i.w * i.c];
        free(b);
    }
    return t;
}
static id<MTLTexture> target(int w, int h, int c, BOOL half) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmtFor(c, half) width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    return [gDev newTextureWithDescriptor:d];
}
static Img download(id<MTLTexture> t, int c) {
    Img i = mk((int)t.width, (int)t.height, c);
    int n = i.w * i.h * c;
    BOOL half = t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatR16Float;
    if (half) {
        __fp16 *b = malloc(n * 2);
        [t getBytes:b bytesPerRow:i.w * c * 2 fromRegion:MTLRegionMake2D(0, 0, i.w, i.h) mipmapLevel:0];
        for (int k = 0; k < n; k++) i.p[k] = b[k];
        free(b);
    } else {
        uint8_t *b = malloc(n);
        [t getBytes:b bytesPerRow:i.w * c fromRegion:MTLRegionMake2D(0, 0, i.w, i.h) mipmapLevel:0];
        for (int k = 0; k < n; k++) i.p[k] = b[k] / 255.f;
        free(b);
    }
    return i;
}

typedef struct { id<MTLRenderPipelineState> ps; NSDictionary<NSString *, NSNumber *> *tex, *buf, *smp; } Pass;
static Pass pass(NSString *frag, NSArray<NSNumber *> *outFormats, MTLFunctionConstantValues *fc) {
    NSError *err = nil;
    static id<MTLFunction> vtx;
    if (!vtx) vtx = [[gDev newLibraryWithSource:kVertexSrc options:nil error:&err] newFunctionWithName:@"checkVertex"];
    id<MTLFunction> f = fc ? [gLib newFunctionWithName:frag constantValues:fc error:&err] : [gLib newFunctionWithName:frag];
    if (!f) die(@"%@: %@", frag, err);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vtx;
    d.fragmentFunction = f;
    for (NSUInteger k = 0; k < outFormats.count; k++) d.colorAttachments[k].pixelFormat = outFormats[k].unsignedIntegerValue;
    MTLRenderPipelineReflection *refl = nil;
    Pass p;
    p.ps = [gDev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionBindingInfo reflection:&refl error:&err];
    if (!p.ps) die(@"%@ pipeline: %@", frag, err);
    NSMutableDictionary *t = [NSMutableDictionary dictionary], *b = [NSMutableDictionary dictionary], *s = [NSMutableDictionary dictionary];
    for (id<MTLBinding> x in refl.fragmentBindings) {
        if (x.type == MTLBindingTypeTexture) t[x.name] = @(x.index);
        else if (x.type == MTLBindingTypeBuffer) b[x.name] = @(x.index);
        else if (x.type == MTLBindingTypeSampler) s[x.name] = @(x.index);
    }
    p.tex = t; p.buf = b; p.smp = s;
    printf("  %s textures %s buffers %s samplers %s\n", frag.UTF8String, t.description.UTF8String, b.description.UTF8String, s.description.UTF8String);
    return p;
}
static void draw(Pass p, NSArray<id<MTLTexture>> *outs, NSArray<id<MTLTexture>> *texs, NSArray<NSData *> *bufs) {
    static id<MTLSamplerState> lin;
    if (!lin) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        lin = [gDev newSamplerStateWithDescriptor:sd];
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (NSUInteger k = 0; k < outs.count; k++) {
        rp.colorAttachments[k].texture = outs[k];
        rp.colorAttachments[k].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[k].storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p.ps];
    for (NSUInteger k = 0; k < texs.count; k++) {
        [e setFragmentTexture:texs[k] atIndex:k];
        [e setFragmentSamplerState:lin atIndex:k];
    }
    for (NSUInteger k = 0; k < bufs.count; k++) [e setFragmentBytes:bufs[k].bytes length:bufs[k].length atIndex:k];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) die(@"GPU error: %@", cb.error);
}
static void compare(const char *name, Img gpu, Img cpu, int chans) {
    double maxd = 0, sum = 0;
    size_t n = 0, big = 0;
    for (int y = 0; y < gpu.h; y++)
        for (int x = 0; x < gpu.w; x++)
            for (int c = 0; c < chans; c++) {
                double d = fabs(px(gpu, x, y)[c] - px(cpu, x, y)[c]);
                maxd = fmax(maxd, d);
                sum += d;
                n++;
                if (d > 2.5 / 255) big++;
            }
    printf("%-28s max %.4f (%.1f/255)  mean %.5f  >2.5/255: %.3f%%\n", name, maxd, maxd * 255, sum / n, 100.0 * big / n);
}

// ---------------- CPU transcriptions of the spec ----------------
static void surfaceWeights(int R, float *w16) {
    for (int k = 0; k < 16; k++) w16[k] = 0;
    float norm = 1.f / sqrtf((float)(2 * M_PI * R * R));
    for (int k = 0; k < R; k++) {
        float v = norm * expf(-(float)(k * k) / (2.f * R * R));
        w16[R - 1 + k] = w16[R - 1 - k] = v;
    }
}
static Img cpuSurfaceBlur(Img guide, Img val, int R, float bsigma, const float *w) {
    Img o = mk(guide.w, guide.h, 1);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            float u = (x + .5f) / o.w, v = (y + .5f) / o.h, g0[4], g[4], m[1];
            sample(guide, u, v, g0);
            float num = 0, den = 0;
            for (int i = 1 - R; i < R; i++)
                for (int j = 1 - R; j < R; j++) {
                    float pu = u + i / 256.f, pv = v + j / 144.f;
                    sample(guide, pu, pv, g);
                    float dd = 0;
                    if (guide.c == 1) dd = 3 * (g[0] - g0[0]) * (g[0] - g0[0]);
                    else for (int c = 0; c < 3; c++) dd += (g[c] - g0[c]) * (g[c] - g0[c]);
                    float wgt = w[i + R - 1] * w[j + R - 1] * expf(-0.5f * dd / (bsigma * bsigma));
                    sample(val, pu, pv, m);
                    num += wgt * m[0];
                    den += wgt;
                }
            px(o, x, y)[0] = num / den;
        }
    return o;
}
static Img cpuPre(Img color, Img mask, float power, int invert) {
    Img o = mk(color.w, color.h, 4);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            float u = (x + .5f) / o.w, v = (y + .5f) / o.h, c[4], m[1];
            sample(color, u, v, c);
            sample(mask, u, v, m);
            float *p = px(o, x, y);
            for (int k = 0; k < 3; k++) p[k] = powf(qh(c[k]), qh(power));
            p[3] = fabsf((float)invert - m[0]);
        }
    return o;
}
// 16-tap directional blur, literal transcription of bokehBlurAlpha / BravoCharlie
static int blurDir(Img t, float u, float v, float dx, float dy, float *out) {
    float rnd = sinf(u * dx + v * dy) * 43758.5453f;
    rnd = rnd - floorf(rnd);
    float s[16][4];
    for (int k = 0; k < 16; k++) sample(t, u + dx / 16 * (rnd + k), v + dy / 16 * (rnd + k), s[k]);
    float total = 0;
    for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) total += s[4 * b + 3][c];
    if (total < 0.01f) return 0;
    float acc[4] = {0};
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < 4; k++)
            for (int c = 0; c < 4; c++) acc[c] += s[4 * b + 3][k] * s[4 * b + k][c];
    for (int c = 0; c < 3; c++) out[c] = acc[c] / total;
    out[3] = 1;
    return 1;
}
static void cpuAlpha(Img pre, float d0x, float d0y, float d1x, float d1y, Img *ov, Img *od) {
    *ov = mk(pre.w, pre.h, 4);
    *od = mk(pre.w, pre.h, 4);
    for (int y = 0; y < pre.h; y++)
        for (int x = 0; x < pre.w; x++) {
            float u = (x + .5f) / pre.w, v = (y + .5f) / pre.h, c[4], a[4], b[4];
            sample(pre, u, v, c);
            float coc = c[3];
            if (!blurDir(pre, u, v, coc * d0x, coc * d0y, a)) memcpy(a, c, sizeof a);
            if (!blurDir(pre, u, v, coc * d1x, coc * d1y, b)) memcpy(b, c, sizeof b);
            float *pv = px(*ov, x, y), *pd = px(*od, x, y);
            for (int k = 0; k < 3; k++) { pv[k] = a[k]; pd[k] = a[k] + b[k]; }
            pv[3] = pd[3] = c[3];
        }
}
static Img cpuBravo(Img tv, Img td, float d1x, float d1y, float d2x, float d2y, float power) {
    Img o = mk(tv.w, tv.h, 4);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            float u = (x + .5f) / o.w, v = (y + .5f) / o.h, c[4], a[4], b[4];
            sample(tv, u, v, c);
            float coc = c[3];
            if (!blurDir(tv, u, v, coc * d1x, coc * d1y, a)) memcpy(a, c, sizeof a);
            if (!blurDir(td, u, v, coc * d2x, coc * d2y, b)) sample(td, u, v, b);
            float *p = px(o, x, y);
            for (int k = 0; k < 3; k++) p[k] = powf((a[k] + b[k]) / 3.f, power);
            p[3] = 1;
        }
    return o;
}


// ---- FiltersTabPipeline transcriptions
static float hl(float x) { x = x < 0.5f ? 2 * x * x : 1 - 2 * (1 - x) * (1 - x); return fminf(1, fmaxf(0, x)); }
static Img cpuTouchOverlay(Img src) {
    Img o = mk(src.w, src.h, 4);
    for (int k = 0; k < src.w * src.h; k++) {
        float g = src.p[k * 4 + 1] * 0.5f, b = src.p[k * 4 + 2] * 0.5f;
        float v = g < 0.5f ? 2 * g * b : 1 - 2 * (1 - g) * (1 - b);
        v = fminf(1, fmaxf(0, v));
        o.p[k * 4] = o.p[k * 4 + 1] = o.p[k * 4 + 2] = v;
        o.p[k * 4 + 3] = 1;
    }
    return o;
}
static Img cpuTouchComposite(Img src, Img ov, Img blur, Img lut, Img mask, float amount) {
    Img o = mk(src.w, src.h, 4);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            float u = (x + .5f) / src.w, v = (y + .5f) / src.h, img[4], m[1], a[4], b[4], t[4];
            sample(src, u, v, img);
            sample(mask, u, v, m);
            float tc[4];
            for (int c = 0; c < 3; c++) { sample(lut, img[c] * 0.99609375f + 0.001953125f, 0.5f, t); tc[c] = t[c]; }
            tc[3] = img[3];
            sample(ov, u, v, a);
            sample(blur, u, v, b);
            float xx = a[0] - b[0] + 0.5f;
            for (int k = 0; k < 3; k++) xx = hl(xx);
            float kk = fminf(1, fmaxf(0, (xx - 0.29411766f) * 2.8651686f));
            float *p = px(o, x, y);
            for (int c = 0; c < 4; c++) {
                float mixed = img[c] + (tc[c] - img[c]) * amount;
                float sm = mixed + (img[c] - mixed) * kk;
                p[c] = img[c] + (sm - img[c]) * m[0];
            }
        }
    return o;
}
static Img cpuExtractSkin(Img src, Img mask) {
    Img o = mk(src.w, src.h, 4);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            float u = (x + .5f) / src.w, v = (y + .5f) / src.h, c[4], m[1];
            sample(src, u, v, c);
            sample(mask, u, v, m);
            float *p = px(o, x, y);
            if (m[0] < 0.5f) continue;
            float cc[4];
            for (int k = 0; k < 4; k++) cc[k] = fminf(0.9999f, fmaxf(0.0001f, c[k]));
            float sum = cc[0] + cc[1] + cc[2], mean = sum * 0.33333f, inv = 3.00003f / sum;
            float sat = fmaxf((cc[0] - mean) * (1 / (1 - mean)), (mean - cc[0]) * inv);
            float lk = fminf(1, fmaxf(0, 4 * fminf(cc[0] - cc[1], 2 * cc[1] - cc[2]) * (1 - sat) * inv));
            if (lk > 0.3f) memcpy(p, c, 16);
        }
    return o;
}
static float hsBlend(float la, float lb) { return la > 0.5f ? 1 - (1 - 2 * (la - 0.5f)) * (0.5f - lb) : 2 * la * (lb + 0.5f); }
static float sgnf(float x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
static Img cpuHS(Img src, Img blur, float shadow, float highlight) {
    Img o = mk(src.w, src.h, 4);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            float u = (x + .5f) / src.w, v = (y + .5f) / src.h, c[4], bl[4];
            sample(src, u, v, c);
            sample(blur, u, v, bl);
            float s[3], t[3];
            for (int k = 0; k < 3; k++) { s[k] = sqrtf(fmaxf(c[k], 0)); t[k] = sqrtf(fmaxf(bl[k], 0)); }
            float ta[3] = {0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2], 0.596f * s[0] - 0.2755f * s[1] - 0.321f * s[2],
                           0.212f * s[0] - 0.523f * s[1] + 0.311f * s[2]};
            float Yb = 0.299f * t[0] + 0.587f * t[1] + 0.114f * t[2], tb0 = 1 - Yb;
            float sgnH = highlight > 0 ? -1 : 1, sgnS = shadow > 0 ? 1 : -1;
            if (tb0 < 0.5f) {
                float h2 = highlight * highlight, xf = fminf(1 - 2 * tb0, 1), lb0 = (0.5f - Yb) * sgnH;
                while (h2 > 0) {
                    float la = ta[0], li = 1 - la, lb = lb0 * sgnf(li);
                    float href = (fabsf(li) > 0.01f ? 1 / fabsf(li) : 100) * (li < 0 ? -1 : 1);
                    float op = fminf(h2, 1) * xf; h2 -= 1;
                    float keep = 1 - op;
                    ta[0] = hsBlend(la, lb) * op + keep * la;
                    float k = (1 - ta[0]) * href * op + keep;
                    ta[1] *= k; ta[2] *= k;
                }
            }
            if (tb0 > 0.5f) {
                float s2 = shadow * shadow, xf = fminf(2 * tb0 - 1, 1), lb0 = (0.5f - Yb) * sgnS;
                while (s2 > 0) {
                    float la = ta[0], lb = lb0 * sgnf(1 - la);
                    float lref = (fabsf(la) > 0.01f ? 1 / fabsf(la) : 100) * (la < 0 ? -1 : 1);
                    float op = fminf(s2, 1) * xf; s2 -= 1;
                    float keep = 1 - op;
                    ta[0] = hsBlend(la, lb) * op + keep * la;
                    float k = ta[0] * lref * op + keep;
                    ta[1] *= k; ta[2] *= k;
                }
            }
            float r = 1.00048006f * ta[0] + 0.955558002f * ta[1] + 0.619548976f * ta[2];
            float g = 0.999863982f * ta[0] - 0.271544993f * ta[1] - 0.646785975f * ta[2];
            float b = 0.999445975f * ta[0] - 1.10802996f * ta[1] + 1.70542002f * ta[2];
            float *p = px(o, x, y);
            p[0] = fmaxf(r, 0) * fmaxf(r, 0); p[1] = fmaxf(g, 0) * fmaxf(g, 0); p[2] = fmaxf(b, 0) * fmaxf(b, 0); p[3] = c[3];
        }
    return o;
}
static Img cpuGauss(Img src, float sigma) {  // separable normalized gaussian, +-3.5 sigma, clamp
    int r = (int)ceilf(3.5f * sigma);
    Img t = mk(src.w, src.h, src.c), o = mk(src.w, src.h, src.c);
    for (int pass = 0; pass < 2; pass++) {
        Img in = pass ? t : src, out = pass ? o : t;
        for (int y = 0; y < src.h; y++)
            for (int x = 0; x < src.w; x++) {
                float acc[4] = {0}, ws = 0;
                for (int i = -r; i <= r; i++) {
                    int xx = pass ? x : x + i, yy = pass ? y + i : y;
                    xx = xx < 0 ? 0 : xx >= src.w ? src.w - 1 : xx;
                    yy = yy < 0 ? 0 : yy >= src.h ? src.h - 1 : yy;
                    float w = expf(-0.5f * i * i / (sigma * sigma));
                    for (int c = 0; c < src.c; c++) acc[c] += w * px(in, xx, yy)[c];
                    ws += w;
                }
                for (int c = 0; c < src.c; c++) px(out, x, y)[c] = acc[c] / ws;
            }
    }
    return o;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *app = argc > 1 ? @(argv[1]) : @"Opal Backup.app";
        NSString *lib = [app stringByAppendingPathComponent:@"Contents/XPCServices/OpalCameraVideoService.xpc/Contents/Resources/default.metallib"];
        gDev = MTLCreateSystemDefaultDevice();
        gQueue = [gDev newCommandQueue];
        NSError *err = nil;
        gLib = [gDev newLibraryWithURL:[NSURL fileURLWithPath:lib] error:&err];
        if (!gLib) die(@"metallib: %@", err);

        // ---- synthetic inputs: colorful pattern with bright spots; a soft "person" blob mask
        const int W = 640, H = 360;
        Img color = mk(W, H, 4);
        srand(7);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float *p = px(color, x, y);
                p[0] = 0.5f + 0.4f * sinf(x * 0.05f) * cosf(y * 0.03f);
                p[1] = 0.4f + 0.3f * sinf((x + y) * 0.02f);
                p[2] = (float)((x / 40 + y / 40) % 2) * 0.6f + 0.2f;
                p[3] = 1;
                if (rand() % 400 == 0) p[0] = p[1] = p[2] = 1;  // highlights
            }
        quant(color, q8);
        Img ch0 = mk(256, 144, 1), ch1 = mk(256, 144, 1);
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < 256; x++) {
                float dx = (x - 128) / 50.f, dy = (y - 90) / 60.f, r = sqrtf(dx * dx + dy * dy);
                float person = 1.f / (1.f + expf((r - 1.f) * 8.f)) + ((rand() % 100) - 50) / 1000.f;  // noisy edge
                px(ch1, x, y)[0] = person * 1.2f - 0.1f;  // raw values go a bit outside 0..1
                px(ch0, x, y)[0] = 1.1f - person * 1.2f;
            }
        quant(ch0, q8);
        quant(ch1, q8);
        // camera image resized to 256x144 (bilinear at destination centers), as MTIImage.resized does
        Img small = mk(256, 144, 4);
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < 256; x++) sample(color, (x + .5f) / 256, (y + .5f) / 144, px(small, x, y));
        quant(small, q8);

        // ---- surface blurs (mask refinement)
        int R = 8;
        float w16[16];
        surfaceWeights(R, w16);
        MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new];
        [fc setConstantValue:&R type:MTLDataTypeInt withName:@"metalpetal::surfaceblur::mtiSurfaceBlurKernelRadius"];
        float bsGray = 2 * 100 / 255.f, bsColor = 2 * 75 / 255.f;
        NSData *wd = [NSData dataWithBytes:w16 length:64];

        Pass pg = pass(@"metalpetal::surfaceblur::mtiSurfaceBlurGray", @[ @(MTLPixelFormatR8Unorm) ], fc);
        id<MTLTexture> tG1 = target(256, 144, 1, NO);
        draw(pg, @[ tG1 ], @[ upload(ch0, NO), upload(ch1, NO) ], @[ wd, [NSData dataWithBytes:&bsGray length:4] ]);
        Img gGray = download(tG1, 1);
        Img cGray = cpuSurfaceBlur(ch0, ch1, R, bsGray, w16);
        quant(cGray, q8);
        compare("surfaceBlurGray (mask)", gGray, cGray, 1);

        Pass pc = pass(@"metalpetal::surfaceblur::mtiSurfaceBlur", @[ @(MTLPixelFormatR8Unorm) ], fc);
        id<MTLTexture> tG2 = target(256, 144, 1, NO);
        draw(pc, @[ tG2 ], @[ upload(small, NO), upload(gGray, NO) ], @[ wd, [NSData dataWithBytes:&bsColor length:4] ]);
        Img gMask = download(tG2, 1);
        Img cMask = cpuSurfaceBlur(small, gGray, R, bsColor, w16);
        quant(cMask, q8);
        compare("surfaceBlur color (mask)", gMask, cMask, 1);

        // ---- bokeh
        float radius = 11, power = powf(10, 0.5f);
        float dlt[3][2];
        for (int i = 0; i < 3; i++) {
            float a = (float)(0 + i * 2 * M_PI / 3);
            dlt[i][0] = radius * sinf(a) / W;
            dlt[i][1] = radius * cosf(a) / H;
        }
        __fp16 ph = (__fp16)power;
        bool inv = true;
        Pass pp = pass(@"metalpetal::bokeh::bokehBlurPre", @[ @(MTLPixelFormatRGBA16Float) ], nil);
        id<MTLTexture> tPre = target(W, H, 4, YES);
        draw(pp, @[ tPre ], @[ upload(color, NO), upload(gMask, NO) ], @[ [NSData dataWithBytes:&ph length:2], [NSData dataWithBytes:&inv length:1] ]);
        Img gPre = download(tPre, 4);
        Img cPre = cpuPre(color, gMask, power, 1);
        quant(cPre, qh);
        compare("bokehBlurPre", gPre, cPre, 4);

        Pass pa = pass(@"metalpetal::bokeh::bokehBlurAlpha", @[ @(MTLPixelFormatRGBA16Float), @(MTLPixelFormatRGBA16Float) ], nil);
        id<MTLTexture> tV = target(W, H, 4, YES), tD = target(W, H, 4, YES);
        draw(pa, @[ tV, tD ], @[ tPre ], @[ [NSData dataWithBytes:dlt[0] length:8], [NSData dataWithBytes:dlt[1] length:8] ]);
        Img gV = download(tV, 4), gD = download(tD, 4), cV, cD;
        cpuAlpha(gPre, dlt[0][0], dlt[0][1], dlt[1][0], dlt[1][1], &cV, &cD);
        quant(cV, qh);
        quant(cD, qh);
        compare("bokehBlurAlpha vertical", gV, cV, 4);
        compare("bokehBlurAlpha diagonal", gD, cD, 4);

        float ip = 1 / power;
        Pass pb = pass(@"metalpetal::bokeh::bokehBlurBravoCharlie", @[ @(MTLPixelFormatRGBA8Unorm) ], nil);
        id<MTLTexture> tOut = target(W, H, 4, NO);
        draw(pb, @[ tOut ], @[ tV, tD ], @[ [NSData dataWithBytes:dlt[1] length:8], [NSData dataWithBytes:dlt[2] length:8], [NSData dataWithBytes:&ip length:4] ]);
        Img gOut = download(tOut, 4);
        Img cOut = cpuBravo(gV, gD, dlt[1][0], dlt[1][1], dlt[2][0], dlt[2][1], ip);
        quant(cOut, q8);
        compare("bokehBlurBravoCharlie", gOut, cOut, 3);


        // ---- touch-up (inputs: color frame, refined mask gMask), amount 0.3
        Pass pt1 = pass(@"metalpetal::touchup::highPassSkinSmoothingGBChannelOverlay", @[ @(MTLPixelFormatRGBA8Unorm) ], nil);
        id<MTLTexture> tOv = target(W, H, 4, NO);
        draw(pt1, @[ tOv ], @[ upload(color, NO) ], @[]);
        Img gOv = download(tOv, 4), cOv = cpuTouchOverlay(color);
        quant(cOv, q8);
        compare("touch-up overlay", gOv, cOv, 3);
        Img blurOv = cpuGauss(gOv, 8);  // stand-in for MPS (measured separately), same input on both sides
        quant(blurOv, q8);
        int lutv[256] = {0,1,2,3,5,6,7,9,10,11,13,14,15,17,18,19,20,22,23,24,26,27,28,30,31,32,34,35,36,37,39,40,
41,43,44,45,46,48,49,50,52,53,54,55,57,58,59,61,62,63,64,66,67,68,69,71,72,73,74,76,77,78,
79,81,82,83,84,85,87,88,89,90,92,93,94,95,96,98,99,100,101,102,103,105,106,107,108,109,110,112,113,114,
115,116,117,118,120,121,122,123,124,125,126,127,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,
146,147,148,149,150,151,152,153,154,155,156,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,
172,173,174,175,176,177,178,179,180,180,181,182,183,184,185,186,186,187,188,189,190,191,191,192,193,194,195,196,
196,197,198,199,200,200,201,202,203,204,204,205,206,207,207,208,209,210,211,211,212,213,214,214,215,216,217,217,
218,219,220,220,221,222,223,223,224,225,225,226,227,228,228,229,230,230,231,232,233,233,234,235,236,236,237,238,
238,239,240,240,241,242,243,243,244,245,245,246,247,248,248,249,250,250,251,252,252,253,254,255};
        Img lut = mk(256, 1, 4);
        for (int i = 0; i < 256; i++) { px(lut, i, 0)[0] = px(lut, i, 0)[1] = px(lut, i, 0)[2] = lutv[i] / 255.f; px(lut, i, 0)[3] = 1; }
        float amount = 0.3f;
        Pass pt4 = pass(@"metalpetal::touchup::highPassSkinSmoothingMaskProcessAndComposite", @[ @(MTLPixelFormatRGBA8Unorm) ], nil);
        id<MTLTexture> tTu = target(W, H, 4, NO);
        draw(pt4, @[ tTu ], @[ upload(color, NO), tOv, upload(blurOv, NO), upload(lut, NO), upload(gMask, NO) ], @[ [NSData dataWithBytes:&amount length:4] ]);
        Img gTu = download(tTu, 4), cTu = cpuTouchComposite(color, gOv, blurOv, lut, gMask, amount);
        quant(cTu, q8);
        compare("touch-up composite", gTu, cTu, 3);
        compare("(touch-up input vs output)", color, gTu, 3);

        // ---- skin extraction
        Pass pe = pass(@"metalpetal::skintoneadjustment::extractSkin", @[ @(MTLPixelFormatRGBA8Unorm) ], nil);
        id<MTLTexture> tSk = target(W, H, 4, NO);
        draw(pe, @[ tSk ], @[ upload(color, NO), upload(gMask, NO) ], @[]);
        Img gSk = download(tSk, 4), cSk = cpuExtractSkin(color, gMask);
        quant(cSk, q8);
        compare("extractSkin", gSk, cSk, 4);

        // ---- highlights & shadows at several settings
        Img blur30 = cpuGauss(color, 30);
        quant(blur30, q8);
        float hsv[4][2] = {{0.6f, 0}, {0, 0.7f}, {-0.5f, 0.4f}, {0.3f, -0.8f}};  // (shadow, highlight)
        Pass phs = pass(@"metalpetal::shadowhighlight::shadowHighlightAdjust", @[ @(MTLPixelFormatRGBA8Unorm) ], nil);
        for (int k = 0; k < 4; k++) {
            id<MTLTexture> tHs = target(W, H, 4, NO);
            draw(phs, @[ tHs ], @[ upload(color, NO), upload(blur30, NO) ], @[ [NSData dataWithBytes:&hsv[k][0] length:4], [NSData dataWithBytes:&hsv[k][1] length:4] ]);
            Img gHs = download(tHs, 4), cHs = cpuHS(color, blur30, hsv[k][0], hsv[k][1]);
            quant(cHs, q8);
            char nm[64];
            snprintf(nm, sizeof nm, "shadowHighlight s%.1f h%.1f", hsv[k][0], hsv[k][1]);
            compare(nm, gHs, cHs, 3);
        }
        // sanity: how different is the result from the input at all (so a match means something)
        compare("(input vs output, for scale)", color, gOut, 3);
    }
    return 0;
}

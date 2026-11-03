// bake-luts: bake Opal.app's 13 camera "looks" into 3D LUTs (.cube) by running Opal's own filter code on this Mac.
//
// What runs here is exactly what OpalCameraVideoService runs (reverse-engineered from Opal 0.26's OpalCameraVideoService):
//  - Core Image looks (CIPhotoEffect*): through Opal's bundled MetalPetal.framework, MTICoreImageKernel.image(
//    byProcessing:using:colorSpace:outputDimensions:outputPixelFormat:outputAlphaType:) with colorSpace DeviceRGB,
//    outputPixelFormat .unspecified, alpha .nonPremultiplied, on an MTIContext whose coreImageContextOptions are
//    Opal's: workingFormat BGRA8, workingColorSpace = outputColorSpace = DeviceRGB, allowLowPower, cacheIntermediates,
//    no software renderer. Input/output textures are BGRA8Unorm (MetalPetal's working format), as in Opal.
//  - Custom looks (D1 G1 Q1 S1 X1): fragment functions MT<name>Fragment from Opal's default.metallib (arguments:
//    inputTexture + textureSampler only), drawn with MetalPetal's VertexOut layout.
//
// Every look is evaluated on all 256^3 8-bit RGB codes (a 4096x4096 image), which is the complete domain Opal sees
// (its frames are BGRA8 at that point). The LUT grid is then taken from that table (CI looks: trilinear between
// 8-bit codes) or evaluated directly at the grid points (Metal looks: float input), and the LUT is checked against
// the full table. A second, permuted copy of the image proves each look is purely per-pixel.
//
// Strength (OCFilters, 0..1): Opal composites with MTIBlendFilter(.normal, intensity = s) over the unfiltered frame,
// i.e. out = in + (look(in) - in) * s; checked here against MetalPetal itself on all 256^3 codes.
// Order: the main app lists Contents/Resources/*_pipeline.json via contentsOfDirectoryAtPath without sorting, so the
// UI order is the file-system enumeration order, which this tool reproduces.
//
// Build/run: tools/win/luts/bake-luts.sh [Opal.app] [size]   (clang, Metal, Core Image; macOS only). BAKE_DEBUG=1
// prints the worst LUT error per look. Output depends on this macOS's Core Image for the CIPhotoEffect looks.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreImage/CoreImage.h>
#import <dlfcn.h>
#import <math.h>

// ---- MetalPetal (no headers ship with Opal; declare what we use) ----
typedef NS_ENUM(NSInteger, MTIAlphaType) { MTIAlphaTypeUnknown = 0, MTIAlphaTypeNonPremultiplied = 1,
    MTIAlphaTypePremultiplied = 2, MTIAlphaTypeAlphaIsOne = 3 };
@protocol MTIShim
- (instancetype)initWithDevice:(id<MTLDevice>)device options:(id)options error:(NSError **)error;
- (void)setCoreImageContextOptions:(NSDictionary *)o;
- (instancetype)initWithTexture:(id<MTLTexture>)texture alphaType:(MTIAlphaType)alphaType;
- (id)startTaskToRenderImage:(id)image toTexture:(id<MTLTexture>)texture destinationAlphaType:(MTIAlphaType)a
                       error:(NSError **)error completion:(void (^)(id task))completion;
- (void)waitUntilCompleted;
- (instancetype)initWithBlendMode:(NSString *)mode;
- (void)setInputBackgroundImage:(id)i;
- (void)setInputImage:(id)i;
- (void)setIntensity:(float)f;
- (void)setOutputPixelFormat:(MTLPixelFormat)f;
- (id)outputImage;
@end
// static MTICoreImageKernel.image(byProcessing: MTIImage, using: CIFilter, colorSpace: CGColorSpace?,
//   outputDimensions: MTITextureDimensions, outputPixelFormat: MTLPixelFormat, outputAlphaType: MTIAlphaType) -> MTIImage
// Swift passes the 3-word MTITextureDimensions exploded in registers; the result is +1.
typedef id (*MTICIKernelFn)(id image, CIFilter *filter, CGColorSpaceRef cs, NSUInteger w, NSUInteger h,
                            NSUInteger d, NSUInteger pixelFormat, NSInteger alphaType) __attribute__((ns_returns_retained));
static const char *kCIKernelSym = "$s10MetalPetal18MTICoreImageKernelV5image12byProcessing5using10colorSpace16outputDimensions0L11PixelFormat0L9AlphaTypeSo8MTIImageCAL_So8CIFilterCSo07CGColorK3RefaSgSo010MTITextureM0VSo08MTLPixelO0VSo08MTIAlphaQ0VtFZ";

#define SIDE 4096          // 4096*4096 = 256^3
#define NPIX (SIDE * SIDE)

static id<MTLDevice> gDev;
static id<MTLCommandQueue> gQueue;
static id gMTIContext;
static MTICIKernelFn gCIKernel;
static Class gMTIImage, gMTIBlendFilter;
static NSString *gBlendNormal;

static void die(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    fprintf(stderr, "bake-luts: %s\n", s.UTF8String);
    exit(1);
}

// ---- layouts: layout A puts code (r,g,b) at index r + 256*g + 65536*b; layout B is a fixed permutation of A ----
static inline uint32_t codeAt(uint32_t idx, int permuted) {
    // multiplication by an odd constant mod 2^24 is a bijection
    return permuted ? (uint32_t)(((uint64_t)idx * 2654435761u + 12345u) & 0xFFFFFFu) : idx;
}

static id<MTLTexture> makeTex(MTLPixelFormat f, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    return [gDev newTextureWithDescriptor:d];
}

static id<MTLTexture> codeTexture(int permuted) {
    id<MTLTexture> t = makeTex(MTLPixelFormatBGRA8Unorm, SIDE, SIDE);
    uint8_t *buf = malloc((size_t)NPIX * 4);
    for (uint32_t i = 0; i < NPIX; i++) {
        uint32_t c = codeAt(i, permuted);
        buf[i * 4 + 0] = (c >> 16) & 255;  // B
        buf[i * 4 + 1] = (c >> 8) & 255;   // G
        buf[i * 4 + 2] = c & 255;          // R
        buf[i * 4 + 3] = 255;
    }
    [t replaceRegion:MTLRegionMake2D(0, 0, SIDE, SIDE) mipmapLevel:0 withBytes:buf bytesPerRow:SIDE * 4];
    free(buf);
    return t;
}

// table: float[256^3 * 3], indexed by code r + 256g + 65536b
static void readBGRA8(id<MTLTexture> t, int permuted, float *table) {
    uint8_t *buf = malloc((size_t)NPIX * 4);
    [t getBytes:buf bytesPerRow:SIDE * 4 fromRegion:MTLRegionMake2D(0, 0, SIDE, SIDE) mipmapLevel:0];
    for (uint32_t i = 0; i < NPIX; i++) {
        uint32_t c = codeAt(i, permuted);
        table[c * 3 + 0] = buf[i * 4 + 2] / 255.0f;
        table[c * 3 + 1] = buf[i * 4 + 1] / 255.0f;
        table[c * 3 + 2] = buf[i * 4 + 0] / 255.0f;
    }
    free(buf);
}

static void readRGBA32F(id<MTLTexture> t, int permuted, float *table, NSUInteger count) {
    float *buf = malloc(count * 16);
    NSUInteger w = t.width, h = t.height;
    [t getBytes:buf bytesPerRow:w * 16 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = permuted >= 0 ? codeAt(i, permuted) : i;
        for (int k = 0; k < 3; k++) table[c * 3 + k] = buf[i * 4 + k];
    }
    free(buf);
}

// ---- Metal looks ----
static NSString *kVertexSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct VertexOut { float4 position [[position]]; float2 textureCoordinate; };\n"
     "vertex VertexOut bakeVertex(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2); VertexOut o;\n"
     "  o.position = float4(p * 2.0 - 1.0, 0.0, 1.0); o.textureCoordinate = float2(p.x, 1.0 - p.y); return o; }\n";

static id<MTLRenderPipelineState> metalPipeline(id<MTLLibrary> opalLib, NSString *fragName) {
    static id<MTLFunction> vtx;
    NSError *err = nil;
    if (!vtx) {
        id<MTLLibrary> vl = [gDev newLibraryWithSource:kVertexSrc options:nil error:&err];
        if (!vl) die(@"vertex compile: %@", err);
        vtx = [vl newFunctionWithName:@"bakeVertex"];
    }
    id<MTLFunction> frag = [opalLib newFunctionWithName:fragName];
    if (!frag) die(@"%@ not found in Opal's default.metallib", fragName);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vtx;
    d.fragmentFunction = frag;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    MTLRenderPipelineReflection *refl = nil;
    id<MTLRenderPipelineState> ps = [gDev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionBindingInfo
                                                                    reflection:&refl error:&err];
    if (!ps) die(@"%@ pipeline: %@", fragName, err);
    // Opal's looks take only inputTexture [[texture(0)]] (+ textureSampler [[sampler(0)]], which the compiler may
    // report unused); anything else (extra textures, uniforms) would need a shader port, not a LUT.
    NSMutableArray *b = [NSMutableArray array];
    BOOL inputUsed = NO;
    for (id<MTLBinding> x in refl.fragmentBindings) {
        NSString *k = [NSString stringWithFormat:@"%@:%d:%d", x.name, (int)x.type, (int)x.index];
        if ([k isEqualToString:@"inputTexture:2:0"]) inputUsed = x.used;
        else if (![k isEqualToString:@"textureSampler:3:0"]) [b addObject:k];
    }
    if (!inputUsed || b.count) die(@"%@: unexpected fragment arguments %@ (input used: %d): needs a shader port", fragName, b, inputUsed);
    return ps;
}

static void runMetal(id<MTLRenderPipelineState> ps, id<MTLTexture> in, id<MTLTexture> out) {
    static id<MTLSamplerState> samp;
    if (!samp) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        samp = [gDev newSamplerStateWithDescriptor:sd];
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setFragmentTexture:in atIndex:0];
    [e setFragmentSamplerState:samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) die(@"GPU error: %@", cb.error);
}

// ---- Core Image looks through MetalPetal, as Opal does ----
static void setupMetalPetal(NSString *frameworkPath) {
    NSString *bin = [frameworkPath stringByAppendingPathComponent:@"MetalPetal"];
    if (!dlopen(bin.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL)) die(@"dlopen MetalPetal: %s", dlerror());
    gCIKernel = (MTICIKernelFn)dlsym(RTLD_DEFAULT, kCIKernelSym);
    NSString *__unsafe_unretained *bm = (NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT, "MTIBlendModeNormal");
    gMTIImage = NSClassFromString(@"MTIImage");
    gMTIBlendFilter = NSClassFromString(@"MTIBlendFilter");
    Class ctxC = NSClassFromString(@"MTIContext"), optC = NSClassFromString(@"MTIContextOptions");
    if (!gCIKernel || !bm || !gMTIImage || !gMTIBlendFilter || !ctxC || !optC) die(@"MetalPetal symbols missing");
    gBlendNormal = *bm;

    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    id opts = [optC new];
    [opts setCoreImageContextOptions:@{
        kCIContextWorkingFormat : @(kCIFormatBGRA8),
        kCIContextAllowLowPower : @YES,
        kCIContextCacheIntermediates : @YES,
        kCIContextWorkingColorSpace : (__bridge id)rgb,
        kCIContextOutputColorSpace : (__bridge id)rgb,
        kCIContextUseSoftwareRenderer : @NO,
    }];
    NSError *err = nil;
    gMTIContext = [[ctxC alloc] initWithDevice:gDev options:opts error:&err];
    if (!gMTIContext) die(@"MTIContext: %@", err);
    CGColorSpaceRelease(rgb);
}

static id mtiImage(id<MTLTexture> t) { return [[gMTIImage alloc] initWithTexture:t alphaType:MTIAlphaTypeAlphaIsOne]; }

static id ciLookImage(id input, NSString *filterName) {
    CIFilter *f = [CIFilter filterWithName:filterName];
    if (!f) die(@"no Core Image filter %@", filterName);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    id out = gCIKernel(input, f, rgb, SIDE, SIDE, 1, MTLPixelFormatInvalid /* .unspecified */, MTIAlphaTypeNonPremultiplied);
    CGColorSpaceRelease(rgb);
    return out;
}

static void renderMTI(id image, id<MTLTexture> out) {
    NSError *err = nil;
    id task = [gMTIContext startTaskToRenderImage:image toTexture:out destinationAlphaType:MTIAlphaTypeNonPremultiplied
                                            error:&err completion:nil];
    if (!task) die(@"MetalPetal render: %@", err);
    [task waitUntilCompleted];
}

// Opal's camera-frame conversion (metalpetal::yuv2rgbconvert_opal_custom::colorConversionFragment, yTexture +
// cbcrTexture): probe it on a grid of (Y, Cb, Cr) codes and compare with BT.709 full range, chroma centered at 0.5.
static double probeYUV(id<MTLLibrary> opalLib) {
    NSError *err = nil;
    id<MTLLibrary> vl = [gDev newLibraryWithSource:kVertexSrc options:nil error:&err];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [vl newFunctionWithName:@"bakeVertex"];
    d.fragmentFunction = [opalLib newFunctionWithName:@"metalpetal::yuv2rgbconvert_opal_custom::colorConversionFragment"];
    if (!d.fragmentFunction) return -1;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> ps = [gDev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) return -1;
    const int W = 4096;  // Y = i % 256, Cb/Cr from a 16x16 grid
    uint8_t *y = malloc(W), *c = malloc(W * 2);
    for (int i = 0; i < W; i++) { y[i] = (uint8_t)(i * 37); c[i * 2] = (uint8_t)((i / 16) * 17); c[i * 2 + 1] = (uint8_t)((i % 16) * 17); }
    id<MTLTexture> ty = makeTex(MTLPixelFormatR8Unorm, W, 1), tc = makeTex(MTLPixelFormatRG8Unorm, W, 1), to = makeTex(MTLPixelFormatRGBA32Float, W, 1);
    [ty replaceRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0 withBytes:y bytesPerRow:W];
    [tc replaceRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0 withBytes:c bytesPerRow:W * 2];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = to;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setFragmentTexture:ty atIndex:0];
    [e setFragmentTexture:tc atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    float *o = malloc(W * 16);
    [to getBytes:o bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0];
    double mx = 0;
    for (int i = 0; i < W; i++) {
        double Y = y[i] / 255.0, Cb = c[i * 2] / 255.0 - 0.5, Cr = c[i * 2 + 1] / 255.0 - 0.5;
        double want[3] = { Y + 1.5748 * Cr, Y - 0.187324 * Cb - 0.468124 * Cr, Y + 1.8556 * Cb };
        for (int k = 0; k < 3; k++) mx = fmax(mx, fabs(want[k] - o[i * 4 + k]));
    }
    free(y); free(c); free(o);
    return mx;
}

// ---- LUT helpers ----
static inline void tri(const float *tab, int n, float r, float g, float b, float out[3]) {
    // tab is an n^3 RGB grid (r fastest) over [0,1]
    float fr = r * (n - 1), fg = g * (n - 1), fb = b * (n - 1);
    int r0 = (int)fr, g0 = (int)fg, b0 = (int)fb;
    if (r0 > n - 2) r0 = n - 2;
    if (g0 > n - 2) g0 = n - 2;
    if (b0 > n - 2) b0 = n - 2;
    float dr = fr - r0, dg = fg - g0, db = fb - b0;
    for (int k = 0; k < 3; k++) {
#define T(i, j, l) tab[(((size_t)(b0 + l) * n + (g0 + j)) * n + (r0 + i)) * 3 + k]
        float c00 = T(0, 0, 0) + (T(1, 0, 0) - T(0, 0, 0)) * dr, c10 = T(0, 1, 0) + (T(1, 1, 0) - T(0, 1, 0)) * dr;
        float c01 = T(0, 0, 1) + (T(1, 0, 1) - T(0, 0, 1)) * dr, c11 = T(0, 1, 1) + (T(1, 1, 1) - T(0, 1, 1)) * dr;
#undef T
        float c0 = c00 + (c10 - c00) * dg, c1 = c01 + (c11 - c01) * dg;
        out[k] = c0 + (c1 - c0) * db;
    }
}

static float clamp01(float v) { return isnan(v) ? 0.0f : (v < 0 ? 0 : (v > 1 ? 1 : v)); }

static void writeCube(NSString *path, NSString *title, const float *lut, int n) {
    FILE *f = fopen(path.fileSystemRepresentation, "w");
    if (!f) die(@"cannot write %@", path);
    fprintf(f, "# Generated by tools/win/luts/bake-luts.m from Opal.app 0.26 (derived from proprietary code; do not publish)\n");
    fprintf(f, "TITLE \"%s\"\nLUT_3D_SIZE %d\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n", title.UTF8String, n);
    for (size_t i = 0; i < (size_t)n * n * n; i++) fprintf(f, "%.6f %.6f %.6f\n", lut[i * 3], lut[i * 3 + 1], lut[i * 3 + 2]);
    fclose(f);
}

typedef struct { double maxErr, meanErr, p999; uint32_t worstCode; float worstLut[3]; } ErrStats;

// compare LUT (trilinear) against the exact table over all 256^3 codes; errors in 8-bit code units
static ErrStats lutError(const float *lut, int n, const float *table) {
    static uint32_t hist[4096];
    memset(hist, 0, sizeof hist);
    double sum = 0, mx = 0;
    uint32_t worst = 0;
    float worstLut[3] = { 0 };
    for (uint32_t c = 0; c < NPIX; c++) {
        float o[3];
        tri(lut, n, (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c >> 16) / 255.0f, o);
        for (int k = 0; k < 3; k++) {
            double e = fabs((double)o[k] - clamp01(table[c * 3 + k])) * 255.0;
            sum += e;
            if (e > mx) { mx = e; worst = c; memcpy(worstLut, o, sizeof o); }
            int bin = (int)(e * 100);
            hist[bin > 4095 ? 4095 : bin]++;
        }
    }
    uint64_t target = (uint64_t)(NPIX * 3.0 * 0.999), acc = 0;
    double p = 0;
    for (int i = 0; i < 4096; i++) { acc += hist[i]; if (acc >= target) { p = (i + 1) / 100.0; break; } }
    ErrStats r = { mx, sum / (NPIX * 3.0), p, worst, { 0 } };
    memcpy(r.worstLut, worstLut, sizeof worstLut);
    return r;
}

// ---- main ----
int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *app = nil, *outDir = nil;
        int n = 64;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--app") && i + 1 < argc) app = @(argv[++i]);
            else if (!strcmp(argv[i], "--out") && i + 1 < argc) outDir = @(argv[++i]);
            else if (!strcmp(argv[i], "--size") && i + 1 < argc) n = atoi(argv[++i]);
            else die(@"usage: bake-luts --app <Opal.app> --out <dir> [--size 64]");
        }
        if (!app || !outDir || n < 2 || n > 256) die(@"usage: bake-luts --app <Opal.app> --out <dir> [--size 64]");
        NSString *vsRes = [app stringByAppendingPathComponent:@"Contents/XPCServices/OpalCameraVideoService.xpc/Contents/Resources"];
        NSString *mainRes = [app stringByAppendingPathComponent:@"Contents/Resources"];
        NSString *mpFramework = [app stringByAppendingPathComponent:@"Contents/Frameworks/MetalPetal.framework"];
        [[NSFileManager defaultManager] createDirectoryAtPath:outDir withIntermediateDirectories:YES attributes:nil error:nil];

        gDev = MTLCreateSystemDefaultDevice();
        gQueue = [gDev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> opalLib = [gDev newLibraryWithURL:[NSURL fileURLWithPath:[vsRes stringByAppendingPathComponent:@"default.metallib"]] error:&err];
        if (!opalLib) die(@"metallib: %@", err);
        setupMetalPetal(mpFramework);

        // Looks, in the order Opal's main app lists them: it filters contentsOfDirectoryAtPath(Contents/Resources)
        // for *_pipeline.json without sorting (no sort call in the binary), i.e. file-system enumeration order.
        NSArray *files = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:mainRes error:&err]
            filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF ENDSWITH '_pipeline.json'"]];
        if (files.count == 0) die(@"no *_pipeline.json in %@", mainRes);
        NSMutableArray *looks = [NSMutableArray array];
        for (NSString *f in files) {
            NSData *d = [NSData dataWithContentsOfFile:[mainRes stringByAppendingPathComponent:f]];
            NSDictionary *j = [NSJSONSerialization JSONObjectWithData:d options:0 error:&err];
            NSArray *filters = j[@"filters"];
            if (!j || filters.count != 1 || [filters[0][@"values"] count] != 0) die(@"%@: unexpected pipeline (expected one filter, no values)", f);
            [looks addObject:@{ @"file" : f, @"name" : j[@"name"], @"filterName" : filters[0][@"filterName"] }];
        }

        printf("Opal: %s\nlooks (UI order): ", app.UTF8String);
        for (NSDictionary *l in looks) printf("%s ", [l[@"name"] UTF8String]);
        printf("\nLUT size %d\n\n", n);

        id<MTLTexture> inA = codeTexture(0), inB = codeTexture(1);
        id<MTLTexture> out8 = makeTex(MTLPixelFormatBGRA8Unorm, SIDE, SIDE);
        id<MTLTexture> out32 = makeTex(MTLPixelFormatRGBA32Float, SIDE, SIDE);
        size_t tabBytes = (size_t)NPIX * 3 * sizeof(float);
        float *tabA = malloc(tabBytes), *tabB = malloc(tabBytes);
        size_t gridN = (size_t)n * n * n;
        float *lut = malloc(gridN * 3 * sizeof(float));

        // grid input for direct evaluation of the Metal looks (float RGB at i/(n-1))
        NSUInteger gw = (NSUInteger)n * n, gh = (NSUInteger)n;  // n^3 pixels
        id<MTLTexture> gridIn = makeTex(MTLPixelFormatRGBA32Float, gw, gh), gridOut = makeTex(MTLPixelFormatRGBA32Float, gw, gh);
        {
            float *g = malloc(gridN * 16);
            for (size_t i = 0; i < gridN; i++) {
                g[i * 4 + 0] = (float)(i % n) / (n - 1);
                g[i * 4 + 1] = (float)((i / n) % n) / (n - 1);
                g[i * 4 + 2] = (float)(i / ((size_t)n * n)) / (n - 1);
                g[i * 4 + 3] = 1.0f;
            }
            [gridIn replaceRegion:MTLRegionMake2D(0, 0, gw, gh) mipmapLevel:0 withBytes:g bytesPerRow:gw * 16];
            free(g);
        }

        // ---- sanity: identity through the same Core Image path (CIAffineTransform with identity transform) ----
        {
            renderMTI(ciLookImage(mtiImage(inA), @"CIAffineTransform"), out8);
            readBGRA8(out8, 0, tabA);
            uint32_t diff = 0;
            for (uint32_t c = 0; c < NPIX; c++) {
                if (lroundf(tabA[c * 3] * 255) != (c & 255) || lroundf(tabA[c * 3 + 1] * 255) != ((c >> 8) & 255) ||
                    lroundf(tabA[c * 3 + 2] * 255) != (c >> 16)) diff++;
            }
            printf("sanity: identity through Opal's CI path (DeviceRGB in/working/out, BGRA8): %u of %u codes changed\n", diff, NPIX);
        }

        double yuvErr = probeYUV(opalLib);
        printf("sanity: Opal's NV12->RGB shader vs BT.709 full range (R = Y + 1.5748(Cr-0.5), ...): max diff %.2g%s\n\n",
               yuvErr, yuvErr >= 0 && yuvErr < 1e-4 ? " (match)" : " (MISMATCH: re-check the frame conversion)");

        NSMutableArray *index = [NSMutableArray array];
        int order = 0;
        BOOL allOK = YES;
        for (NSDictionary *l in looks) {
            NSString *name = l[@"name"], *filterName = l[@"filterName"];
            BOOL isCI = [filterName hasPrefix:@"CI"];
            if (isCI) {
                renderMTI(ciLookImage(mtiImage(inA), filterName), out8);
                readBGRA8(out8, 0, tabA);
                renderMTI(ciLookImage(mtiImage(inB), filterName), out8);
                readBGRA8(out8, 1, tabB);
            } else {
                id<MTLRenderPipelineState> ps = metalPipeline(opalLib, [NSString stringWithFormat:@"MT%@Fragment", filterName]);
                runMetal(ps, inA, out32);
                readRGBA32F(out32, 0, tabA, NPIX);
                runMetal(ps, inB, out32);
                readRGBA32F(out32, 1, tabB, NPIX);
            }
            // per-pixel check: same color at a different position must give the same result
            size_t mism = 0, nonfinite = 0, outOfRange = 0;
            double maxd = 0;
            for (size_t i = 0; i < (size_t)NPIX * 3; i++) {
                double d = fabs((double)tabA[i] - tabB[i]);
                if (d > 0) { mism++; if (d > maxd) maxd = d; }
                if (!isfinite(tabA[i])) nonfinite++;
                else if (tabA[i] < 0 || tabA[i] > 1) outOfRange++;
            }
            // grayscale check
            double maxChroma = 0;
            for (uint32_t c = 0; c < NPIX; c++) {
                float r = clamp01(tabA[c * 3]), g = clamp01(tabA[c * 3 + 1]), b = clamp01(tabA[c * 3 + 2]);
                double m = fmax(fabs(r - g), fmax(fabs(g - b), fabs(r - b)));
                if (m > maxChroma) maxChroma = m;
            }
            // LUT grid
            if (isCI) {
                for (size_t i = 0; i < gridN; i++) {
                    float r = (float)(i % n) / (n - 1), g = (float)((i / n) % n) / (n - 1), b = (float)(i / ((size_t)n * n)) / (n - 1);
                    tri(tabA, 256, r, g, b, &lut[i * 3]);  // trilinear between the 8-bit codes around the grid point
                }
            } else {
                id<MTLRenderPipelineState> ps = metalPipeline(opalLib, [NSString stringWithFormat:@"MT%@Fragment", filterName]);
                runMetal(ps, gridIn, gridOut);
                readRGBA32F(gridOut, -1, lut, gridN);
            }
            for (size_t i = 0; i < gridN * 3; i++) lut[i] = clamp01(lut[i]);
            ErrStats es = lutError(lut, n, tabA);

            NSString *cubeName = [NSString stringWithFormat:@"%@.cube", name];
            writeCube([outDir stringByAppendingPathComponent:cubeName], [NSString stringWithFormat:@"Opal %@ (%@)", name, filterName], lut, n);
            BOOL perPixel = (mism == 0);
            allOK &= perPixel;
            printf("%-3s %-22s %-6s per-pixel:%s%s  grayscale:%s (max |r-g|.. %.4f)  values outside [0,1]: %zu%s  LUT vs exact: max %.2f, mean %.3f, 99.9%% %.2f (8-bit units)\n",
                   name.UTF8String, filterName.UTF8String, isCI ? "CI" : "Metal", perPixel ? "yes" : "NO",
                   perPixel ? "" : [NSString stringWithFormat:@" (%zu channels differ, max %.4f)", mism, maxd].UTF8String,
                   maxChroma < 1.0 / 255 ? "yes" : "no", maxChroma, outOfRange,
                   nonfinite ? [NSString stringWithFormat:@", non-finite %zu", nonfinite].UTF8String : "",
                   es.maxErr, es.meanErr, es.p999);
            if (getenv("BAKE_DEBUG")) {
                uint32_t c = es.worstCode;
                printf("    worst: in (%u,%u,%u) exact (%.1f,%.1f,%.1f) lut (%.1f,%.1f,%.1f)\n", c & 255, (c >> 8) & 255, c >> 16,
                       tabA[c * 3] * 255, tabA[c * 3 + 1] * 255, tabA[c * 3 + 2] * 255, es.worstLut[0] * 255, es.worstLut[1] * 255, es.worstLut[2] * 255);
            }
            [index addObject:@{
                @"name" : name, @"file" : cubeName, @"filterName" : filterName, @"order" : @(order++),
                @"kind" : isCI ? @"coreimage" : @"metal", @"perPixel" : @(perPixel),
                @"lutMaxError8bit" : [NSDecimalNumber decimalNumberWithString:[NSString stringWithFormat:@"%.2f", es.maxErr]],
                @"lutMeanError8bit" : [NSDecimalNumber decimalNumberWithString:[NSString stringWithFormat:@"%.3f", es.meanErr]],
                @"lutP999Error8bit" : [NSDecimalNumber decimalNumberWithString:[NSString stringWithFormat:@"%.2f", es.p999]],
            }];

            // strength check on one look: Opal = MTIBlendFilter(normal), background = original, input = look,
            // intensity = OCFilters. Verify it equals mix(original, look, s) on 8-bit values.
            if ([name isEqualToString:@"I2"]) {
                float s = 0.71f;
                id orig = mtiImage(inA);
                id look = ciLookImage(orig, filterName);
                {
                    id blend = [[gMTIBlendFilter alloc] initWithBlendMode:gBlendNormal];
                    [blend setOutputPixelFormat:MTLPixelFormatBGRA8Unorm];
                    [blend setInputBackgroundImage:orig];
                    [blend setInputImage:look];
                    [blend setIntensity:s];
                    renderMTI([blend outputImage], out8);
                    readBGRA8(out8, 0, tabB);
                    double mx = 0, sum = 0;
                    for (uint32_t c = 0; c < NPIX; c++) {
                        float o[3] = { (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c >> 16) / 255.0f };
                        for (int k = 0; k < 3; k++) {
                            double want = o[k] + (tabA[c * 3 + k] - o[k]) * s;
                            double e = fabs(want - tabB[c * 3 + k]) * 255;
                            sum += e;
                            if (e > mx) mx = e;
                        }
                    }
                    printf("    strength check (%s, s=%.2f): MTIBlendFilter vs mix(orig, look, s): max %.2f, mean %.3f (8-bit units)\n",
                           name.UTF8String, s, mx, sum / (NPIX * 3.0));
                }
            }
        }

        NSDictionary *json = @{
            @"source" : @"Opal.app 0.26 (build 25.992), OpalCameraVideoService looks, baked by tools/win/luts/bake-luts.m",
            @"lutSize" : @(n),
            @"bakedOn" : [NSString stringWithFormat:@"macOS %@ (the Core Image looks come from this OS's CIPhotoEffect filters)",
                          [NSProcessInfo processInfo].operatingSystemVersionString],
            @"format" : @"Adobe .cube, LUT_3D_SIZE n, red fastest, domain [0,1]",
            @"colorSpace" : @"gamma-encoded (non-linear) RGB in -> gamma-encoded RGB out; Opal applies looks to BGRA8 frames "
                             "with no linearization (Core Image working/output space DeviceRGB, working format BGRA8; Metal looks "
                             "on BGRA8Unorm textures). Feed the LUT the same full-range R'G'B' that comes out of the NV12->RGB step.",
            @"strength" : @{
                @"setting" : @"OCFilters (Double 0..1, sent to the video service unchanged as Float via setFilterIntensityWithNewAmount:; the main app falls back to 0.5)",
                @"formula" : @"out = in + (lut(in) - in) * s   // MTIBlendFilter(.normal, intensity = s), background = unfiltered frame",
                @"onOff" : @"OCAreFiltersOn; selected look by name in OCSelectedFilter",
            },
            @"frameConversion" : @"Opal converts camera NV12 to RGB with its shader yuv2rgbconvert_opal_custom: BT.709 FULL range, "
                                  "chroma centered at 0.5 (R = Y + 1.5748(Cr-0.5); G = Y - 0.187324(Cb-0.5) - 0.468124(Cr-0.5); "
                                  "B = Y + 1.8556(Cb-0.5)), unclamped, into BGRA8. Back to YUV: Y = 0.2126R + 0.7152G + 0.0722B "
                                  "(full range). Verified by probing the shaders.",
            @"interpolation" : @"trilinear (GPU 3D texture, linear filter, texel centers at (i + 0.5) / n)",
            @"looks" : index,
        };
        NSData *jd = [NSJSONSerialization dataWithJSONObject:json options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&err];
        [jd writeToFile:[outDir stringByAppendingPathComponent:@"index.json"] atomically:YES];
        printf("\nwrote %lu LUTs + index.json to %s\n", (unsigned long)index.count, outDir.UTF8String);
        free(tabA); free(tabB); free(lut);
        return allOK ? 0 : 2;
    }
}

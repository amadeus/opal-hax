// preview: renders the Windows popover (app/ui.cpp + app/fluent.cpp, the same code Opal.exe draws) on macOS with
// Metal and writes PNG screenshots, so the design can be checked without a PC. Text uses a stand-in font (Windows has
// Segoe UI); the camera picture is a placeholder.
//   preview.sh [outdir]
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "../app/fluent.hpp"
#include "../app/ui.hpp"
#include "imgui.h"
#include "imgui_impl_metal.h"

using namespace opal;

static id<MTLDevice> gDev;
static id<MTLCommandQueue> gQ;

static id<MTLTexture> placeholder(int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for(int y = 0; y < h; y++)
        for(int x = 0; x < w; x++) {
            float u = (float)x / w, v = (float)y / h;
            // soft studio-ish backdrop with a figure-shaped blob (no camera data)
            float dx = (u - 0.5f) * 1.6f, dy = (v - 0.62f) * 1.0f;
            float head = std::exp(-((u - 0.5f) * (u - 0.5f) / 0.006f + (v - 0.42f) * (v - 0.42f) / 0.014f));
            float body = std::exp(-(dx * dx / 0.08f + std::max(0.f, -dy) * std::max(0.f, -dy) / 0.02f)) * (v > 0.58f ? 1.f : 0.f);
            float f = std::min(1.f, head + body);
            float r = 70 + 60 * u + 40 * v, g = 80 + 30 * u + 30 * v, b = 95 + 20 * (1 - u);
            r = r * (1 - f) + 150 * f, g = g * (1 - f) + 110 * f, b = b * (1 - f) + 95 * f;
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)b, p[1] = (uint8_t)g, p[2] = (uint8_t)r, p[3] = 255;
        }
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    id<MTLTexture> t = [gDev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px.data() bytesPerRow:w * 4];
    return t;
}

static void writePng(id<MTLTexture> tex, NSString* path) {
    int w = (int)tex.width, h = (int)tex.height;
    std::vector<uint8_t> px((size_t)w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(d, img, nullptr);
    CGImageDestinationFinalize(d);
    CFRelease(d), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}

struct Shot {
    std::string name;
    bool dark;
    int tab;
    std::function<void(AppSettings&)> setup;
    float scrollY = 0;                         // wheel notches
    ImVec2 hover = ImVec2(-1, -1);             // logical px
    ImVec2 click = ImVec2(-1, -1);             // logical px, clicked once (e.g. opens a combo box)
};

int main(int argc, char** argv) {
    @autoreleasepool {
        NSString* out = argc > 1 ? [NSString stringWithUTF8String:argv[1]] : @".";
        const char* fontReg = argc > 2 ? argv[2] : "/System/Library/Fonts/Supplemental/Tahoma.ttf";
        const char* fontBold = argc > 3 ? argv[3] : "/System/Library/Fonts/Supplemental/Tahoma Bold.ttf";
        float em = argc > 4 ? (float)atof(argv[4]) : 1.207f;
        float scale = argc > 5 ? (float)atof(argv[5]) : 1.5f;
        [[NSFileManager defaultManager] createDirectoryAtPath:out withIntermediateDirectories:YES attributes:nil error:nil];
        gDev = MTLCreateSystemDefaultDevice();
        gQ = [gDev newCommandQueue];
        const float LW = 400, LH = 740;  // popover size in logical px (main.cpp kPopW/kPopH)
        int W = (int)(LW * scale), H = (int)(LH * scale);

        IMGUI_CHECKVERSION();
        id<MTLTexture> pic = placeholder(640, 360);

        std::vector<std::string> looks = {"N1", "N2", "I1", "I2", "D1", "G1", "Q1", "S1", "X1", "M1", "T1", "C1", "B1"};
        float hist[64];
        for(int i = 0; i < 64; i++) hist[i] = std::exp(-(i - 26) * (i - 26) / 90.f) + 0.4f * std::exp(-(i - 48) * (i - 48) / 30.f);

        std::vector<Shot> shots = {
            {"1-camera-dark", true, 0, [](AppSettings& s) { s.filtersOn = true; s.bokeh = true; s.zoom = true; }},
            {"2-camera-dark-scrolled", true, 0, [](AppSettings& s) { s.filtersOn = true; s.bokeh = true; s.zoom = true; }, 6},
            {"3-effects-dark", true, 1, [](AppSettings& s) { s.touchUp = true; s.highlights = true; s.highlightsAmount = 0.3f; s.histogram = true; }},
            {"4-manual-dark", true, 2, [](AppSettings& s) { s.whiteBalanceAuto = false; }},
            {"5-audio-dark", true, 3, [](AppSettings&) {}},
            {"6-settings-dark", true, 4, [](AppSettings&) {}},
            {"7-camera-light", false, 0, [](AppSettings& s) { s.filtersOn = true; s.faceLock = true; }},
            {"8-audio-muted-light", false, 3, [](AppSettings& s) { s.micMuted = true; }},
            {"9-settings-combo-open-dark", true, 4, [](AppSettings&) {}, 0, ImVec2(-1, -1), ImVec2(330, 320)},
            {"10-manual-light-hover", false, 2, [](AppSettings&) {}, 0, ImVec2(300, 345)},
        };
        for(Shot& sh : shots) {
            ImGui::CreateContext();  // fresh state per shot (scroll positions, popups, animations)
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2((float)W, (float)H);
            ImGui_ImplMetal_Init(gDev);
            AppSettings s;
            sh.setup(s);
            UiHost host;
            host.settings = &s;
            host.looks = looks;
            host.preview = (ImTextureID)(intptr_t)(__bridge void*)pic;
            host.cameraStatus = "Streaming";
            host.cameraStreaming = true;
            host.fps = 30;
            host.vcamInUse = sh.tab == 4;
            host.chipTempC = 58.4f, host.cssCpu = 0.55f, host.mssCpu = 0.70f;
            host.histogram = hist;
            host.tab = sh.tab;
            fluent::setScale(scale);
            fluent::setTheme(sh.dark);
            fluent::applyStyle();
            fluent::loadFonts(fontReg, fontBold, em);
            ImGui_ImplMetal_DestroyFontsTexture();
            ImGui_ImplMetal_CreateFontsTexture(gDev);
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> rt = [gDev newTextureWithDescriptor:td];
            ImVec4 bg = ImGui::ColorConvertU32ToFloat4(fluent::C().bg);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            for(int f = 0; f < 90; f++) {
                io.DeltaTime = 1.f / 60;
                if(f == 2 && sh.scrollY != 0) io.AddMouseWheelEvent(0, -sh.scrollY);
                if(f == 1 && sh.scrollY != 0) io.AddMousePosEvent(200 * scale, 600 * scale);
                if(sh.click.x >= 0) {
                    if(f == 3) io.AddMousePosEvent(sh.click.x * scale, sh.click.y * scale);
                    if(f == 4) io.AddMouseButtonEvent(0, true);
                    if(f == 5) io.AddMouseButtonEvent(0, false);
                    if(f == 6) io.AddMousePosEvent(sh.click.x * scale, (sh.click.y + 40) * scale);
                }
                if(sh.hover.x >= 0 && f == 3) io.AddMousePosEvent(sh.hover.x * scale, sh.hover.y * scale);
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(bg.x, bg.y, bg.z, 1);
                ImGui_ImplMetal_NewFrame(rp);
                ImGui::NewFrame();
                drawUI(host);
                ImGui::Render();
                id<MTLCommandBuffer> cb = [gQ commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
            }
            NSString* path = [out stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.png", sh.name.c_str()]];
            writePng(rt, path);
            printf("%s\n", path.UTF8String);
            ImGui_ImplMetal_Shutdown();
            ImGui::DestroyContext();
        }
    }
    return 0;
}

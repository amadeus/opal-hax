// fpsfix: 30 fps instead of 26.4 at 1080p/1440p (2026-11-06, tools/profile + tools/README.md "frame rate").
// Opal's camera control messages carry several 3A commands at once (its stream start: START_STREAM + MOVE_LENS +
// AE_AUTO + AWB + SCENE + EXP_COMP + CONTROL_MODE + SATURATION) and its pipeline's initial control applies six at
// start. Any message applying two or more such commands makes the camera's 4K readout drop every 8th frame for the
// rest of the session (26.4 fps); one command per message, 40 ms apart, keeps 30.0 fps with no gaps (back-to-back
// singles do not). So:
//  1. every camera control Opal sends on its "control" queue with 2+ commands is sent as single commands, 40 ms apart,
//     synchronously on Opal's own thread (keeps the order with everything else it sends);
//  2. the initial control at pipeline start is cleared (nothing applied at start) and replayed the same way right
//     after the pipeline starts.
// Disable with env OPALCORE_NO_FPS_FIX. Log: ~/Library/Logs/opal-watch/camera.log "[fps]".
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "depthai/device/Device.hpp"
#include "depthai/pipeline/Pipeline.hpp"
#include "depthai/pipeline/datatype/CameraControl.hpp"
#include "depthai/pipeline/node/ColorCamera.hpp"

void opalcore_log(NSString *s);  // opalcore.mm

static const int kGapMs = 40;

// --------------------------------------------------------------- GOT patching (calls from the service into depthai)
static bool rebind(const char *mangled, void *replacement, void **original) {
    void *target = dlsym(RTLD_DEFAULT, mangled + 1);
    if(!target) return false;
    *original = target;
    const char *const kSects[][2] = {{"__DATA_CONST", "__got"}, {"__DATA", "__got"}, {"__DATA", "__la_symbol_ptr"}, {"__AUTH_CONST", "__auth_got"}};
    bool patched = false;
    for(uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if(!strstr(name, "OpalCameraDeviceService")) continue;
        auto *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        for(auto &s : kSects) {
            unsigned long size = 0;
            uint8_t *sect = getsectiondata(mh, s[0], s[1], &size);
            if(!sect) continue;
            void **slots = (void **)sect;
            for(unsigned long k = 0; k < size / sizeof(void *); k++) {
                if(slots[k] != target) continue;
                uintptr_t page = (uintptr_t)&slots[k] & ~(uintptr_t)(getpagesize() - 1);
                mprotect((void *)page, getpagesize(), PROT_READ | PROT_WRITE);
                slots[k] = replacement;
                patched = true;
            }
        }
    }
    return patched;
}

using StartFn = bool (*)(void *device, const dai::Pipeline &pipeline);
using SendFn = void (*)(void *queue, const dai::ADatatype &msg);
using SendPtrFn = void (*)(void *queue, const std::shared_ptr<dai::ADatatype> &msg);
using NameFn = std::string (*)(const void *queue);
static StartFn gOrigStart;
static SendFn gOrigSend;
static SendPtrFn gOrigSendPtr;
static NameFn gQueueName;
static std::atomic<int> gSplit{0};
// deferred initial control: Opal's own raw object (its layout and serializer; a copy made with our headers serialized
// wrongly and the service threw) and the command mask it had
static std::shared_ptr<dai::RawCameraControl> gPendingInit;
static uint64_t gPendingMask = 0;
static std::mutex gPendingMu;

static int popcount64(uint64_t v) {
    int n = 0;
    while(v) { n += (int)(v & 1); v >>= 1; }
    return n;
}

// send the control's commands one per message, in command order (START_STREAM is the lowest bit), kGapMs apart;
// the message wraps the caller's own raw object (Opal's layout), only its mask is changed and restored
static void sendSplit(void *queue, const std::shared_ptr<dai::RawCameraControl> &raw, uint64_t maskOverride = 0) {
    uint64_t mask = maskOverride ? maskOverride : raw->cmdMask;
    bool first = true;
    for(int bit = 0; bit < 64; bit++) {
        if(!(mask & (1ull << bit))) continue;
        if(!first) usleep(kGapMs * 1000);
        first = false;
        raw->cmdMask = 1ull << bit;
        dai::CameraControl one(raw);
        try {
            gOrigSend(queue, one);
        } catch(std::exception &e) {
            opalcore_log([NSString stringWithFormat:@"[fps] send failed: %s", e.what()]);
            break;
        }
    }
    raw->cmdMask = mask;
}

// a camera control with 2+ commands on the "control" queue: sent as single commands instead; false = not one
static bool splitIfNeeded(void *queue, const dai::ADatatype &msg) {
    if(getenv("OPALCORE_NO_FPS_FIX")) return false;
    auto rb = msg.getRaw();
    if(!rb || !gQueueName || gQueueName(queue) != "control") return false;  // Opal only sends camera controls there
    {   // the pipeline's initial control, deferred at start: replay it now, on the queue Opal itself created
        std::shared_ptr<dai::RawCameraControl> init;
        uint64_t mask = 0;
        {
            std::lock_guard<std::mutex> lk(gPendingMu);
            init.swap(gPendingInit);
            mask = gPendingMask;
        }
        if(init && mask) {
            sendSplit(queue, init, mask);
            init->cmdMask = 0;
            opalcore_log([NSString stringWithFormat:@"[fps] initial control replayed as %d single commands before Opal's first control message", popcount64(mask)]);
        }
    }
    auto rc = std::static_pointer_cast<dai::RawCameraControl>(rb);
    int n = popcount64(rc->cmdMask);
    if(n < 2) return false;
    int k = ++gSplit;
    if(k <= 6) opalcore_log([NSString stringWithFormat:@"[fps] camera control with %d commands (mask 0x%llx) sent as single commands", n, (unsigned long long)rc->cmdMask]);
    sendSplit(queue, rc);
    return true;
}

static void hook_send(void *queue, const dai::ADatatype &msg) {
    if(!splitIfNeeded(queue, msg)) gOrigSend(queue, msg);
}

static void hook_sendPtr(void *queue, const std::shared_ptr<dai::ADatatype> &msg) {
    if(!msg || !splitIfNeeded(queue, *msg)) gOrigSendPtr(queue, msg);
}

static bool hook_startPipeline(void *device, const dai::Pipeline &pipeline) {
    std::shared_ptr<dai::RawCameraControl> saved;
    uint64_t savedMask = 0;
    if(!getenv("OPALCORE_NO_FPS_FIX")) {
        try {
            auto nodes = pipeline.getAllNodes();
            std::string names;
            for(auto &n : nodes) names += std::string(n->getName()) + " ";
            opalcore_log([NSString stringWithFormat:@"[fps] startPipeline: %zu nodes: %s", nodes.size(), names.c_str()]);
            for(auto &n : nodes) {
                auto cam = std::dynamic_pointer_cast<dai::node::ColorCamera>(std::const_pointer_cast<dai::Node>(n));
                if(!cam && std::string(n->getName()) == "ColorCamera") cam = std::static_pointer_cast<dai::node::ColorCamera>(std::const_pointer_cast<dai::Node>(n));
                if(!cam) continue;
                auto raw = std::static_pointer_cast<dai::RawCameraControl>(cam->initialControl.getRaw());
                dai::RawCameraControl &prop = cam->properties.initialControl;
                int cmdsRaw = popcount64(raw->cmdMask), cmdsProp = popcount64(prop.cmdMask);
                opalcore_log([NSString stringWithFormat:@"[fps] ColorCamera initial control: %d commands via initialControl (0x%llx), %d via properties (0x%llx)", cmdsRaw,
                                                        (unsigned long long)raw->cmdMask, cmdsProp, (unsigned long long)prop.cmdMask]);
                if(cmdsRaw >= 2) {
                    saved = raw;  // Opal's object, kept alive by this reference
                    savedMask = raw->cmdMask;
                    raw->cmdMask = 0;  // apply nothing at start; replayed one command at a time before Opal's first control
                    prop.cmdMask = 0;
                }
            }
        } catch(std::exception &e) {
            opalcore_log([NSString stringWithFormat:@"[fps] pipeline inspection failed: %s", e.what()]);
        }
    }
    {
        std::lock_guard<std::mutex> lk(gPendingMu);
        gPendingInit = nullptr;
        gPendingMask = 0;
    }
    bool ok = gOrigStart(device, pipeline);
    if(ok && saved) {
        // experiment flags (files in ~/Library/Logs/opal-watch): fpsfix-noreplay = don't replay the initial control at
        // all; fpsfix-replay-subset = replay only what Opal never sends again at runtime (anti-banding, lens range limits)
        NSString *base = [@"~/Library/Logs/opal-watch" stringByExpandingTildeInPath];
        bool noReplay = [NSFileManager.defaultManager fileExistsAtPath:[base stringByAppendingPathComponent:@"fpsfix-noreplay"]];
        bool subset = [NSFileManager.defaultManager fileExistsAtPath:[base stringByAppendingPathComponent:@"fpsfix-replay-subset"]];
        if(subset) savedMask &= (1ull << 10);  // anti-banding only
        opalcore_log([NSString stringWithFormat:@"[fps] initial control deferred: %s (mask 0x%llx)", noReplay ? "NOT replayed" : subset ? "subset replayed" : "replayed",
                                                (unsigned long long)savedMask]);
        std::lock_guard<std::mutex> lk(gPendingMu);
        gPendingInit = noReplay ? nullptr : saved;  // sent by the first control message's hook (Opal creates the queue; we must not)
        gPendingMask = savedMask;
    }
    return ok;
}

void opalcore_fpsfix_init(void) {
    gQueueName = (NameFn)dlsym(RTLD_DEFAULT, "_ZNK3dai14DataInputQueue7getNameEv");
    bool a = rebind("__ZN3dai10DeviceBase13startPipelineERKNS_8PipelineE", (void *)hook_startPipeline, (void **)&gOrigStart);
    bool b = rebind("__ZN3dai14DataInputQueue4sendERKNS_9ADatatypeE", (void *)hook_send, (void **)&gOrigSend);
    bool c = rebind("__ZN3dai14DataInputQueue4sendERKNSt3__110shared_ptrINS_9ADatatypeEEE", (void *)hook_sendPtr, (void **)&gOrigSendPtr);
    opalcore_log([NSString stringWithFormat:@"[fps] 30 fps fix: startPipeline hook=%s, control send hooks=%s/%s%s", a ? "yes" : "NO", b ? "yes" : "NO", c ? "yes" : "NO",
                                            getenv("OPALCORE_NO_FPS_FIX") ? " (disabled by OPALCORE_NO_FPS_FIX)" : ""]);
}

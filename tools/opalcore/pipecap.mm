// pipecap: record what Opal's camera service sends to the camera, as reference for the Windows port.
// Off unless ~/Library/Logs/opal-watch/pipecap/ENABLE exists when the service starts (XPC services get no env).
// Output in ~/Library/Logs/opal-watch/pipecap/<timestamp>-<pid>/:
//   depthai.log      depthai host log at trace level: every RPC in full (setPipelineSchema with the exact
//                    LIBNOP property bytes, setAssets, buildPipeline, startPipeline, ...)
//   pipeline-N.json  PipelineImpl::serializeToJson() of the N-th started pipeline (readable properties, assets map,
//                    asset storage bytes)
//   controls.log     every message sent to the camera at runtime (DataInputQueue::send): time, stream name, size,
//                    wire bytes (StreamMessageParser::serializeMessage) as hex
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "depthai/pipeline/datatype/ADatatype.hpp"
#include "nlohmann/json.hpp"

static NSString *gDir;
static std::mutex gMu;
static std::atomic<int> gPipelineCount{0};

static void writeFile(NSString *name, const void *bytes, size_t len) {
    [[NSData dataWithBytes:bytes length:len] writeToFile:[gDir stringByAppendingPathComponent:name] atomically:NO];
}

static void appendLine(NSString *name, const std::string &line) {
    std::lock_guard<std::mutex> lk(gMu);
    NSString *path = [gDir stringByAppendingPathComponent:name];
    FILE *f = fopen(path.fileSystemRepresentation, "a");
    if(!f) return;
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
}

static double now() { return [NSDate timeIntervalSinceReferenceDate]; }

// --------------------------------------------------------------- GOT patching (calls from the service into depthai)
static bool rebind(const char *mangled, void *replacement, void **original) {
    void *target = dlsym(RTLD_DEFAULT, mangled + 1);  // dlsym wants the name without the leading underscore
    if(!target) return false;
    *original = target;
    const char *const kSects[][2] = {{"__DATA_CONST", "__got"}, {"__DATA", "__got"}, {"__DATA", "__la_symbol_ptr"},
                                     {"__AUTH_CONST", "__auth_got"}};
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

// --------------------------------------------------------------- hooks
using StartFn = bool (*)(void *device, const void *pipeline);
using SendFn = void (*)(void *queue, const dai::ADatatype &msg);
using ToJsonFn = nlohmann::json (*)(const void *pipelineImpl);
using SerializeMsgFn = std::vector<uint8_t> (*)(const dai::ADatatype &msg);
using NameFn = std::string (*)(const void *queue);

static StartFn gOrigStart;
static SendFn gOrigSend;
static ToJsonFn gToJson;
static SerializeMsgFn gSerializeMsg;
static NameFn gQueueName;

static bool hook_startPipeline(void *device, const void *pipeline) {
    int n = ++gPipelineCount;
    try {
        // dai::Pipeline's only member is std::shared_ptr<PipelineImpl>; its first word is the raw pointer
        const void *impl = *(const void *const *)pipeline;
        nlohmann::json j = gToJson(impl);
        std::string s = j.dump(1);
        writeFile([NSString stringWithFormat:@"pipeline-%d.json", n], s.data(), s.size());
        appendLine(@"controls.log", "--- startPipeline #" + std::to_string(n));
    } catch(std::exception &e) {
        appendLine(@"controls.log", std::string("--- serializeToJson failed: ") + e.what());
    }
    return gOrigStart(device, pipeline);
}

static void hook_send(void *queue, const dai::ADatatype &msg) {
    try {
        std::vector<uint8_t> wire = gSerializeMsg(msg);
        std::string name = gQueueName(queue);
        static const char *hex = "0123456789abcdef";
        std::string line = [[NSString stringWithFormat:@"%.3f %s %zu ", now(), name.c_str(), wire.size()] UTF8String];
        size_t shown = std::min<size_t>(wire.size(), 4096);  // control messages are small; cap image payloads
        line.reserve(line.size() + shown * 2);
        for(size_t i = 0; i < shown; i++) {
            line += hex[wire[i] >> 4];
            line += hex[wire[i] & 15];
        }
        appendLine(@"controls.log", line);
    } catch(...) {
    }
    gOrigSend(queue, msg);
}

void opalcore_pipecap_init(void) {
    NSString *base = [@"~/Library/Logs/opal-watch/pipecap" stringByExpandingTildeInPath];
    if(![NSFileManager.defaultManager fileExistsAtPath:[base stringByAppendingPathComponent:@"ENABLE"]]) return;
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateFormat = @"yyyyMMdd-HHmmss";
    gDir = [base stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%d", [f stringFromDate:NSDate.date], getpid()]];
    [NSFileManager.defaultManager createDirectoryAtPath:gDir withIntermediateDirectories:YES attributes:nil error:nil];
    // depthai reads DEPTHAI_LEVEL on first use; its spdlog sink is stdout
    setenv("DEPTHAI_LEVEL", "trace", 1);
    int fd = open([gDir stringByAppendingPathComponent:@"depthai.log"].fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if(fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    gToJson = (ToJsonFn)dlsym(RTLD_DEFAULT, "_ZNK3dai12PipelineImpl15serializeToJsonEv");
    gSerializeMsg = (SerializeMsgFn)dlsym(RTLD_DEFAULT, "_ZN3dai19StreamMessageParser16serializeMessageERKNS_9ADatatypeE");
    gQueueName = (NameFn)dlsym(RTLD_DEFAULT, "_ZNK3dai14DataInputQueue7getNameEv");
    bool a = gToJson && rebind("__ZN3dai10DeviceBase13startPipelineERKNS_8PipelineE", (void *)hook_startPipeline, (void **)&gOrigStart);
    bool b = gSerializeMsg && gQueueName && rebind("__ZN3dai14DataInputQueue4sendERKNS_9ADatatypeE", (void *)hook_send, (void **)&gOrigSend);
    appendLine(@"controls.log", std::string("pipecap active: startPipeline hook=") + (a ? "yes" : "NO") + " send hook=" + (b ? "yes" : "NO"));
}

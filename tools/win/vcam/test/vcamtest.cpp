// OpalVCamTest: exercises the "Opal C1" virtual camera without the Opal app.
//
// 1. (once, elevated) regsvr32 "C:\Program Files\Opal\OpalVirtualCamera.dll"   (folder readable by LocalService)
// 2. OpalVCamTest.exe  -> registers "Opal C1" for this session (MFCreateVirtualCamera) and starts it
// 3. open the Camera app (or Teams, a browser, OBS ...) and pick "Opal C1": dark gray placeholder until this tool
//    opens the frame channel, then a moving test pattern (scrolling gradient, bouncing white box, colored quadrants)
// 4. press Enter here: the camera is removed again
//
// It also plays the producer role the Opal app will play: wait for the media source (running in the Frame Server
// service) to create the shared mapping, then write NV12 frames into it under the seqlock described in
// tools/win/common/opal_vcam_shm.h.
#include "../mf_compat.h"
#include "../../common/opal_vcam_shm.h"

#include <stdio.h>
#include <string.h>

namespace {

void printError(const char* what, HRESULT hr) {
    wchar_t text[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr, 0, text,
                   (DWORD)(sizeof(text) / sizeof(text[0])), nullptr);
    fprintf(stderr, "%s failed: 0x%08lX %ls\n", what, (unsigned long)hr, text);
}

// ---- producer side of the frame channel ----------------------------------------------------------------------------

struct Channel {
    HANDLE mapping = nullptr;
    OpalVcamHeader* header = nullptr;
    BYTE* frame = nullptr;
    ULONGLONG openedAt = 0;

    // The consumer creates the mapping only while a stream is running, so this fails until an app opens the camera.
    bool open() {
        const wchar_t* names[] = {OPAL_VCAM_SHM_NAME_GLOBAL, OPAL_VCAM_SHM_NAME_LOCAL};
        for (const wchar_t* name : names) {
            // Exactly read + write: the DACL grants interactive users nothing more.
            HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
            if (!m) continue;
            void* view = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, OPAL_VCAM_SHM_SIZE);
            if (!view) {
                CloseHandle(m);
                continue;
            }
            auto* h = static_cast<OpalVcamHeader*>(view);
            MemoryBarrier();
            if (h->magic != OPAL_VCAM_MAGIC || h->version != OPAL_VCAM_VERSION || h->width != OPAL_VCAM_WIDTH ||
                h->height != OPAL_VCAM_HEIGHT || h->headerSize != OPAL_VCAM_HEADER_SIZE) {
                UnmapViewOfFile(view);  // not (yet) initialized by the consumer, or a different layout
                CloseHandle(m);
                continue;
            }
            mapping = m;
            header = h;
            frame = static_cast<BYTE*>(view) + h->headerSize;
            openedAt = GetTickCount64();
            printf("frame channel opened (%ls)\n", name);
            return true;
        }
        return false;
    }

    void close() {
        if (header) UnmapViewOfFile(header);
        if (mapping) CloseHandle(mapping);
        header = nullptr;
        frame = nullptr;
        mapping = nullptr;
    }

    // Our handle keeps the mapping alive even after the consumer closed it, so "mapping exists" stops meaning
    // "someone is watching" once we hold it. Use the consumer heartbeat instead and let go when it goes quiet; the
    // consumer recreates the mapping on its next stream start. (The real app needs the same logic.)
    bool consumerGone() const {
        ULONGLONG now = GetTickCount64();
        LONG64 beat = InterlockedCompareExchange64(&header->consumerHeartbeatMs, 0, 0);
        if (beat <= 0) return now - openedAt > 5000;
        return now - (ULONGLONG)beat > 3000;
    }
};

void drawPattern(BYTE* dst, unsigned frameNo) {
    const int w = OPAL_VCAM_WIDTH, h = OPAL_VCAM_HEIGHT, box = 200;
    // bouncing box position
    int span = w - box, spanY = h - box;
    int bx = (int)((frameNo * 7) % (2 * span)), by = (int)((frameNo * 5) % (2 * spanY));
    if (bx > span) bx = 2 * span - bx;
    if (by > spanY) by = 2 * spanY - by;

    for (int y = 0; y < h; y++) {
        BYTE* row = dst + (size_t)y * w;
        bool inBoxRow = y >= by && y < by + box;
        for (int x = 0; x < w; x++) {
            BYTE luma = (BYTE)(48 + (((x >> 2) + frameNo * 6) & 127));  // scrolling gradient, 48..175
            row[x] = (inBoxRow && x >= bx && x < bx + box) ? 235 : luma;
        }
    }
    // Chroma (interleaved U,V at half resolution): a different tint per quadrant.
    BYTE* uv = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        BYTE* row = uv + (size_t)y * w;
        bool top = y < h / 4;
        for (int x = 0; x < w / 2; x++) {
            bool left = x < w / 4;
            BYTE u = top ? (left ? 90 : 160) : (left ? 160 : 100);
            BYTE v = top ? (left ? 200 : 170) : (left ? 90 : 110);
            row[2 * x] = u;
            row[2 * x + 1] = v;
        }
    }
}

// Seqlock write: odd while the frame is being written, next even value once it is complete.
void writeFrame(Channel& c, unsigned frameNo) {
    LONG64 seq = c.header->writeSeq & ~1LL;  // we are the only writer; recover if a crashed writer left it odd
    InterlockedExchange64(&c.header->writeSeq, seq + 1);  // full barrier: odd is visible before any pixel changes
    drawPattern(c.frame, frameNo);
    InterlockedExchange64(&c.header->writeSeq, seq + 2);  // full barrier: pixels are visible before the even value
    InterlockedExchange64(&c.header->producerHeartbeatMs, (LONG64)GetTickCount64());
}

volatile LONG g_stop = 0;

DWORD WINAPI producerThread(void*) {
    Channel channel;
    unsigned frameNo = 0;
    LONG64 lastConsumerFrames = -1;
    ULONGLONG lastReport = 0;
    const double frameMs = 1000.0 / OPAL_VCAM_FPS;
    ULONGLONG start = GetTickCount64();

    while (!g_stop) {
        if (!channel.header) {
            if (!channel.open()) {
                Sleep(100);  // no app is streaming from Opal C1 yet
                continue;
            }
            start = GetTickCount64();
            frameNo = 0;
        }
        if (channel.consumerGone()) {
            printf("consumer stopped; waiting for the next stream\n");
            channel.close();
            continue;
        }

        writeFrame(channel, frameNo++);

        ULONGLONG now = GetTickCount64();
        if (now - lastReport >= 2000) {
            LONG64 frames = InterlockedCompareExchange64(&channel.header->consumerFrames, 0, 0);
            if (frames != lastConsumerFrames) printf("produced %u frames, consumer delivered %lld\n", frameNo, frames);
            lastConsumerFrames = frames;
            lastReport = now;
        }
        // Pace to ~30 fps against the start time (Sleep granularity makes single frames jitter, the average holds).
        ULONGLONG due = start + (ULONGLONG)(frameNo * frameMs);
        now = GetTickCount64();
        if (due > now) Sleep((DWORD)(due - now));
    }
    channel.close();
    return 0;
}

}  // namespace

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printError("CoInitializeEx", hr);
        return 1;
    }
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        printError("MFStartup", hr);
        CoUninitialize();
        return 1;
    }

    int exitCode = 1;
    IMFVirtualCamera* vcam = nullptr;
    // Resolved at runtime so the exe neither links mfsensorgroup.lib nor fails to load on older Windows.
    HMODULE sensorGroup = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto createVirtualCamera = sensorGroup ? reinterpret_cast<PFN_OpalMFCreateVirtualCamera>(reinterpret_cast<void*>(
                                                 GetProcAddress(sensorGroup, "MFCreateVirtualCamera")))
                                           : nullptr;
    if (!createVirtualCamera) {
        fprintf(stderr, "MFCreateVirtualCamera is not available: Windows 11 (build 22000 or later) is required.\n");
        goto done;
    }

    hr = createVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                             MFVirtualCameraAccess_CurrentUser, OPAL_VCAM_FRIENDLY_NAME, OPAL_VCAM_CLSID_STRING,
                             nullptr, 0, &vcam);
    if (FAILED(hr)) {
        printError("MFCreateVirtualCamera", hr);
        goto done;
    }
    hr = vcam->Start(nullptr);
    if (FAILED(hr)) {
        printError("IMFVirtualCamera::Start", hr);
        if (hr == REGDB_E_CLASSNOTREG)
            fprintf(stderr, "The media source is not registered: run  regsvr32 OpalVirtualCamera.dll  as administrator.\n");
        else if (hr == E_ACCESSDENIED)
            fprintf(stderr,
                    "Frame Server could not load OpalVirtualCamera.dll. Put it in a folder LocalService can read (e.g.\n"
                    "C:\\Program Files\\Opal), not under your user profile, unblock it if it was downloaded, and\n"
                    "register it again from there.\n");
        goto done;
    }

    {
        printf("\"Opal C1\" is running. Open the Camera app (or any video app) and select it.\n");
        printf("Press Enter to remove the camera and exit.\n");
        HANDLE thread = CreateThread(nullptr, 0, producerThread, nullptr, 0, nullptr);
        if (!thread) {
            printError("CreateThread", HRESULT_FROM_WIN32(GetLastError()));
        } else {
            getchar();
            InterlockedExchange(&g_stop, 1);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        exitCode = 0;
    }

done:
    if (vcam) {
        hr = vcam->Remove();
        if (FAILED(hr)) printError("IMFVirtualCamera::Remove", hr);
        hr = vcam->Shutdown();
        if (FAILED(hr)) printError("IMFVirtualCamera::Shutdown", hr);
        vcam->Release();
    }
    MFShutdown();
    CoUninitialize();
    return exitCode;
}

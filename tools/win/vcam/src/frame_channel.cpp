#include "frame_channel.h"

#include <sddl.h>
#include <string.h>

namespace opal {

namespace {

// D:P = protected DACL (no inheritance).
//   SY (LocalSystem), LS (LocalService: the Frame Server account), BA (Administrators): full access.
//   IU (interactive users: the Opal app), AU (authenticated users): read + write.
// GENERIC_READ/GENERIC_WRITE map to SECTION_MAP_READ/SECTION_MAP_WRITE (+ SECTION_QUERY), which is exactly what
// OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE) needs. The producer must not ask for FILE_MAP_ALL_ACCESS.
const wchar_t kSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)(A;;GRGW;;;AU)";

const uint64_t kProducerStaleMs = 1000;  // older producer heartbeat => show the placeholder
const int kCopyAttempts = 3;

HANDLE createMapping(const wchar_t* name, SECURITY_ATTRIBUTES* sa) {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, 0, (DWORD)OPAL_VCAM_SHM_SIZE, name);
}

// NV12 is height * 3 / 2 rows of `width` bytes; with the chroma plane at pitch * height the row index simply continues.
const UINT32 kRows = OPAL_VCAM_HEIGHT * 3 / 2;

void copyRows(BYTE* dst, LONG pitch, const BYTE* src) {
    if (pitch == OPAL_VCAM_WIDTH) {
        memcpy(dst, src, OPAL_VCAM_FRAME_BYTES);
        return;
    }
    for (UINT32 r = 0; r < kRows; r++) memcpy(dst + (size_t)r * pitch, src + (size_t)r * OPAL_VCAM_WIDTH, OPAL_VCAM_WIDTH);
}

void fillPlaceholder(BYTE* dst, LONG pitch) {
    for (UINT32 r = 0; r < kRows; r++)
        memset(dst + (size_t)r * pitch, r < OPAL_VCAM_HEIGHT ? 32 : 128, OPAL_VCAM_WIDTH);  // dark gray
}

}  // namespace

HRESULT FrameChannel::open() {
    if (header_) return S_OK;

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Log(L"frame channel: SDDL conversion failed 0x%08X", hr);
        return hr;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};

    // Global\ is what the producer looks for first. Creating a Global\ mapping from outside session 0 needs
    // SeCreateGlobalPrivilege, so if the source ever runs in a user-session process the Local\ fallback (which the
    // producer tries second) still connects them. In session 0, Local\ and Global\ are the same namespace anyway.
    const wchar_t* name = OPAL_VCAM_SHM_NAME_GLOBAL;
    HANDLE mapping = createMapping(name, &sa);
    if (!mapping) {
        Log(L"frame channel: creating %ls failed (%lu), trying Local", name, GetLastError());
        name = OPAL_VCAM_SHM_NAME_LOCAL;
        mapping = createMapping(name, &sa);
    }
    DWORD createError = GetLastError();  // ERROR_ALREADY_EXISTS when we opened an existing mapping
    LocalFree(sd);
    if (!mapping) {
        HRESULT hr = HRESULT_FROM_WIN32(createError);
        Log(L"frame channel: CreateFileMapping failed 0x%08X", hr);
        return hr;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, OPAL_VCAM_SHM_SIZE);
    if (!view) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Log(L"frame channel: MapViewOfFile failed 0x%08X", hr);
        CloseHandle(mapping);
        return hr;
    }

    auto* h = static_cast<OpalVcamHeader*>(view);
    bool existed = createError == ERROR_ALREADY_EXISTS;
    // A mapping can outlive us if the producer still holds a handle. Its seqlock/heartbeat state is then still valid
    // and a producer may be mid-write, so only (re)write the immutable description fields, never writeSeq.
    h->version = OPAL_VCAM_VERSION;
    h->headerSize = OPAL_VCAM_HEADER_SIZE;
    h->width = OPAL_VCAM_WIDTH;
    h->height = OPAL_VCAM_HEIGHT;
    h->fps = OPAL_VCAM_FPS;
    h->fourcc = MFVideoFormat_NV12.Data1;  // FOURCC 'NV12' (little-endian "NV12")
    MemoryBarrier();  // description visible before the magic that tells the producer it is valid
    h->magic = OPAL_VCAM_MAGIC;

    mapping_ = mapping;
    header_ = h;
    frame_ = static_cast<const BYTE*>(view) + OPAL_VCAM_HEADER_SIZE;
    if (last_.size() != OPAL_VCAM_FRAME_BYTES) last_.resize(OPAL_VCAM_FRAME_BYTES);
    haveLast_ = false;
    Log(L"frame channel: %ls %ls", existed ? L"opened existing" : L"created", name);
    return S_OK;
}

void FrameChannel::close() {
    if (header_) UnmapViewOfFile(header_);
    if (mapping_) CloseHandle(mapping_);
    header_ = nullptr;
    frame_ = nullptr;
    mapping_ = nullptr;
    haveLast_ = false;
}

bool FrameChannel::copyLatest() {
    for (int attempt = 0; attempt < kCopyAttempts; attempt++) {
        if (attempt) Sleep(1);  // a 3 MB producer copy takes well under a millisecond or two
        LONG64 before = header_->writeSeq;
        MemoryBarrier();
        if (before & 1) continue;  // producer is writing right now
        memcpy(last_.data(), frame_, OPAL_VCAM_FRAME_BYTES);
        MemoryBarrier();
        if (header_->writeSeq == before) return true;  // nothing changed under us: consistent frame
    }
    return false;
}

void FrameChannel::fill(BYTE* dst, LONG pitch) {
    if (header_) {
        LONG64 now = (LONG64)GetTickCount64();
        InterlockedExchange64(&header_->consumerHeartbeatMs, now);
        InterlockedIncrement64(&header_->consumerFrames);

        LONG64 produced = InterlockedCompareExchange64(&header_->producerHeartbeatMs, 0, 0);  // atomic read
        bool producerAlive = produced > 0 && (uint64_t)(now - produced) <= kProducerStaleMs;
        if (producerAlive) {
            if (copyLatest()) haveLast_ = true;  // else keep showing the last good frame
            if (haveLast_) {
                copyRows(dst, pitch, last_.data());
                return;
            }
        } else {
            haveLast_ = false;  // don't flash a stale frame when the producer comes back
        }
    }
    fillPlaceholder(dst, pitch);
}

}  // namespace opal

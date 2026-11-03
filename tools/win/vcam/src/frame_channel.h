// Consumer side of the shared-memory frame channel (tools/win/common/opal_vcam_shm.h).
//
// The media source runs inside the Windows Camera Frame Server service, so it is the side that CREATES the mapping
// (a session-0 service may create Global\ objects; a normal user process may not) and gives interactive users
// read/write access. The Opal app opens it and writes NV12 frames under a seqlock.
#pragma once
#include <vector>

#include "../../common/opal_vcam_shm.h"
#include "common.h"

namespace opal {

class FrameChannel {
public:
    FrameChannel() = default;
    ~FrameChannel() { close(); }
    FrameChannel(const FrameChannel&) = delete;
    FrameChannel& operator=(const FrameChannel&) = delete;

    // Creates (or opens, if it already exists) the named mapping and publishes the header. Safe to call when open.
    HRESULT open();
    void close();
    bool isOpen() const { return header_ != nullptr; }

    // Writes one OPAL_VCAM_WIDTH x OPAL_VCAM_HEIGHT NV12 frame into dst (rows `pitch` bytes apart, chroma plane at
    // dst + pitch * height): the producer's latest frame, the last good one, or the placeholder. Also bumps the
    // consumer heartbeat/frame counter. Works (placeholder only) when the channel is not open.
    void fill(BYTE* dst, LONG pitch);

private:
    bool copyLatest();  // seqlock read into last_; false if the producer kept writing during every attempt

    HANDLE mapping_ = nullptr;
    OpalVcamHeader* header_ = nullptr;
    const BYTE* frame_ = nullptr;  // header_ + headerSize
    std::vector<BYTE> last_;       // last frame copied consistently from the producer
    bool haveLast_ = false;
};

}  // namespace opal

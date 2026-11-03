// rawpkt: run Opal's template pipeline and print raw XLink packet sizes on a stream (debug: messages arriving
// without payload). usage: rawpkt <template dir> <stream> [n]
#include <cstdio>
#include "../core/opal_pipeline.hpp"
#include "depthai/xlink/XLinkStream.hpp"
int main(int argc, char** argv) {
    opal::PipelineTemplate t(argv[1], "full");
    auto p = t.build({});
    dai::Device dev(p);
    dai::CameraControl start;
    start.setStartStreaming();
    dev.getInputQueue("control")->send(start);
    dai::XLinkStream s(dev.getConnection(), argv[2], 1);
    int n = argc > 3 ? atoi(argv[3]) : 10;
    for(int i = 0; i < n; i++) {
        auto pkt = s.readMove();
        int type = *(int*)(pkt.data + pkt.length - 8), meta = *(int*)(pkt.data + pkt.length - 4);
        printf("packet %d: length=%u tail type=%d meta=%d first bytes:", i, pkt.length, type, meta);
        for(int k = 0; k < 8 && k < (int)pkt.length; k++) printf(" %02x", pkt.data[k]);
        printf("\n");
    }
}

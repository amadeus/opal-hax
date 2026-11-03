// opalstream: build Opal's captured app-mode pipeline from the template with our depthai, check it serializes like
// the capture, then run it on the camera and report what each output stream delivers.
// usage: opalstream <template dir> [seconds] [compare-with rpc-schema.json]
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include "../core/opal_pipeline.hpp"

int main(int argc, char** argv) {
    std::string dir = argv[1];
    int secs = argc > 2 ? atoi(argv[2]) : 10;
    opal::PipelineTemplate t(dir, "full");
    opal::PipelineOptions o;
    o.resolution = opal::Resolution::R1080;
    o.lensPosition = 136;
    auto p = t.build(o);
    if(argc > 3) {
        dai::PipelineSchema schema;
        dai::Assets assets;
        std::vector<uint8_t> storage;
        p.serialize(schema, assets, storage);
        nlohmann::json wire;
        std::ifstream(argv[3]) >> wire;
        int same = 0, diff = 0;
        for(auto& kv : wire["nodes"]) {
            auto& mine = schema.nodes.at(kv[0].get<int64_t>());
            if(mine.properties == kv[1]["properties"].get<std::vector<uint8_t>>() && mine.name == kv[1]["name"].get<std::string>()) same++;
            else { diff++; std::cout << "node " << kv[0] << " " << mine.name << " differs\n"; }
        }
        nlohmann::json mineConn = schema.connections;
        std::cout << "schema check: nodes same=" << same << " differ=" << diff << ", connections " << (mineConn.size() == wire["connections"].size() ? "same count" : "COUNT DIFFERS")
                  << ", storage " << storage.size() << " bytes\n";
    }
    if(secs <= 0) return 0;
    dai::Device dev(p);
    std::map<std::string, std::shared_ptr<dai::DataOutputQueue>> qs;
    for(auto& s : t.outputStreams()) qs[s] = dev.getOutputQueue(s, 8, false);
    // Opal turns streaming on with a CameraControl when the popover opens / an app uses the camera
    dai::CameraControl start;
    start.setStartStreaming();
    dev.getInputQueue("control")->send(start);
    std::map<std::string, int> count;
    std::map<std::string, size_t> lastSize;
    auto t0 = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
        bool any = false;
        for(auto& kv : qs) {
            while(auto m = kv.second->tryGet()) {
                any = true;
                count[kv.first]++;
                lastSize[kv.first] = m->getRaw()->data.size();
                if(kv.first == "rgb" && count["rgb"] % 60 == 1) {
                    auto f = std::dynamic_pointer_cast<dai::ImgFrame>(m);
                    printf("rgb %d: %ux%u type=%d bytes=%zu lens=%d exp=%dus iso=%d\n", count["rgb"], f->getWidth(), f->getHeight(), (int)f->getType(),
                           f->getData().size(), f->getLensPosition(), f->getExposureTime(), f->getSensitivity());
                }
            }
        }
        if(!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for(auto& kv : count) printf("%-28s %5d msgs (%.1f/s), last %zu bytes\n", kv.first.c_str(), kv.second, kv.second / (double)secs, lastSize[kv.first]);
}

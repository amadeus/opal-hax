// verify: does the public depthai-shared (our build) serialize every node's properties to exactly the bytes Opal's
// private library sent? Input: a pipecap session (pipeline-N.json = readable properties, rpc-schema-N.json = the
// LIBNOP bytes from the setPipelineSchema RPC). usage: verify <session dir> <N>
#include <fstream>
#include <iostream>
#include "depthai-shared/properties/AudioMicProperties.hpp"
#include "depthai-shared/properties/ColorCameraProperties.hpp"
#include "depthai-shared/properties/DetectionNetworkProperties.hpp"
#include "depthai-shared/properties/GlobalProperties.hpp"
#include "depthai-shared/properties/ImageManipProperties.hpp"
#include "depthai-shared/properties/NeuralNetworkProperties.hpp"
#include "depthai-shared/properties/ScriptProperties.hpp"
#include "depthai-shared/properties/SystemLoggerProperties.hpp"
#include "depthai-shared/properties/UACProperties.hpp"
#include "depthai-shared/properties/XLinkInProperties.hpp"
#include "depthai-shared/properties/XLinkOutProperties.hpp"
#include "depthai-shared/utility/Serialization.hpp"

template <typename P>
std::vector<uint8_t> ser(const nlohmann::json& j) {
    P p = j.get<P>();
    std::vector<uint8_t> out;
    dai::utility::serialize<dai::SerializationType::LIBNOP>(p, out);
    return out;
}

int main(int argc, char** argv) {
    std::string dir = argv[1], n = argv[2];
    nlohmann::json readable, wire;
    std::ifstream(dir + "/pipeline-" + n + ".json") >> readable;
    std::ifstream(dir + "/rpc-schema-" + n + ".json") >> wire;
    std::map<int64_t, std::vector<uint8_t>> wireProps;
    for(auto& kv : wire["nodes"]) wireProps[kv[0].get<int64_t>()] = kv[1]["properties"].get<std::vector<uint8_t>>();
    int ok = 0, bad = 0;
    for(auto& kv : readable["pipeline"]["nodes"]) {
        int64_t id = kv[0];
        std::string name = kv[1]["name"];
        auto& pj = kv[1]["properties"];
        std::vector<uint8_t> mine;
        try {
            if(name == "ColorCamera") mine = ser<dai::ColorCameraProperties>(pj);
            else if(name == "XLinkIn") mine = ser<dai::XLinkInProperties>(pj);
            else if(name == "XLinkOut") mine = ser<dai::XLinkOutProperties>(pj);
            else if(name == "UAC") mine = ser<dai::UACProperties>(pj);
            else if(name == "AudioMic") mine = ser<dai::AudioMicProperties>(pj);
            else if(name == "Script") mine = ser<dai::ScriptProperties>(pj);
            else if(name == "ImageManip") mine = ser<dai::ImageManipProperties>(pj);
            else if(name == "DetectionNetwork") mine = ser<dai::DetectionNetworkProperties>(pj);
            else if(name == "NeuralNetwork") mine = ser<dai::NeuralNetworkProperties>(pj);
            else if(name == "SystemLogger") mine = ser<dai::SystemLoggerProperties>(pj);
            else { std::cout << id << " " << name << ": no mapping\n"; bad++; continue; }
        } catch(std::exception& e) {
            std::cout << id << " " << name << ": from_json failed: " << e.what() << "\n";
            bad++;
            continue;
        }
        auto& w = wireProps[id];
        if(mine == w) { ok++; continue; }
        bad++;
        std::cout << id << " " << name << ": MISMATCH mine=" << mine.size() << "B wire=" << w.size() << "B first diff at ";
        size_t i = 0;
        while(i < mine.size() && i < w.size() && mine[i] == w[i]) i++;
        std::cout << i << "\n";
    }
    // global properties travel as JSON (msgpack) inside the RPC: compare key sets/values
    dai::GlobalProperties g = readable["pipeline"]["globalProperties"].get<dai::GlobalProperties>();
    nlohmann::json gj = g;
    for(auto& it : wire["globalProperties"].items())
        if(!gj.contains(it.key())) std::cout << "global: missing key " << it.key() << "\n";
    for(auto& it : gj.items())
        if(!wire["globalProperties"].contains(it.key())) std::cout << "global: extra key " << it.key() << "\n";
    std::cout << "nodes ok=" << ok << " mismatched=" << bad << "\n";
}

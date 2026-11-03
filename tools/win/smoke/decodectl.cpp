// decodectl: decode a pipecap controls.log (wire bytes of every message Opal sent to the camera) into readable JSON.
// usage: decodectl <controls.log>
#include <fstream>
#include <iostream>
#include <sstream>
#include "depthai/depthai.hpp"
#include "depthai/pipeline/datatype/StreamMessageParser.hpp"
#include "depthai-shared/datatype/RawAudioInConfig.hpp"
#include "depthai-shared/datatype/RawCameraControl.hpp"
#include "depthai-shared/datatype/RawImageManipConfig.hpp"
#include "depthai-shared/datatype/RawNNConfig.hpp"
#include "XLink/XLinkPublicDefines.h"

int main(int argc, char** argv) {
    std::ifstream f(argv[1]);
    std::string line;
    while(std::getline(f, line)) {
        std::istringstream ss(line);
        std::string t, stream, hex;
        size_t size;
        if(!(ss >> t >> stream >> size >> hex) || hex.size() != size * 2) {
            if(line.rfind("---", 0) == 0) std::cout << line << "\n";
            continue;
        }
        std::vector<uint8_t> b(size);
        for(size_t i = 0; i < size; i++) b[i] = (uint8_t)std::stoi(hex.substr(i * 2, 2), nullptr, 16);
        streamPacketDesc_t pkt{b.data(), (uint32_t)b.size()};
        nlohmann::json j;
        try {
            auto raw = dai::StreamMessageParser::parseMessage(&pkt);
            if(auto c = std::dynamic_pointer_cast<dai::RawCameraControl>(raw)) {
                j = *c;
                // name the commands set in cmdMask
                std::vector<int> cmds;
                for(int k = 0; k < 64; k++)
                    if(c->cmdMask & (1ull << k)) cmds.push_back(k);
                j["cmds"] = cmds;
            } else if(auto m = std::dynamic_pointer_cast<dai::RawImageManipConfig>(raw)) j = *m;
            else if(auto n = std::dynamic_pointer_cast<dai::RawNNConfig>(raw)) j = *n;
            else if(auto a = std::dynamic_pointer_cast<dai::RawAudioInConfig>(raw)) j = *a;
            else j = {{"type", "Buffer"}, {"data", raw->data}};
        } catch(std::exception& e) {
            j = {{"error", e.what()}};
        }
        std::cout << t << " " << stream << " " << j.dump() << "\n";
    }
}

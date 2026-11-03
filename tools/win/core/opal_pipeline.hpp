// opal_pipeline: rebuilds the exact pipeline Opal.app runs on the Opal C1, from a template captured off the real app
// (tools/opalcore/pipecap.mm -> tools/win/make-template.py). Every node is created through the public depthai API with
// the captured properties, so the bytes sent to the camera match Opal's (verified: tools/win/smoke/verify.cpp).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "depthai/depthai.hpp"

namespace opal {

enum class Resolution { R4K, R1440, R1080, R720, R360 };

struct PipelineOptions {
    Resolution resolution = Resolution::R1080;
    int lensPosition = -1;  // initial manual lens position (0..255); -1 keeps the template's
    int antiBanding = -1;   // dai::RawCameraControl::AntiBandingMode; -1 keeps the template's
    int xlinkChunkSize = -1;  // -1 keeps the template's (Opal: 0 = no chunking, one USB transfer per message)
};

class PipelineTemplate {
   public:
    // dir holds <variant>.json and assets/ (make-template.py output); variant "full" or "lowpower"
    PipelineTemplate(const std::string& dir, const std::string& variant);
    dai::Pipeline build(const PipelineOptions& opt) const;
    // names of the template's XLinkOut / XLinkIn streams
    std::vector<std::string> outputStreams() const;
    std::vector<std::string> inputStreams() const;

   private:
    std::string dir_;
    nlohmann::json tmpl_;
    std::vector<uint8_t> asset(const std::string& key) const;
};

// width/height of the "rgb" video stream for a resolution
void resolutionSize(Resolution r, int& w, int& h);

}  // namespace opal

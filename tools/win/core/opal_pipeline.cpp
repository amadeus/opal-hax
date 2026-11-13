#include "opal_pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace opal {

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if(!f) throw std::runtime_error("cannot read " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void resolutionSize(Resolution r, int& w, int& h) {
    switch(r) {
        case Resolution::R4K: w = 3840; h = 2160; break;
        case Resolution::R1440: w = 2560; h = 1440; break;
        case Resolution::R1080: w = 1920; h = 1080; break;
        case Resolution::R720: w = 1280; h = 720; break;
        case Resolution::R360: w = 640; h = 360; break;
    }
}

PipelineTemplate::PipelineTemplate(const std::string& dir, const std::string& variant) : dir_(dir) {
    std::ifstream f(dir + "/" + variant + ".json");
    if(!f) throw std::runtime_error("missing pipeline template " + dir + "/" + variant + ".json");
    f >> tmpl_;
    // Opal's pipeline has two XLinkIn nodes with the same stream name (ctrlGesturesPrePdImageManip feeds both the
    // palm-detection and the landmark ImageManip). Opening both closes one of them while the other still uses the
    // stream; on Windows that jammed XLink until the camera's watchdog reset it (~20 s). Give each its own name
    // ("<name>", "<name>2", ...), in node id order.
    std::map<std::string, int> seen;
    std::vector<nlohmann::json*> ins;
    for(auto& n : tmpl_["nodes"])
        if(n[1]["name"] == "XLinkIn") ins.push_back(&n);
    std::sort(ins.begin(), ins.end(), [](nlohmann::json* a, nlohmann::json* b) { return (*a)[0].get<int64_t>() < (*b)[0].get<int64_t>(); });
    for(auto* n : ins) {
        std::string name = (*n)[1]["properties"]["streamName"];
        int k = ++seen[name];
        if(k > 1) (*n)[1]["properties"]["streamName"] = name + std::to_string(k);
    }
}

std::vector<uint8_t> PipelineTemplate::asset(const std::string& key) const {
    return readFile(dir_ + "/" + tmpl_["assets"].at(key)["file"].get<std::string>());
}

std::vector<std::string> PipelineTemplate::outputStreams() const {
    std::vector<std::string> v;
    for(auto& n : tmpl_["nodes"])
        if(n[1]["name"] == "XLinkOut") v.push_back(n[1]["properties"]["streamName"]);
    return v;
}

std::vector<std::string> PipelineTemplate::inputStreams() const {
    std::vector<std::string> v;
    for(auto& n : tmpl_["nodes"])
        if(n[1]["name"] == "XLinkIn") v.push_back(n[1]["properties"]["streamName"]);
    return v;
}

template <typename N>
static std::shared_ptr<N> make(dai::Pipeline& p, const nlohmann::json& props) {
    auto n = p.create<N>();
    n->properties = props.get<typename N::Properties>();
    return n;
}

// Some nodes keep their initial control/config in a separate message object that overwrites the properties when the
// pipeline is serialized (getProperties()); copy the template's values into that object too.
template <typename Raw, typename Msg, typename Val>
static void setInitial(Msg& msg, const Val& v) {
    *std::static_pointer_cast<Raw>(msg.getRaw()) = v;
}

static std::shared_ptr<dai::Node> createNode(dai::Pipeline& p, const std::string& name, const nlohmann::json& props) {
    using namespace dai::node;
    if(name == "ColorCamera") {
        auto n = make<ColorCamera>(p, props);
        setInitial<dai::RawCameraControl>(n->initialControl, n->properties.initialControl);
        return n;
    }
    if(name == "ImageManip") {
        auto n = make<ImageManip>(p, props);
        setInitial<dai::RawImageManipConfig>(n->initialConfig, n->properties.initialConfig);
        return n;
    }
    if(name == "AudioMic") {
        auto n = make<AudioMic>(p, props);
        setInitial<dai::RawAudioInConfig>(n->initialConfig, n->properties.initialConfig);
        return n;
    }
    if(name == "XLinkIn") return make<XLinkIn>(p, props);
    if(name == "XLinkOut") return make<XLinkOut>(p, props);
    if(name == "UAC") return make<UAC>(p, props);
    if(name == "UVC") return make<UVC>(p, props);
    if(name == "Script") return make<Script>(p, props);
    if(name == "DetectionNetwork")  // abstract in the API; the schema name is the same for both families
        return props["nnFamily"].get<int>() == 1 ? std::shared_ptr<dai::Node>(make<MobileNetDetectionNetwork>(p, props))
                                                  : std::shared_ptr<dai::Node>(make<YoloDetectionNetwork>(p, props));
    if(name == "NeuralNetwork") return make<NeuralNetwork>(p, props);
    if(name == "SystemLogger") return make<SystemLogger>(p, props);
    throw std::runtime_error("pipeline template: unsupported node type " + name);
}

dai::Pipeline PipelineTemplate::build(const PipelineOptions& opt) const {
    dai::Pipeline p;
    auto& g = tmpl_["globalProperties"];
    // Opal's networks are compiled for OpenVINO 2021.4 (its log: "Device - OpenVINO version: 2021.4"); nodes created
    // from raw properties don't declare it, so force it or the firmware boots the 2020.3 variant and the NNs fail
    p.setOpenVINOVersion(dai::OpenVINO::VERSION_2021_4);
    p.setXLinkChunkSize(opt.xlinkChunkSize >= 0 ? opt.xlinkChunkSize : g["xlinkChunkSize"].get<int>());
    p.setImageManipCmxSizeAdjust(g["imageManipAdjustCmxSize"].get<int>());
    if(!opt.noTuningBlob && tmpl_["assets"].contains("/pipeline/camTuning")) {
        // the public API only takes a path for the sensor tuning blob
        p.setCameraTuningBlobPath(dir_ + "/" + tmpl_["assets"]["/pipeline/camTuning"]["file"].get<std::string>());
    }

    // nodes, in id order so the ids match the template (asset keys and connections use them)
    std::vector<nlohmann::json> nodes(tmpl_["nodes"].begin(), tmpl_["nodes"].end());
    std::sort(nodes.begin(), nodes.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a[0].get<int64_t>() < b[0].get<int64_t>(); });
    std::map<int64_t, std::shared_ptr<dai::Node>> byId;
    for(auto& n : nodes) {
        int64_t id = n[0];
        auto& info = n[1];
        nlohmann::json props = info["properties"];
        std::string name = info["name"];
        if(name == "AudioMic" && opt.micMuted) {
            props["initialConfig"]["config"]["micGain"] = 0.0;
            props["initialConfig"]["config"]["disableOutput"] = true;
        }
        if(name == "ColorCamera") {
            if(opt.resolution != Resolution::R4K) {
                static const std::map<Resolution, std::pair<int, int>> scale = {
                    {Resolution::R1440, {2, 3}}, {Resolution::R1080, {1, 2}}, {Resolution::R720, {1, 3}}, {Resolution::R360, {1, 6}}};
                auto s = scale.at(opt.resolution);
                props["ispScale"] = {{"horizNumerator", s.first}, {"horizDenominator", s.second}, {"vertNumerator", s.first}, {"vertDenominator", s.second}};
            } else {
                props["ispScale"] = {{"horizNumerator", 0}, {"horizDenominator", 0}, {"vertNumerator", 0}, {"vertDenominator", 0}};
            }
            int w, h;
            resolutionSize(opt.resolution, w, h);
            props["videoWidth"] = w;
            props["videoHeight"] = h;
            if(opt.sensorResolution >= 0) {
                props["resolution"] = opt.sensorResolution;
                props["ispScale"] = {{"horizNumerator", 0}, {"horizDenominator", 0}, {"vertNumerator", 0}, {"vertDenominator", 0}};
            }
            if(opt.fps > 0) props["fps"] = opt.fps;
            if(opt.orientation >= 0) props["imageOrientation"] = opt.orientation;
            if(opt.previewWidth > 0 && opt.previewHeight > 0) {
                props["previewWidth"] = opt.previewWidth;
                props["previewHeight"] = opt.previewHeight;
            }
            if(opt.lensPosition >= 0) props["initialControl"]["lensPosition"] = opt.lensPosition;
            if(opt.antiBanding >= 0) props["initialControl"]["antiBandingMode"] = opt.antiBanding;
            // Apply NOTHING at pipeline start: Opal's template applies 8 commands at once, and any camera control
            // applying 2+ commands makes the camera drop every 8th frame for the session (26.4 instead of 30 fps,
            // measured 2026-11-06). OpalCamera::sendInitialControls sends the same settings one command per message.
            props["initialControl"]["cmdMask"] = 0;
        }
        auto node = createNode(p, name, props);
        if(node->id != id) throw std::runtime_error("pipeline template: node id mismatch");
        byId[id] = node;
        // assets ("/node/<id>/<key>")
        std::string prefix = "/node/" + std::to_string(id) + "/";
        for(auto& a : tmpl_["assets"].items()) {
            if(a.key().rfind(prefix, 0) != 0) continue;
            std::string key = a.key().substr(prefix.size());
            auto nn = std::dynamic_pointer_cast<dai::node::NeuralNetwork>(node);
            if(nn && key == "__blob")
                nn->setBlob(dai::OpenVINO::Blob(asset(a.key())));  // also records the blob's OpenVINO version
            else
                node->getAssetManager().set(key, asset(a.key()), a.value()["alignment"].get<int>());
        }
        // Script I/O maps are created on first access
        if(auto script = std::dynamic_pointer_cast<dai::node::Script>(node)) {
            for(auto& io : info["ioInfo"]) {
                auto& d = io[1];
                if(d["group"] != "io") continue;
                if(d["type"].get<int>() == 3) script->inputs[d["name"].get<std::string>()];
                else script->outputs[d["name"].get<std::string>()];
            }
        }
        // input queue behaviour
        for(auto& io : info["ioInfo"]) {
            auto& d = io[1];
            if(d["type"].get<int>() != 3) continue;
            for(auto* in : node->getInputRefs()) {
                if(in->group == d["group"].get<std::string>() && in->name == d["name"].get<std::string>()) {
                    in->setBlocking(d["blocking"]);
                    in->setQueueSize(d["queueSize"]);
                    in->setWaitForMessage(d["waitForMessage"]);
                }
            }
        }
    }

    // links
    for(auto& c : tmpl_["connections"]) {
        auto src = byId.at(c["node1Id"].get<int64_t>());
        auto dst = byId.at(c["node2Id"].get<int64_t>());
        decltype(src->getOutputRefs())::value_type out = nullptr;
        decltype(dst->getInputRefs())::value_type in = nullptr;
        for(auto* o : src->getOutputRefs())
            if(o->name == c["node1Output"].get<std::string>() && o->group == c["node1OutputGroup"].get<std::string>()) out = o;
        for(auto* i : dst->getInputRefs())
            if(i->name == c["node2Input"].get<std::string>() && i->group == c["node2InputGroup"].get<std::string>()) in = i;
        if(!out || !in) throw std::runtime_error("pipeline template: unresolved connection");
        out->link(*in);
    }
    return p;
}

}  // namespace opal

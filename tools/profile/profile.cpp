// profile: run Opal's captured app-mode pipeline (tools/win template) on the camera with parts switched off,
// rate-limited or restructured, and record what the camera reports every second: LEON CSS/MSS CPU, the four chip
// temperature sensors, DDR/CMX/heap use, and the message rate of every output stream. The point is to find out what
// each piece of the pipeline costs (heat), without changing the picture that reaches the host.
//
// usage: profile <template dir> <seconds> [key=value ...]
//   name=<label>          run label (default from the options)
//   res=4k|1440|1080|720  "rgb" output resolution (ISP scale), default 1080
//   sensor=1080p|4k|12mp  sensor mode (default: the template's 4K; 1080p = on-sensor 2x2 binning, no ISP scaling)
//   fps=<f>               sensor frame rate (default template 30)
//   preview=WxH           ColorCamera preview size (default 640x360, feeds the networks)
//   face=0|1 seg=0|1 hands=0|1   networks on/off at runtime, the way Opal switches them (default 1,1,0: Opal with
//                         focus lock + bokeh, gestures off)
//   gate=face:N,seg:N,pd:N,lm:N  insert an on-camera Script between ColorCamera.preview and the named ImageManip
//                         that forwards every Nth frame (NN rate = sensor fps / N)
//   gateproc=css|mss      which LEON runs the gate script (default css)
//   rm=<id,id,...>        remove template nodes by id (and their connections/assets): structural experiments
//   stream=0              don't start the sensor (idle baseline)
//   log=warn|info|debug|trace   camera log level to print (default warn)
//   chunk=<bytes>         XLink chunk size (default template 0)
//   dump=<path prefix>    write one rgb frame at t=10s as <prefix>.nv12 (+ .txt with the size)
//   csv=<path>            per-second CSV (default ~/Library/Logs/opal-watch/profile/<name>-<time>.csv)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../standalone.hpp"
#include "../win/core/opal_pipeline.hpp"

using Clock = std::chrono::steady_clock;

// libusb (inside depthai's static XLink): Opal's "UVC device reset" vendor request puts a camera that is still running
// Opal's app-mode firmware (03e7:f63b, booted by a previous host session) back into its flashed program
extern "C" {
int libusb_init(void** ctx);
void* libusb_open_device_with_vid_pid(void* ctx, uint16_t vid, uint16_t pid);
int libusb_control_transfer(void* h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, unsigned char* d, uint16_t len, unsigned int timeout);
void libusb_close(void* h);
void libusb_exit(void* ctx);
int libusb_reset_device(void* h);
}
// a camera left in its bootloader (what a UVC-mode session leaves behind) boots the test firmware into a crash 1.4 s in;
// a USB port reset makes the bootloader load the flashed program (f63d), which is the state the good runs start from
static bool usbResetBootloader() {
    void* ctx = nullptr;
    if(libusb_init(&ctx) != 0) return false;
    void* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, 0xf63c);
    bool ok = false;
    if(h) {
        libusb_reset_device(h);
        ok = true;
        libusb_close(h);
    }
    libusb_exit(ctx);
    return ok;
}
static bool uvcResetBootedCamera() {
    void* ctx = nullptr;
    if(libusb_init(&ctx) != 0) return false;
    void* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, 0xf63b);
    bool ok = false;
    if(h) {
        ok = libusb_control_transfer(h, 0x00, 0xF5, 0x0DA1, 0, nullptr, 0, 1000) >= 0;
        libusb_close(h);
    }
    libusb_exit(ctx);
    return ok;
}

static std::map<std::string, std::string> parseArgs(int argc, char** argv, int from) {
    std::map<std::string, std::string> m;
    for(int i = from; i < argc; i++) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if(eq == std::string::npos) m[a] = "1";
        else m[a.substr(0, eq)] = a.substr(eq + 1);
    }
    return m;
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string item;
    while(std::getline(ss, item, sep))
        if(!item.empty()) v.push_back(item);
    return v;
}

static std::string nowStamp() {
    char buf[32];
    time_t t = time(nullptr);
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", localtime(&t));
    return buf;
}

// Template node ids (tools/vendor/opal-template/full.json): 0 ColorCamera, 10/11 face manip+NN, 15/16 segmenter
// manip+NN, 21/22 palm-detection manip+NN, 23 post-pd NN, 25/26 landmark manip+NN, 20 manager script.
static const std::map<std::string, int64_t> kGateTargets = {{"face", 10}, {"seg", 15}, {"pd", 21}, {"lm", 25}};

// remove a node, its connections and assets from the template
static void removeNode(nlohmann::json& t, int64_t id) {
    auto& nodes = t["nodes"];
    for(auto it = nodes.begin(); it != nodes.end();) {
        if((*it)[0].get<int64_t>() == id) it = nodes.erase(it);
        else ++it;
    }
    auto& conns = t["connections"];
    for(auto it = conns.begin(); it != conns.end();) {
        if((*it)["node1Id"].get<int64_t>() == id || (*it)["node2Id"].get<int64_t>() == id) it = conns.erase(it);
        else ++it;
    }
    std::string prefix = "/node/" + std::to_string(id) + "/";
    auto& assets = t["assets"];
    for(auto it = assets.begin(); it != assets.end();) {
        if(it.key().rfind(prefix, 0) == 0) it = assets.erase(it);
        else ++it;
    }
}

// Node ids must stay contiguous from 0 (the builder creates nodes in id order and checks the ids), so after removing
// nodes the remaining ones are renumbered.
static void renumber(nlohmann::json& t) {
    std::vector<int64_t> ids;
    for(auto& n : t["nodes"]) ids.push_back(n[0].get<int64_t>());
    std::sort(ids.begin(), ids.end());
    std::map<int64_t, int64_t> remap;
    for(size_t i = 0; i < ids.size(); i++) remap[ids[i]] = (int64_t)i;
    for(auto& n : t["nodes"]) {
        int64_t old = n[0].get<int64_t>();
        n[0] = remap[old];
        n[1]["id"] = remap[old];
    }
    for(auto& c : t["connections"]) {
        c["node1Id"] = remap[c["node1Id"].get<int64_t>()];
        c["node2Id"] = remap[c["node2Id"].get<int64_t>()];
    }
    nlohmann::json assets = nlohmann::json::object();
    for(auto& a : t["assets"].items()) {
        std::string k = a.key();
        if(k.rfind("/node/", 0) == 0) {
            auto rest = k.substr(6);
            auto slash = rest.find('/');
            int64_t old = std::stoll(rest.substr(0, slash));
            k = "/node/" + std::to_string(remap[old]) + rest.substr(slash);
        }
        assets[k] = a.value();
    }
    t["assets"] = assets;
}

// insert one Script node between ColorCamera.preview and the given ImageManip inputs, forwarding every Nth frame
static void insertGate(nlohmann::json& t, const std::map<int64_t, int>& divisors, bool onMss, const std::string& scriptPath, const std::string& assetFile) {
    int64_t cam = 0;
    int64_t newId = 0;
    for(auto& n : t["nodes"]) newId = std::max(newId, n[0].get<int64_t>() + 1);
    // script source
    std::string src = "NS = {";
    int k = 0;
    nlohmann::json io = nlohmann::json::array();
    io.push_back({{"io", "in"}, {{"blocking", false}, {"group", "io"}, {"name", "in"}, {"queueSize", 2}, {"type", 3}, {"waitForMessage", false}}});
    for(auto& d : divisors) {
        std::string out = "o" + std::to_string(k++);
        src += "'" + out + "': " + std::to_string(d.second) + ", ";
        io.push_back({{"io", out}, {{"blocking", false}, {"group", "io"}, {"name", out}, {"queueSize", 8}, {"type", 0}, {"waitForMessage", false}}});
    }
    src += "}\n";
    src +=
        "cnt = {}\n"
        "for name in NS:\n"
        "    cnt[name] = 0\n"
        "node.warn('gate: ' + str(NS))\n"
        "while True:\n"
        "    f = node.io['in'].get()\n"
        "    for name in NS:\n"
        "        cnt[name] += 1\n"
        "        if cnt[name] >= NS[name]:\n"
        "            cnt[name] = 0\n"
        "            node.io[name].send(f)\n";
    std::ofstream(scriptPath) << src;
    nlohmann::json node = {{"id", newId},
                           {"ioInfo", io},
                           {"name", "Script"},
                           {"properties", {{"processor", onMss ? 1 : 0}, {"scriptName", "gate"}, {"scriptUri", "asset:__script"}}}};
    t["nodes"].push_back({newId, node});
    t["assets"]["/node/" + std::to_string(newId) + "/__script"] = {{"file", assetFile}, {"alignment", 64}, {"size", (int64_t)src.size()}};
    // rewire
    k = 0;
    for(auto& d : divisors) {
        std::string out = "o" + std::to_string(k++);
        bool found = false;
        for(auto& c : t["connections"]) {
            if(c["node1Id"].get<int64_t>() == cam && c["node1Output"] == "preview" && c["node2Id"].get<int64_t>() == d.first) {
                c["node1Id"] = newId;
                c["node1Output"] = out;
                c["node1OutputGroup"] = "io";
                found = true;
            }
        }
        if(!found) throw std::runtime_error("gate: no preview connection to node " + std::to_string(d.first));
    }
    t["connections"].push_back({{"node1Id", cam}, {"node1Output", "preview"}, {"node1OutputGroup", ""}, {"node2Id", newId}, {"node2Input", "in"}, {"node2InputGroup", "io"}});
}

struct Sample {
    double t = 0;
    float css = 0, mss = 0;             // cpu %
    float tavg = 0, tcss = 0, tmss = 0, tupa = 0, tdss = 0;
    double ddr = 0, cmx = 0, cssHeap = 0, mssHeap = 0;  // MB used
    std::map<std::string, int> msgs;    // per stream in this second
    double latSum = 0; int latN = 0;    // rgb: capture timestamp -> arrival on the host, ms
    double bytes = 0;                   // rgb payload bytes this second
    int seqGaps = 0;                    // rgb: missing sequence numbers (frames the camera produced but never sent)
    int dt[4] = {0, 0, 0, 0};           // rgb: capture-to-capture interval histogram: <30, 30-36, 36-40, >40 ms
};

int main(int argc, char** argv) {
    if(argc < 3) {
        fprintf(stderr, "usage: profile <template dir> <seconds> [key=value ...]\n");
        return 2;
    }
    std::string dir = argv[1];
    int secs = atoi(argv[2]);
    auto args = parseArgs(argc, argv, 3);
    auto arg = [&](const std::string& k, const std::string& def) { return args.count(k) ? args[k] : def; };

    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string outDir = home + "/Library/Logs/opal-watch/profile";
    system(("mkdir -p '" + outDir + "'").c_str());

    opal::PipelineTemplate tmpl(dir, "full");
    auto& t = tmpl.json();

    // ---- structural edits
    std::string label;
    if(args.count("rm")) {
        for(auto& s : split(args["rm"], ',')) removeNode(t, std::stoll(s));
        renumber(t);
        label += "rm" + args["rm"] + "_";
    }
    // src=isp|preview|video: which ColorCamera output feeds the "rgb" XLinkOut (template: video)
    if(args.count("src")) {
        for(auto& c : t["connections"])
            if(c["node1Id"].get<int64_t>() == 0 && c["node1Output"] == "video" && c["node2Id"].get<int64_t>() == 9) c["node1Output"] = args["src"];
        label += "_src" + args["src"];
    }
    if(args.count("camin")) {  // camin=<queueSize>:<blocking>: the ColorCamera inputControl queue (template: 8, blocking)
        int qs = 8, bl = 1;
        sscanf(args["camin"].c_str(), "%d:%d", &qs, &bl);
        for(auto& n : t["nodes"])
            if(n[0].get<int64_t>() == 0)
                for(auto& io : n[1]["ioInfo"])
                    if(io[1]["name"] == "inputControl") { io[1]["queueSize"] = qs; io[1]["blocking"] = bl != 0; }
        label += "_camin" + args["camin"];
    }
    if(arg("relay", "0") == "1") {  // control XLinkIn -> Script relay -> ColorCamera.inputControl (as the flashed program feeds it from a Script)
        int64_t newId = 0;
        for(auto& n : t["nodes"]) newId = std::max(newId, n[0].get<int64_t>() + 1);
        std::string src = "while True:\n    node.io['out'].send(node.io['in'].get())\n";
        std::ofstream(dir + "/relay.py") << src;
        nlohmann::json io = nlohmann::json::array();
        io.push_back({{"io", "in"}, {{"blocking", true}, {"group", "io"}, {"name", "in"}, {"queueSize", 8}, {"type", 3}, {"waitForMessage", false}}});
        io.push_back({{"io", "out"}, {{"blocking", false}, {"group", "io"}, {"name", "out"}, {"queueSize", 8}, {"type", 0}, {"waitForMessage", false}}});
        t["nodes"].push_back({newId, {{"id", newId}, {"ioInfo", io}, {"name", "Script"}, {"properties", {{"processor", 0}, {"scriptName", "relay"}, {"scriptUri", "asset:__script"}}}}});
        t["assets"]["/node/" + std::to_string(newId) + "/__script"] = {{"file", "relay.py"}, {"alignment", 64}, {"size", (int64_t)src.size()}};
        for(auto& c : t["connections"])
            if(c["node1Id"].get<int64_t>() == 1 && c["node2Input"] == "inputControl") { c["node2Id"] = newId; c["node2Input"] = "in"; c["node2InputGroup"] = "io"; }
        t["connections"].push_back({{"node1Id", newId}, {"node1Output", "out"}, {"node1OutputGroup", "io"}, {"node2Id", 0}, {"node2Input", "inputControl"}, {"node2InputGroup", ""}});
        label += "_relay";
    }
    if(arg("rgbmeta", "0") == "1") {  // XLinkOut rgb sends metadata only (no pixel data over USB)
        for(auto& n : t["nodes"])
            if(n[0].get<int64_t>() == 9) n[1]["properties"]["metadataOnly"] = true;
        label += "_rgbmeta";
    }
    // rgbq=<size>:<blocking 0|1>: the rgb XLinkOut input queue (template: 3, non-blocking = drop oldest)
    if(args.count("rgbq")) {
        int qs = 3, bl = 0;
        sscanf(args["rgbq"].c_str(), "%d:%d", &qs, &bl);
        for(auto& n : t["nodes"])
            if(n[0].get<int64_t>() == 9)
                for(auto& io : n[1]["ioInfo"]) {
                    io[1]["queueSize"] = qs;
                    io[1]["blocking"] = bl != 0;
                }
        label += "_rgbq" + args["rgbq"];
    }
    // cam.<prop>=<value> / init.<prop>=<value>: raw ColorCamera properties / initialControl fields in the template
    for(auto& kv : args) {
        bool isCam = kv.first.rfind("cam.", 0) == 0, isInit = kv.first.rfind("init.", 0) == 0;
        if(!isCam && !isInit) continue;
        std::string key = kv.first.substr(isCam ? 4 : 5);
        nlohmann::json val = kv.second == "true" ? nlohmann::json(true) : kv.second == "false" ? nlohmann::json(false)
                             : kv.second.find('.') != std::string::npos ? nlohmann::json(atof(kv.second.c_str())) : nlohmann::json((int64_t)atoll(kv.second.c_str()));
        for(auto& n : t["nodes"])
            if(n[1]["name"] == "ColorCamera") (isCam ? n[1]["properties"] : n[1]["properties"]["initialControl"])[key] = val;
        label += "_" + key + "=" + kv.second;
    }
    bool faceOn = arg("face", "1") == "1", segOn = arg("seg", "1") == "1", handsOn = arg("hands", "0") == "1";
    label += std::string("f") + (faceOn ? "1" : "0") + "s" + (segOn ? "1" : "0") + "h" + (handsOn ? "1" : "0");
    if(args.count("choke")) label += "c" + args["choke"];
    if(args.count("gate")) {
        std::map<int64_t, int> div;
        for(auto& s : split(args["gate"], ',')) {
            auto colon = s.find(':');
            std::string name = s.substr(0, colon);
            int n = colon == std::string::npos ? 2 : atoi(s.c_str() + colon + 1);
            if(!kGateTargets.count(name)) throw std::runtime_error("gate: unknown target " + name);
            div[kGateTargets.at(name)] = n;
        }
        insertGate(t, div, arg("gateproc", "css") == "mss", dir + "/gate.py", "gate.py");  // asset paths are relative to the template dir
        label += "_gate" + args["gate"];
    }

    // ---- camera options
    opal::PipelineOptions o;
    std::string res = arg("res", "1080");
    o.resolution = res == "4k" ? opal::Resolution::R4K : res == "1440" ? opal::Resolution::R1440 : res == "720" ? opal::Resolution::R720 : res == "360" ? opal::Resolution::R360 : opal::Resolution::R1080;
    label += "_" + res;
    o.lensPosition = 136;
    if(args.count("sensor")) {
        std::string s = args["sensor"];
        // this depthai revision: THE_1080_P=0, THE_1200_P=1, THE_4_K=2 (Opal's template), THE_5_MP=3, THE_12_MP=4
        o.sensorResolution = s == "1080p" ? 0 : s == "1200p" ? 1 : s == "4k" ? 2 : s == "5mp" ? 3 : s == "12mp" ? 4 : -1;
        label += "_sensor" + s;
    }
    if(args.count("fps")) {
        o.fps = (float)atof(args["fps"].c_str());
        label += "_fps" + args["fps"];
    }
    for(const char* k : {"exp", "scene", "ctrlmode", "ab", "af", "noexpcomp", "nosat", "bare", "add", "seq", "seqdelay"}) if(args.count(k)) label += std::string("_") + k + args[k];
    if(arg("tuning", "opal") == "none") {
        o.noTuningBlob = true;
        label += "_notuning";
    }
    if(args.count("orient")) {  // ColorCamera imageOrientation: 0 auto, 1 normal, 2 flip H, 3 rotate 180 (Opal), 4 flip V
        o.orientation = atoi(args["orient"].c_str());
        label += "_orient" + args["orient"];
    }
    if(args.count("preview")) {
        sscanf(args["preview"].c_str(), "%dx%d", &o.previewWidth, &o.previewHeight);
        label += "_pv" + args["preview"];
    }
    if(args.count("chunk")) o.xlinkChunkSize = atoi(args["chunk"].c_str());
    bool stream = arg("stream", "1") == "1";
    if(!stream) label += "_idle";
    std::string name = arg("name", label);
    std::string csvPath = arg("csv", outDir + "/" + name + "-" + nowStamp() + ".csv");

    auto pipeline = tmpl.build(o);
    std::string uvcMode;
    if(arg("pipe", "opal") == "standalone") {
        std::string tuningPath = dir + "/" + t["assets"]["/pipeline/camTuning"]["file"].get<std::string>();
        pipeline = opal::buildStandalonePipeline(tuningPath, false, false);
        std::shared_ptr<dai::node::ColorCamera> cam;
        std::shared_ptr<dai::node::UVC> uvc;
        for(auto& n : pipeline.getAllNodes()) {
            if(auto c = std::dynamic_pointer_cast<dai::node::ColorCamera>(n)) cam = c;
            if(auto u = std::dynamic_pointer_cast<dai::node::UVC>(n)) uvc = u;
        }
        pipeline.remove(uvc);
        auto out = pipeline.create<dai::node::XLinkOut>();
        out->setStreamName("rgb");
        cam->video.link(out->input);
        auto in = pipeline.create<dai::node::XLinkIn>();
        in->setStreamName("control");
        in->out.link(cam->inputControl);
        name += "_standalone";
    }
    if(arg("pipe", "opal") == "manual") {
        // hand-built: the flashed program's camera settings (standalone.hpp) with XLink instead of UVC, plus its audio
        // nodes; knobs: man.preview=WxH man.af=auto|off man.ab=<n> man.video=WxH man.fps=<f> man.audio=0
        dai::Pipeline p;
        p.setCameraTuningBlobPath(dir + "/" + t["assets"]["/pipeline/camTuning"]["file"].get<std::string>());
        auto cam = p.create<dai::node::ColorCamera>();
        cam->setBoardSocket(dai::CameraBoardSocket::RGB);
        cam->setInterleaved(false);
        int pw = 1920, ph = 1080;
        if(args.count("man.preview")) sscanf(args["man.preview"].c_str(), "%dx%d", &pw, &ph);
        cam->setPreviewSize(pw, ph);
        cam->setImageOrientation(dai::CameraImageOrientation::ROTATE_180_DEG);
        if(arg("man.af", "auto") == "auto") cam->initialControl.setAutoFocusMode(dai::CameraControl::AutoFocusMode::AUTO);
        else cam->initialControl.setManualFocus(126);
        cam->initialControl.setAntiBandingMode((dai::CameraControl::AntiBandingMode)atoi(arg("man.ab", "2").c_str()));
        cam->setResolution(dai::ColorCameraProperties::SensorResolution::THE_4_K);
        cam->setIspScale(1, 2);
        if(args.count("man.video")) { int vw = 0, vh = 0; sscanf(args["man.video"].c_str(), "%dx%d", &vw, &vh); cam->setVideoSize(vw, vh); }
        if(args.count("man.fps")) cam->setFps((float)atof(args["man.fps"].c_str()));
        if(arg("man.audio", "1") == "1") {
            auto mic = p.create<dai::node::AudioMic>();
            auto uac = p.create<dai::node::UAC>();
            mic->setStreamBackMic(false);
            mic->out.link(uac->input);
            mic->initialConfig.setMicGainDecibels(28.0f);
        }
        if(arg("man.consumer", "xlink") == "uvc") {  // the flashed program's consumer
            auto uvc = p.create<dai::node::UVC>();
            cam->video.link(uvc->input);
            uvc->setGpiosOnInit({{58, 0}});
            uvc->setGpiosOnStreamOn({{58, 1}});
            uvc->setGpiosOnStreamOff({{58, 0}});
            uvcMode = "board";  // boot with BoardConfig.uvcEnable
        } else {
            auto out = p.create<dai::node::XLinkOut>();
            out->setStreamName("rgb");
            cam->video.link(out->input);
        }
        if(arg("man.control", "1") == "1") {
            auto in = p.create<dai::node::XLinkIn>();
            in->setStreamName("control");
            in->out.link(cam->inputControl);
        }
        if(arg("man.loop", "0") == "1") {  // an XLink stream pair not touching the camera: XLinkIn -> XLinkOut loopback
            auto li = p.create<dai::node::XLinkIn>();
            li->setStreamName("loopin");
            auto lo = p.create<dai::node::XLinkOut>();
            lo->setStreamName("loopout");
            li->out.link(lo->input);
        }
        float sysHz = (float)atof(arg("man.sysinfo", "0").c_str());
        if(sysHz > 0) {  // unrelated XLink traffic: SystemLogger -> XLinkOut at sysHz
            auto sl = p.create<dai::node::SystemLogger>();
            sl->setRate(sysHz);
            auto so = p.create<dai::node::XLinkOut>();
            so->setStreamName("sysinfo");
            sl->out.link(so->input);
        }
        pipeline = p;
        name += "_manual";
        for(auto& kv : args) if(kv.first.rfind("man.", 0) == 0) name += "_" + kv.first.substr(4) + kv.second;
    }
    if(args.count("openvino")) {  // firmware variant (the NN blobs need 2021.4; only meaningful with the NN branches removed)
        std::string v = args["openvino"];
        pipeline.setOpenVINOVersion(v == "2020.3" ? dai::OpenVINO::VERSION_2020_3 : v == "2020.4" ? dai::OpenVINO::VERSION_2020_4 : v == "2021.1" ? dai::OpenVINO::VERSION_2021_1
                                    : v == "2021.2" ? dai::OpenVINO::VERSION_2021_2 : v == "2021.3" ? dai::OpenVINO::VERSION_2021_3 : dai::OpenVINO::VERSION_2021_4);
        name += "_ov" + v;
    }
    // enc=h264|h265|mjpeg[:kbps]: ColorCamera.video -> VideoEncoder -> XLinkOut "rgb" (bitstream) instead of raw frames
    if(args.count("enc")) {
        std::string spec = args["enc"];
        auto colon = spec.find(':');
        std::string codec = spec.substr(0, colon);
        int kbps = colon == std::string::npos ? 0 : atoi(spec.c_str() + colon + 1);
        std::shared_ptr<dai::node::ColorCamera> cam;
        std::shared_ptr<dai::node::XLinkOut> rgbOut;
        for(auto& n : pipeline.getAllNodes()) {
            if(auto c = std::dynamic_pointer_cast<dai::node::ColorCamera>(n)) cam = c;
            if(auto x = std::dynamic_pointer_cast<dai::node::XLinkOut>(n))
                if(x->getStreamName() == "rgb") rgbOut = x;
        }
        if(!cam || !rgbOut) throw std::runtime_error("enc: camera or rgb output not found");
        pipeline.unlink(cam->video, rgbOut->input);
        auto enc = pipeline.create<dai::node::VideoEncoder>();
        using P = dai::VideoEncoderProperties::Profile;
        P prof = codec == "h265" ? P::H265_MAIN : codec == "mjpeg" ? P::MJPEG : P::H264_MAIN;
        enc->setDefaultProfilePreset(o.fps > 0 ? o.fps : 30.0f, prof);
        if(kbps > 0) enc->setBitrateKbps(kbps);
        if(prof != P::MJPEG) {
            enc->setNumBFrames(0);  // low latency: no reordering
            enc->setKeyframeFrequency(30);
        }
        cam->video.link(enc->input);
        enc->bitstream.link(rgbOut->input);
        name += "_" + spec;
        csvPath = arg("csv", outDir + "/" + name + "-" + nowStamp() + ".csv");
    }
    printf("profile: %s (%d s) -> %s\n", name.c_str(), secs, csvPath.c_str());
    fflush(stdout);

    // ---- find + boot the camera (a camera left running Opal's firmware by a previous session is reset first)
    dai::DeviceInfo info;
    for(int attempt = 0;; attempt++) {
        auto devices = dai::XLinkConnection::getAllConnectedDevices();
        bool usable = false;
        bool inBootloader = false;
        for(auto& d : devices) {
            printf("device %s state=%s\n", d.getMxId().c_str(), (d.state == X_LINK_BOOTED ? "BOOTED" : d.state == X_LINK_UNBOOTED ? "UNBOOTED" : d.state == X_LINK_BOOTLOADER ? "BOOTLOADER" : "FLASH_BOOTED/other"));
            if(d.state == X_LINK_BOOTLOADER) inBootloader = true;
            if(!usable && d.state != X_LINK_BOOTED && d.state != X_LINK_BOOTLOADER) { info = d; usable = true; }
        }
        if(!usable && inBootloader && attempt >= 3 && attempt < 6) {  // only after it sat in the bootloader for a while
            printf("camera is in its bootloader: USB reset so it loads the flashed program -> %s\n", usbResetBootloader() ? "sent" : "FAILED");
            std::this_thread::sleep_for(std::chrono::seconds(6));
            continue;
        }
        if(!usable && inBootloader) {  // still in the bootloader after resets: boot from there anyway
            for(auto& d : devices) if(d.state == X_LINK_BOOTLOADER) { info = d; usable = true; }
        }
        if(usable) break;
        if(!devices.empty()) {
            printf("camera still running a previous session: UVC reset -> %s\n", uvcResetBootedCamera() ? "sent" : "FAILED");
            std::this_thread::sleep_for(std::chrono::seconds(4));
        } else printf("no camera found, waiting\n");
        fflush(stdout);
        if(attempt > 20) { fprintf(stderr, "giving up\n"); return 1; }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    // uvc=board: boot the firmware in its UVC-enabled mode (BoardConfig.uvcEnable, as the flashed webcam program runs);
    // uvc=node: additionally add a UVC node consuming ColorCamera.video next to the XLinkOut (macOS then sees a UVC camera)
    if(arg("manip", "0") == "1") {  // video -> ImageManip (copy into its own 4-frame pool) -> XLinkOut rgb
        std::shared_ptr<dai::node::ColorCamera> cam;
        std::shared_ptr<dai::node::XLinkOut> rgbOut;
        for(auto& n : pipeline.getAllNodes()) {
            if(auto c = std::dynamic_pointer_cast<dai::node::ColorCamera>(n)) cam = c;
            if(auto x = std::dynamic_pointer_cast<dai::node::XLinkOut>(n))
                if(x->getStreamName() == "rgb") rgbOut = x;
        }
        pipeline.unlink(cam->video, rgbOut->input);
        auto manip = pipeline.create<dai::node::ImageManip>();
        manip->initialConfig.setResize(1920, 1080);
        manip->setMaxOutputFrameSize(1920 * 1080 * 3 / 2);
        manip->setNumFramesPool(4);
        cam->video.link(manip->inputImage);
        manip->out.link(rgbOut->input);
        name += "_manip";
    }
    if(uvcMode.empty()) uvcMode = arg("uvc", "");
    if(uvcMode == "only") {  // UVC node as the ONLY consumer of video (the XLinkOut rgb is unlinked)
        std::shared_ptr<dai::node::ColorCamera> cam;
        std::shared_ptr<dai::node::XLinkOut> rgbOut;
        for(auto& n : pipeline.getAllNodes()) {
            if(auto c = std::dynamic_pointer_cast<dai::node::ColorCamera>(n)) cam = c;
            if(auto x = std::dynamic_pointer_cast<dai::node::XLinkOut>(n))
                if(x->getStreamName() == "rgb") rgbOut = x;
        }
        pipeline.unlink(cam->video, rgbOut->input);
    }
    if(uvcMode == "node" || uvcMode == "only") {
        std::shared_ptr<dai::node::ColorCamera> cam;
        for(auto& n : pipeline.getAllNodes())
            if(auto c = std::dynamic_pointer_cast<dai::node::ColorCamera>(n)) cam = c;
        auto uvc = pipeline.create<dai::node::UVC>();
        cam->video.link(uvc->input);
        uvc->setGpiosOnInit({{58, 0}});
        uvc->setGpiosOnStreamOn({{58, 1}});
        uvc->setGpiosOnStreamOff({{58, 0}});
    }
    if(!uvcMode.empty()) name += "_uvc" + uvcMode;
    auto config = pipeline.getDeviceConfig();
    if(!uvcMode.empty()) config.board.uvcEnable = true;
    if(arg("blonly", "0") == "1") {  // just put the camera into its bootloader and exit (a defined state for other tools)
        try {
            dai::XLinkConnection::bootBootloader(info);
            printf("camera reset into the bootloader\n");
        } catch(std::exception& e) {
            printf("bootBootloader failed: %s\n", e.what());
        }
        return 0;
    }
    if(arg("preboot", "") == "bl") {  // put the camera into its bootloader first (a clean state before the firmware boot)
        try {
            info = dai::XLinkConnection::bootBootloader(info);
            printf("camera reset into the bootloader\n");
        } catch(std::exception& e) {
            printf("bootBootloader failed: %s\n", e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    auto tBoot = Clock::now();
    dai::Device dev(config, info);
    dev.startPipeline(pipeline);
    printf("booted in %.1f s, usb %d\n", std::chrono::duration<double>(Clock::now() - tBoot).count(), (int)dev.getUsbSpeed());
    std::string lvl = arg("log", "warn");
    dai::LogLevel ll = lvl == "trace" ? dai::LogLevel::TRACE : lvl == "debug" ? dai::LogLevel::DEBUG : lvl == "info" ? dai::LogLevel::INFO : dai::LogLevel::WARN;
    dev.setLogLevel(ll);
    dev.setLogOutputLevel(dai::LogLevel::OFF);
    std::mutex pmu;
    auto t0 = Clock::now();
    dev.addLogCallback([&](dai::LogMessage m) {
        std::lock_guard<std::mutex> lk(pmu);
        printf("  [cam %6.1f %s L%d] %s\n", std::chrono::duration<double>(Clock::now() - t0).count(), m.nodeIdName.c_str(), (int)m.level, m.payload.c_str());
        fflush(stdout);
    });

    // ---- queues
    std::vector<std::string> inNames, outNames;
    for(auto& n : pipeline.getAllNodes()) {
        if(auto x = std::dynamic_pointer_cast<dai::node::XLinkIn>(n)) inNames.push_back(x->getStreamName());
        if(auto x = std::dynamic_pointer_cast<dai::node::XLinkOut>(n)) outNames.push_back(x->getStreamName());
    }
    // noinq=1: open no XLinkIn queues on the host (nothing can be sent); inq=<name>[,<name>]: open only these
    std::map<std::string, std::shared_ptr<dai::DataInputQueue>> ins;
    if(arg("noinq", "0") != "1") {
        std::vector<std::string> only = args.count("inq") ? split(args["inq"], ',') : std::vector<std::string>();
        for(auto& s : inNames)
            if(only.empty() || std::find(only.begin(), only.end(), s) != only.end()) ins[s] = dev.getInputQueue(s, 8, false);
    }
    printf("input queues opened: %zu of %zu\n", ins.size(), inNames.size());
    auto send = [&](const std::string& s, const std::shared_ptr<dai::ADatatype>& m) {
        if(ins.count(s)) ins[s]->send(m);
    };
    std::mutex smu;
    Sample cur;
    bool haveSys = false;
    std::shared_ptr<dai::ImgFrame> dumpFrame;
    bool wantDump = args.count("dump") > 0, dumped = false;
    std::ofstream encDump;  // dumpenc=<path>: 5 s of the rgb payload (t = 10..15 s), e.g. the H.264 Annex B stream
    if(args.count("dumpenc")) encDump.open(args["dumpenc"], std::ios::binary);
    double dumpAt = 10.0;
    for(auto& s : outNames) {
        auto q = dev.getOutputQueue(s, 4, false);
        q->addCallback([&, s](std::shared_ptr<dai::ADatatype> m) {
            std::lock_guard<std::mutex> lk(smu);
            cur.msgs[s]++;
            if(s == "sysinfo") {
                auto si = std::dynamic_pointer_cast<dai::SystemInformation>(m);
                if(si) {
                    haveSys = true;
                    cur.css = si->leonCssCpuUsage.average * 100;
                    cur.mss = si->leonMssCpuUsage.average * 100;
                    cur.tavg = si->chipTemperature.average;
                    cur.tcss = si->chipTemperature.css;
                    cur.tmss = si->chipTemperature.mss;
                    cur.tupa = si->chipTemperature.upa;
                    cur.tdss = si->chipTemperature.dss;
                    cur.ddr = si->ddrMemoryUsage.used / 1048576.0;
                    cur.cmx = si->cmxMemoryUsage.used / 1048576.0;
                    cur.cssHeap = si->leonCssMemoryUsage.used / 1048576.0;
                    cur.mssHeap = si->leonMssMemoryUsage.used / 1048576.0;
                }
            } else if(s == "rgb") {
                auto f = std::dynamic_pointer_cast<dai::ImgFrame>(m);
                if(f) {
                    static int64_t lastSeq = -1;
                    static double lastTs = 0;
                    int64_t seq = f->getSequenceNum();
                    double ts = std::chrono::duration<double, std::milli>(f->getTimestamp().time_since_epoch()).count();
                    if(lastSeq >= 0) {
                        if(seq > lastSeq + 1) cur.seqGaps += (int)(seq - lastSeq - 1);
                        double d = ts - lastTs;
                        cur.dt[d < 30 ? 0 : d < 36 ? 1 : d < 40 ? 2 : 3]++;
                    }
                    lastSeq = seq;
                    lastTs = ts;
                    cur.latSum += std::chrono::duration<double, std::milli>(Clock::now() - f->getTimestamp()).count();
                    cur.latN++;
                    cur.bytes += f->getData().size();
                    double t = std::chrono::duration<double>(Clock::now() - t0).count();
                    if(encDump.is_open() && t > 10 && t < 15) encDump.write((const char*)f->getData().data(), f->getData().size());
                    if(wantDump && !dumped && t > dumpAt) { dumpFrame = f; dumped = true; }
                }
            }
        });
    }

    // ---- Opal's start sequence: networks as configured, LED, then START_STREAM with its usual controls
    {
        struct Net { const char *manip, *ctrl; int w, h; bool keepAR, on; };
        std::vector<Net> nets = {{"ctrlFaceImageManip", "ctrlFace", 300, 300, false, faceOn}, {"ctrlSegmenterImageManip", "ctrlSegmenter", 256, 144, true, segOn}};
        for(auto& n : nets) {
            auto m = std::make_shared<dai::ImageManipConfig>();
            m->setResize(n.w, n.h);
            m->setKeepAspectRatio(n.keepAR);
            m->setSkipCurrentImage(!n.on);
            send(n.manip, m);
            auto c = std::make_shared<dai::NNConfig>();
            c->setProcessInputs(n.on);
            send(n.ctrl, c);
        }
        for(auto mw : {std::make_pair("ctrlGesturesPrePdImageManip", 128), std::make_pair("ctrlGesturesPrePdImageManip2", 256)}) {
            auto m = std::make_shared<dai::ImageManipConfig>();
            m->setResize(mw.second, mw.second);
            m->setKeepAspectRatio(true);
            m->setSkipCurrentImage(!handsOn);
            send(mw.first, m);
        }
        auto hc = std::make_shared<dai::NNConfig>();
        hc->setProcessInputs(handsOn);
        send("ctrlGestures", hc);
        auto choke = std::make_shared<dai::Buffer>();
        bool chokeOn = args.count("choke") ? args["choke"] == "1" : handsOn;  // Opal sends [1] at every stream start
        choke->setData({(uint8_t)(chokeOn ? 1 : 0)});
        send("gesturesCtrlQueueChokeEmbedded", choke);
        if(stream) {
            auto led = std::make_shared<dai::Buffer>();
            led->setData(std::vector<uint8_t>{1, 1, 1, 1});
            send("ledCtrl", led);
            auto c = std::make_shared<dai::CameraControl>();
            c->setStartStreaming();
            bool bare = arg("bare", "0") == "1";  // bare=1: START_STREAM only (+ focus when af= is given), none of Opal's other fields
            if(arg("af", bare ? "none" : "manual") == "auto") c->setAutoFocusMode(dai::CameraControl::AutoFocusMode::AUTO);  // like the UVC program
            else if(arg("af", bare ? "none" : "manual") == "manual") c->setManualFocus(136);
            if(bare && args.count("add")) {  // add=ae,awb,scene,exp,ctrl,sat: single commands on top of the bare start
                for(auto& a : split(args["add"], ',')) {
                    if(a == "ae") c->setAutoExposureEnable();
                    if(a == "awb") c->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
                    if(a == "scene") c->setSceneMode(dai::CameraControl::SceneMode::FACE_PRIORITY);
                    if(a == "exp") c->setAutoExposureCompensation(0);
                    if(a == "ctrl") c->setControlMode(dai::CameraControl::ControlMode::AUTO);
                    if(a == "sat") c->setSaturation(0);
                }
            }
            if(bare && args.count("seq")) {  // seq=ae,awb,...: after the start, each command in its own message, 300 ms apart
                send("control", c);
                for(auto& a : split(args["seq"], ',')) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(atoi(arg("seqdelay", "300").c_str())));
                    auto m = std::make_shared<dai::CameraControl>();
                    if(a == "ae") m->setAutoExposureEnable();
                    if(a == "awb") m->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
                    if(a == "scene") m->setSceneMode(dai::CameraControl::SceneMode::FACE_PRIORITY);
                    if(a == "exp") m->setAutoExposureCompensation(0);
                    if(a == "ctrl") m->setControlMode(dai::CameraControl::ControlMode::AUTO);
                    if(a == "sat") m->setSaturation(0);
                    if(a == "focus") m->setManualFocus(136);
                    send("control", m);
                }
                c = nullptr;
            }
            if(!bare) {
            if(args.count("exp")) {  // exp=<us>:<iso>: manual exposure instead of Opal's auto exposure
                int us = 0, iso = 0;
                sscanf(args["exp"].c_str(), "%d:%d", &us, &iso);
                c->setManualExposure(us, iso);
            } else {
                c->setAutoExposureEnable();
            }
            c->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
            int scene = args.count("scene") ? atoi(args["scene"].c_str()) : 1;      // Opal: 1 = face priority
            int ctrlMode = args.count("ctrlmode") ? atoi(args["ctrlmode"].c_str()) : 1;  // Opal's start message: AUTO
            if(scene >= 0) c->setSceneMode((dai::CameraControl::SceneMode)scene);
            if(arg("noexpcomp", "0") != "1") c->setAutoExposureCompensation(0);
            if(ctrlMode >= 0) c->setControlMode((dai::CameraControl::ControlMode)ctrlMode);
            if(args.count("ab")) c->setAntiBandingMode((dai::CameraControl::AntiBandingMode)atoi(args["ab"].c_str()));
            if(arg("nosat", "0") != "1") c->setSaturation(0);
            }
            if(c) send("control", c);
        }
    }

    // ---- sample every second
    std::ofstream csv(csvPath);
    auto streams = outNames;
    csv << "t,css_cpu,mss_cpu,t_avg,t_css,t_mss,t_upa,t_dss,ddr_mb,cmx_mb,css_heap_mb,mss_heap_mb,rgb_lat_ms,rgb_bytes";
    for(auto& s : streams) csv << "," << s;
    csv << "\n";
    std::vector<Sample> hist;
    for(int sec = 1; sec <= secs; sec++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        Sample s;
        {
            std::lock_guard<std::mutex> lk(smu);
            s = cur;
            cur.msgs.clear();
            cur.latSum = 0; cur.latN = 0; cur.bytes = 0; cur.seqGaps = 0; cur.dt[0] = cur.dt[1] = cur.dt[2] = cur.dt[3] = 0;
        }
        s.t = std::chrono::duration<double>(Clock::now() - t0).count();
        if(!haveSys) {  // no SystemLogger in this pipeline variant: ask over RPC
            try {
                auto ct = dev.getChipTemperature();
                s.tavg = ct.average; s.tcss = ct.css; s.tmss = ct.mss; s.tupa = ct.upa; s.tdss = ct.dss;
                s.css = dev.getLeonCssCpuUsage().average * 100;
                s.mss = dev.getLeonMssCpuUsage().average * 100;
                s.ddr = dev.getDdrMemoryUsage().used / 1048576.0;
                s.cmx = dev.getCmxMemoryUsage().used / 1048576.0;
            } catch(std::exception& e) {
                printf("rpc failed: %s\n", e.what());
            }
        }
        hist.push_back(s);
        csv << s.t << "," << s.css << "," << s.mss << "," << s.tavg << "," << s.tcss << "," << s.tmss << "," << s.tupa << "," << s.tdss << "," << s.ddr << "," << s.cmx << "," << s.cssHeap << "," << s.mssHeap
            << "," << (s.latN ? s.latSum / s.latN : 0) << "," << s.bytes;
        for(auto& n : streams) csv << "," << s.msgs[n];
        csv << "\n";
        csv.flush();
        if(sec % 5 == 0 || sec <= 3) {
            std::lock_guard<std::mutex> lk(pmu);
            printf("%4d s  cpu css %4.0f%% mss %4.0f%%  temp %.1f (css %.1f mss %.1f upa %.1f dss %.1f)  ddr %.0fMB  rgb %d (%.1f ms, %.0f KB/frame) face %d seg %d hands %d\n", sec, s.css, s.mss, s.tavg,
                   s.tcss, s.tmss, s.tupa, s.tdss, s.ddr, s.msgs["rgb"], s.latN ? s.latSum / s.latN : 0, s.msgs["rgb"] ? s.bytes / s.msgs["rgb"] / 1024 : 0, s.msgs["face_detection"], s.msgs["google_meet_segmentation"], s.msgs["handpose_python_manager"]);
            fflush(stdout);
        }
        if(dumpFrame) {
            auto f = dumpFrame;
            dumpFrame = nullptr;
            std::string p = args["dump"];
            std::ofstream(p + ".nv12", std::ios::binary).write((const char*)f->getData().data(), f->getData().size());
            std::ofstream(p + ".txt") << f->getWidth() << "x" << f->getHeight() << " type " << (int)f->getType() << " bytes " << f->getData().size() << " lens " << f->getLensPosition()
                                      << " exp " << f->getExposureTime() << "us iso " << f->getSensitivity() << "\n";
            printf("dumped frame %ux%u to %s.nv12\n", f->getWidth(), f->getHeight(), p.c_str());
        }
    }

    // ---- summary over the last third (steady state), temperature slope over the last 60 s
    size_t n = hist.size(), from = n - std::max<size_t>(1, n / 3);
    Sample avg;
    for(size_t i = from; i < n; i++) {
        auto& s = hist[i];
        avg.css += s.css; avg.mss += s.mss; avg.tavg += s.tavg; avg.tcss += s.tcss; avg.tmss += s.tmss; avg.tupa += s.tupa; avg.tdss += s.tdss; avg.ddr += s.ddr; avg.cmx += s.cmx;
        for(auto& kv : s.msgs) avg.msgs[kv.first] += kv.second;
        avg.latSum += s.latSum; avg.latN += s.latN; avg.bytes += s.bytes; avg.seqGaps += s.seqGaps;
        for(int i = 0; i < 4; i++) avg.dt[i] += s.dt[i];
    }
    double k = (double)(n - from);
    size_t s60 = n > 60 ? n - 60 : 0;
    double slope = n > s60 + 10 ? (hist[n - 1].tavg - hist[s60].tavg) / (hist[n - 1].t - hist[s60].t) * 60.0 : NAN;
    printf("SUMMARY %s: last %.0f s: cpu css %.1f%% mss %.1f%% | temp %.1f (css %.1f mss %.1f upa %.1f dss %.1f) | dT/min %.2f | ddr %.0f MB cmx %.2f MB |", name.c_str(), k, avg.css / k,
           avg.mss / k, avg.tavg / k, avg.tcss / k, avg.tmss / k, avg.tupa / k, avg.tdss / k, slope, avg.ddr / k, avg.cmx / k);
    for(auto& kv : avg.msgs) printf(" %s %.1f/s", kv.first.c_str(), kv.second / k);
    printf(" | rgb latency %.1f ms, %.0f KB/frame, %.1f MB/s\n", avg.latN ? avg.latSum / avg.latN : 0, avg.latN ? avg.bytes / avg.latN / 1024 : 0, avg.bytes / k / 1048576);
    printf("FRAMES %s: received %d, sequence gaps (produced but not delivered) %d; capture intervals: <30ms %d, 30-36ms %d, 36-40ms %d, >40ms %d\n", name.c_str(), avg.latN,
           avg.seqGaps, avg.dt[0], avg.dt[1], avg.dt[2], avg.dt[3]);
    std::ofstream(outDir + "/summary.log", std::ios::app) << nowStamp() << " " << name << " " << secs << "s cpu css " << avg.css / k << " mss " << avg.mss / k << " temp " << avg.tavg / k
                                                          << " (css " << avg.tcss / k << " mss " << avg.tmss / k << " upa " << avg.tupa / k << " dss " << avg.tdss / k << ") dT/min " << slope
                                                          << " rgb " << avg.msgs["rgb"] / k << " face " << avg.msgs["face_detection"] / k << " seg " << avg.msgs["google_meet_segmentation"] / k
                                                          << " hands " << avg.msgs["handpose_python_manager"] / k << " start " << (hist.empty() ? 0 : hist[0].tavg) << "\n";
    if(stream) {
        auto c = std::make_shared<dai::CameraControl>();
        c->setStopStreaming();
        send("control", c);
        auto led = std::make_shared<dai::Buffer>();
        led->setData(std::vector<uint8_t>{1, 0, 0, 0});
        send("ledCtrl", led);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

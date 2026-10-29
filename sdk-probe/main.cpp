// obsbot-sdk-probe
//
// Minimal Linux SDK integration spike for the OBSBOT Tiny 3.
//
// Purpose: prove that the official OBSBOT device SDK (libdev v2.1.0_8)
// links and runs on Linux/x86_64, can discover a USB-connected camera,
// report its identity, and issue a small set of *safe* control commands.
//
// This is intentionally NOT the full GUI. It only exercises discovery and a
// handful of low-risk commands so we can validate the SDK path before
// investing in a Wayland-native control app.
//
// See README.md in this directory for build/run instructions and the
// runtime library-loading story.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <dev/devs.hpp>

namespace {

// Default USB discovery settling time. The SDK spins up a background USB
// detection task on first Devices::get(); give it a moment to enumerate.
constexpr int kDefaultWaitMs = 3000;

struct Options {
    bool status = false;   // print live camera status snapshot
    bool wake = false;     // set device run state -> Run
    bool sleep = false;    // set device run state -> Sleep
    bool center = false;   // reset gimbal to zero position (safe, no loops)
    bool enable_mdns = false;
    int wait_ms = kDefaultWaitMs;
    std::string sn;        // optional: target a specific device by SN

    // Feature probes (empty = not requested). Each one sends a single SDK
    // command, prints its rc, then prints the device-side readback.
    std::string track_speed;   // --track-speed
    std::string track;         // --track       (cameraSetAiModeU)
    std::string track_alt;     // --track-alt   (aiSetAiTrackModeEnabledR)
    std::string auto_zoom;     // --auto-zoom   (aiSetTargetZoomTypeR)
    std::string zone;          // --zone        (aiSetLimitedZoneTrackEnabledR)
    std::string zone_state;    // --zone-state  (aiSetZoneTrackStateR)
    bool zone_limits = false;  // --zone-limits
    float yaw_min = 0.f, yaw_max = 0.f, pitch_min = 0.f, pitch_max = 0.f;
    bool zone_read = false;    // --zone-read
    std::string hdr;           // --hdr

    // Limit-area tracking via the "tail2 and later" control-parameter API
    // (aiSetControlParaR / aiGetControlParaR, target = Human).
    bool czone_read = false;       // --czone-read
    bool czone_limits = false;     // --czone-limits
    float pan_min = 0.f, pan_max = 0.f, cpitch_min = 0.f, cpitch_max = 0.f;
    std::string czone;             // --czone on|off (TrackerType LimitArea|Normal)
    std::string czone_autosel;     // --czone-autoselect on|off
    bool cspeed_read = false;      // --cspeed-read
    int cspeed = -1;               // --cspeed N (DevGimCtrlSpeedMode)

    bool anyProbe() const {
        return !track_speed.empty() || !track.empty() || !track_alt.empty() ||
               !auto_zoom.empty() || !zone.empty() || !zone_state.empty() ||
               zone_limits || zone_read || !hdr.empty() || czone_read ||
               czone_limits || !czone.empty() || !czone_autosel.empty() ||
               cspeed_read || cspeed >= 0;
    }
};

// Latest status push from the SDK (~every 2-3 s once enabled). The SDK calls
// back on its own thread, so access is guarded and waiters are notified.
struct StatusCapture {
    std::mutex m;
    std::condition_variable cv;
    Device::CameraStatus last{};
    uint64_t count = 0;
};
StatusCapture g_status;

void onStatusPush(void *, const void *data) {
    if (!data) return;
    {
        std::lock_guard<std::mutex> lk(g_status.m);
        std::memcpy(&g_status.last, data, sizeof(Device::CameraStatus));
        ++g_status.count;
    }
    g_status.cv.notify_all();
}

// Wait until `pushes` NEW status pushes have arrived (the device's status lags
// a command by up to one push cycle, so 2 pushes gives a settled readback).
bool waitForPushes(int pushes, int timeout_ms, Device::CameraStatus &out) {
    std::unique_lock<std::mutex> lk(g_status.m);
    const uint64_t target = g_status.count + static_cast<uint64_t>(pushes);
    const bool ok = g_status.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                         [&] { return g_status.count >= target; });
    out = g_status.last;
    return ok;
}

const char *productName(ObsbotProductType type) {
    switch (type) {
    case ObsbotProdTiny:        return "Tiny";
    case ObsbotProdTiny4k:      return "Tiny4K";
    case ObsbotProdTiny2:       return "Tiny2";
    case ObsbotProdTiny2Lite:   return "Tiny2Lite";
    case ObsbotProdTailAir:     return "TailAir";
    case ObsbotProdMeet:        return "Meet";
    case ObsbotProdMeet4k:      return "Meet4K";
    case ObsbotProdMe:          return "Me";
    case ObsbotProdHDMIBox:     return "HDMIBox";
    case ObsbotProdNDIBox:      return "NDIBox";
    case ObsbotProdMeet2:       return "Meet2";
    case ObsbotProdTail2:       return "Tail2";
    case ObsbotProdTinySE:      return "TinySE";
    case ObsbotProdMeetSE:      return "MeetSE";
    case ObsbotProdTail2S:      return "Tail2S";
    case ObsbotProdTiny3:       return "Tiny3";      // enum value 18
    case ObsbotProdTiny3Lite:   return "Tiny3Lite";  // enum value 19
    default:                    return "Unknown";
    }
}

const char *devModeName(Device::DevMode mode) {
    switch (mode) {
    case Device::DevModeUvc: return "UVC";
    case Device::DevModeNet: return "Net";
    case Device::DevModeMtp: return "MTP";
    case Device::DevModeBle: return "BLE";
    default:                 return "Unknown";
    }
}

void printUsage(const char *argv0) {
    std::cout <<
        "Usage: " << argv0 << " [options]\n"
        "\n"
        "Discovers OBSBOT USB devices via the official SDK and prints their\n"
        "identity. With no command flag it only lists devices (read-only).\n"
        "\n"
        "Options:\n"
        "  --status        Print a live status snapshot (zoom / AI mode)\n"
        "  --wake          Set the selected device run state to Run\n"
        "  --sleep         Set the selected device run state to Sleep\n"
        "  --center        Reset the gimbal to its zero position (safe)\n"
        "  --sn <SN>       Target the device with this 14-char serial number\n"
        "  --wait-ms <N>   USB discovery settle time in ms (default "
                           << kDefaultWaitMs << ")\n"
        "  --enable-mdns   Enable mDNS/network scan (off by default)\n"
        "\n"
        "Feature probes (each prints the SDK rc, then the device readback):\n"
        "  --track-speed lazy|slow|standard|fast|crazy|auto\n"
        "                  aiSetTrackSpeedTypeR\n"
        "  --track off|solo|group\n"
        "                  cameraSetAiModeU(None|Human|Group)\n"
        "  --track-alt off|solo|group\n"
        "                  aiSetAiTrackModeEnabledR(Normal|HumanNormal|Group)\n"
        "  --auto-zoom off|full|upper|closeup|headless|lower|adaptive\n"
        "                  aiSetTargetZoomTypeR\n"
        "  --zone on|off   aiSetLimitedZoneTrackEnabledR\n"
        "  --zone-state on|off\n"
        "                  aiSetZoneTrackStateR\n"
        "  --zone-limits <yawMin> <yawMax> <pitchMin> <pitchMax>\n"
        "                  aiSetLimitedZoneTrack{Yaw,Pitch}{Min,Max}R (degrees)\n"
        "  --zone-read     Read back the limited-zone settings\n"
        "  --hdr on|off    cameraSetWdrR (also prints the WDR list + hdr_support)\n"
        "  --czone-read    Read limit-area params (aiGetControlParaR, Human)\n"
        "  --czone-limits <panMin> <panMax> <pitchMin> <pitchMax>\n"
        "                  aiSetControlParaR(Human, LimitPan/PitchMin/Max) (degrees)\n"
        "  --czone on|off  aiSetControlParaR(Human, TrackerType, LimitArea|Normal)\n"
        "  --czone-autoselect on|off\n"
        "  --cspeed-read   Read GimCtrlSpeedMode / GimCtrlMode / Motion (Human)\n"
        "  --cspeed N      aiSetControlParaR(Human, GimCtrlSpeedMode, N)\n"
        "                  0=SuperLazy 1=Lazy 2=Slow 3=Fast 4=Crazy\n"
        "                  aiSetControlParaR(Human, LimitAutoSelection)\n"
        "  -h, --help      Show this help\n"
        "\n"
        "Exit codes: 0 = at least one OBSBOT device found; non-zero otherwise.\n";
}

// Returns false if parsing failed / help was requested (caller should exit).
bool parseArgs(int argc, char **argv, Options &opt, int &exit_code) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            exit_code = 0;
            return false;
        } else if (a == "--status") {
            opt.status = true;
        } else if (a == "--wake") {
            opt.wake = true;
        } else if (a == "--sleep") {
            opt.sleep = true;
        } else if (a == "--center") {
            opt.center = true;
        } else if (a == "--enable-mdns") {
            opt.enable_mdns = true;
        } else if (a == "--track-speed" || a == "--track" || a == "--track-alt" ||
                   a == "--auto-zoom" || a == "--zone" || a == "--zone-state" ||
                   a == "--hdr") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << a << " requires a value\n";
                exit_code = 2;
                return false;
            }
            const std::string v = argv[++i];
            if (a == "--track-speed") opt.track_speed = v;
            else if (a == "--track") opt.track = v;
            else if (a == "--track-alt") opt.track_alt = v;
            else if (a == "--auto-zoom") opt.auto_zoom = v;
            else if (a == "--zone") opt.zone = v;
            else if (a == "--zone-state") opt.zone_state = v;
            else opt.hdr = v;
        } else if (a == "--zone-limits") {
            if (i + 4 >= argc) {
                std::cerr << "error: --zone-limits requires 4 values\n";
                exit_code = 2;
                return false;
            }
            opt.zone_limits = true;
            opt.yaw_min = std::strtof(argv[++i], nullptr);
            opt.yaw_max = std::strtof(argv[++i], nullptr);
            opt.pitch_min = std::strtof(argv[++i], nullptr);
            opt.pitch_max = std::strtof(argv[++i], nullptr);
        } else if (a == "--zone-read") {
            opt.zone_read = true;
        } else if (a == "--czone-read") {
            opt.czone_read = true;
        } else if (a == "--cspeed-read") {
            opt.cspeed_read = true;
        } else if (a == "--cspeed") {
            if (i + 1 >= argc) {
                std::cerr << "error: --cspeed requires a value\n";
                exit_code = 2;
                return false;
            }
            opt.cspeed = std::atoi(argv[++i]);
        } else if (a == "--czone" || a == "--czone-autoselect") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << a << " requires a value\n";
                exit_code = 2;
                return false;
            }
            (a == "--czone" ? opt.czone : opt.czone_autosel) = argv[++i];
        } else if (a == "--czone-limits") {
            if (i + 4 >= argc) {
                std::cerr << "error: --czone-limits requires 4 values\n";
                exit_code = 2;
                return false;
            }
            opt.czone_limits = true;
            opt.pan_min = std::strtof(argv[++i], nullptr);
            opt.pan_max = std::strtof(argv[++i], nullptr);
            opt.cpitch_min = std::strtof(argv[++i], nullptr);
            opt.cpitch_max = std::strtof(argv[++i], nullptr);
        } else if (a == "--sn") {
            if (i + 1 >= argc) {
                std::cerr << "error: --sn requires a value\n";
                exit_code = 2;
                return false;
            }
            opt.sn = argv[++i];
        } else if (a == "--wait-ms") {
            if (i + 1 >= argc) {
                std::cerr << "error: --wait-ms requires a value\n";
                exit_code = 2;
                return false;
            }
            opt.wait_ms = std::atoi(argv[++i]);
            if (opt.wait_ms < 0) opt.wait_ms = 0;
        } else {
            std::cerr << "error: unknown argument '" << a << "'\n";
            printUsage(argv[0]);
            exit_code = 2;
            return false;
        }
    }
    if (opt.wake && opt.sleep) {
        std::cerr << "error: --wake and --sleep are mutually exclusive\n";
        exit_code = 2;
        return false;
    }
    return true;
}

void printDevice(const std::shared_ptr<Device> &dev, int index) {
    const ObsbotProductType type = dev->productType();
    const bool is_tiny3 = (type == ObsbotProdTiny3);

    std::cout << "-----------------------------------------------------\n";
    std::cout << "  index:        " << index << "\n";
    std::cout << "  name:         " << dev->devName() << "\n";
    std::cout << "  SN:           " << dev->devSn() << "\n";
    std::cout << "  version:      " << dev->devVersion() << "\n";
    std::cout << "  product:      " << productName(type)
              << " (enum " << static_cast<int>(type) << ")\n";
    std::cout << "  mode:         " << devModeName(dev->devMode()) << "\n";
    std::cout << "  is Tiny 3:    " << (is_tiny3 ? "YES" : "no") << "\n";
}

// Device-pushed status for the tiny family. Read-only. uint8_t fields are
// cast to int so they print as numbers, not characters.
void printStatusFields(const Device::CameraStatus &st) {
    const auto &t = st.tiny;
    std::cout << "    dev_status=" << int(t.dev_status)
              << " ai_mode=" << int(t.ai_mode)
              << " ai_sub_mode=" << int(t.ai_sub_mode)
              << " ai_tracker_speed=" << int(t.ai_tracker_speed)
              << " ai_target=" << int(t.ai_target) << "\n";
    std::cout << "    zoom_ratio=" << t.zoom_ratio
              << " fov=" << int(t.fov)
              << " hdr=" << int(t.hdr)
              << " hdr_support=" << int(t.hdr_support)
              << " fps=" << int(t.fps) << "\n";
    std::cout << "    auto_frame.landscape=" << int(t.auto_frame.landscape_auto_frame)
              << " auto_frame.portrait=" << int(t.auto_frame.portrait_auto_frame)
              << " gesture_para.auto_frame_mode=" << int(t.gesture_para.auto_frame_mode)
              << " vertical=" << int(t.vertical) << "\n";
}

void printStatus(const std::shared_ptr<Device> &dev) {
    const ObsbotProductType type = dev->productType();
    std::cout << "  status snapshot:\n";
    switch (type) {
    case ObsbotProdTiny:
    case ObsbotProdTiny4k:
    case ObsbotProdTiny2:
    case ObsbotProdTiny3:
    case ObsbotProdTiny3Lite: {
        Device::CameraStatus st{};
        if (waitForPushes(1, 8000, st)) {
            printStatusFields(st);
        } else {
            std::cout << "    (no status push within 8 s)\n";
        }
        break;
    }
    default:
        std::cout << "    (no tiny-family status view for this product)\n";
        break;
    }
}

void printAiStatus(const std::shared_ptr<Device> &dev) {
    Device::AiStatus ai{};
    const int32_t rc = dev->aiGetAiStatusR(&ai);
    std::cout << "    aiGetAiStatusR rc=" << rc;
    if (rc == RM_RET_OK) {
        std::cout << " main_mode=" << int(ai.main_mode)
                  << " speed_mode=" << int(ai.speed_mode)
                  << " ai_zone_track=" << ai.ai_zone_track
                  << " ai_main_mode=" << int(ai.ai_main_mode)
                  << " ai_track_sub_mode=" << int(ai.ai_track_sub_mode);
    }
    std::cout << "\n";
}

// Print the settled device state after a command: two status pushes (the
// first may still be stale) plus the AiStatus getter.
void printReadback(const std::shared_ptr<Device> &dev) {
    Device::CameraStatus st{};
    if (waitForPushes(2, 10000, st)) {
        std::cout << "  readback (status push):\n";
        printStatusFields(st);
    } else {
        std::cout << "  readback: no settled status push within 10 s\n";
    }
    printAiStatus(dev);
}

void printZone(const std::shared_ptr<Device> &dev) {
    bool enabled = false, auto_sel = false;
    int32_t rc = dev->aiGetLimitedZoneTrackEnabledR(enabled);
    std::cout << "    aiGetLimitedZoneTrackEnabledR rc=" << rc << " enabled=" << enabled << "\n";
    rc = dev->aiGetLimitedZoneTrackAutoSelectR(auto_sel);
    std::cout << "    aiGetLimitedZoneTrackAutoSelectR rc=" << rc << " auto_select=" << auto_sel << "\n";
    float v = 0.f;
    rc = dev->aiGetLimitedZoneTrackYawMinR(v, 0);
    std::cout << "    yaw_min   rc=" << rc << " deg=" << v << "\n";
    rc = dev->aiGetLimitedZoneTrackYawMaxR(v, 0);
    std::cout << "    yaw_max   rc=" << rc << " deg=" << v << "\n";
    rc = dev->aiGetLimitedZoneTrackPitchMinR(v, 0);
    std::cout << "    pitch_min rc=" << rc << " deg=" << v << "\n";
    rc = dev->aiGetLimitedZoneTrackPitchMaxR(v, 0);
    std::cout << "    pitch_max rc=" << rc << " deg=" << v << "\n";
}

void printCtrlZone(const std::shared_ptr<Device> &dev) {
    const auto H = Device::DevControlTargetTypeHuman;
    int tracker = -1;
    int32_t rc = dev->aiGetControlParaR(H, Device::DevControlParaTypeTrackerType, tracker);
    std::cout << "    TrackerType        rc=" << rc << " value=" << tracker
              << " (0=Normal, 1=LimitArea)\n";
    bool autosel = false;
    rc = dev->aiGetControlParaR(H, Device::DevControlParaTypeLimitAutoSelection, autosel);
    std::cout << "    LimitAutoSelection rc=" << rc << " value=" << autosel << "\n";
    static const std::pair<const char *, Device::DevControlParaType> kLimits[] = {
        {"LimitPanMin  ", Device::DevControlParaTypeLimitPanMin},
        {"LimitPanMax  ", Device::DevControlParaTypeLimitPanMax},
        {"LimitPitchMin", Device::DevControlParaTypeLimitPitchMin},
        {"LimitPitchMax", Device::DevControlParaTypeLimitPitchMax}};
    for (const auto &l : kLimits) {
        float cur = 0.f, def = 0.f;
        const int32_t rc_cur = dev->aiGetControlParaR(H, l.second, cur, false);
        const int32_t rc_def = dev->aiGetControlParaR(H, l.second, def, true);
        std::cout << "    " << l.first << "      rc=" << rc_cur << " value=" << cur
                  << "   (default rc=" << rc_def << " value=" << def << ")\n";
    }
}

void printCtrlSpeed(const std::shared_ptr<Device> &dev) {
    const auto H = Device::DevControlTargetTypeHuman;
    int speed = -1, gim = -1;
    bool motion = false;
    int32_t rc = dev->aiGetControlParaR(H, Device::DevControlParaTypeGimCtrlSpeedMode, speed);
    std::cout << "    GimCtrlSpeedMode rc=" << rc << " value=" << speed
              << " (0=SuperLazy 1=Lazy 2=Slow 3=Fast 4=Crazy 100=Custom)\n";
    rc = dev->aiGetControlParaR(H, Device::DevControlParaTypeGimCtrlMode, gim);
    std::cout << "    GimCtrlMode      rc=" << rc << " value=" << gim << " (0=Normal 1=PRO)\n";
    rc = dev->aiGetControlParaR(H, Device::DevControlParaTypeMotion, motion);
    std::cout << "    Motion           rc=" << rc << " value=" << motion << "\n";
}

bool parseOnOff(const std::string &v, bool &out) {
    if (v == "on") { out = true; return true; }
    if (v == "off") { out = false; return true; }
    std::cerr << "error: expected on|off, got '" << v << "'\n";
    return false;
}

// Runs every requested feature probe on `dev`. Returns false on a bad value.
bool runProbes(const Options &opt, const std::shared_ptr<Device> &dev) {
    if (!opt.track_speed.empty()) {
        static const std::pair<const char *, Device::AiTrackSpeedType> kMap[] = {
            {"lazy", Device::AiTrackSpeedLazy},         {"slow", Device::AiTrackSpeedSlow},
            {"standard", Device::AiTrackSpeedStandard}, {"fast", Device::AiTrackSpeedFast},
            {"crazy", Device::AiTrackSpeedCrazy},       {"auto", Device::AiTrackSpeedAuto}};
        auto it = std::find_if(std::begin(kMap), std::end(kMap),
                               [&](const auto &p) { return opt.track_speed == p.first; });
        if (it == std::end(kMap)) {
            std::cerr << "error: bad --track-speed '" << opt.track_speed << "'\n";
            return false;
        }
        const int32_t rc = dev->aiSetTrackSpeedTypeR(it->second);
        std::cout << "\n--track-speed " << it->first << "  aiSetTrackSpeedTypeR("
                  << int(it->second) << ") -> rc=" << rc << "\n";
        printReadback(dev);
    }
    if (!opt.track.empty()) {
        Device::AiWorkModeType mode;
        if (opt.track == "off") mode = Device::AiWorkModeNone;
        else if (opt.track == "solo") mode = Device::AiWorkModeHuman;
        else if (opt.track == "group") mode = Device::AiWorkModeGroup;
        else { std::cerr << "error: bad --track '" << opt.track << "'\n"; return false; }
        const int32_t rc = dev->cameraSetAiModeU(mode);
        std::cout << "\n--track " << opt.track << "  cameraSetAiModeU(" << int(mode)
                  << ") -> rc=" << rc << "\n";
        printReadback(dev);
    }
    if (!opt.track_alt.empty()) {
        Device::AiTrackModeType mode;
        bool enabled = true;
        if (opt.track_alt == "off") { mode = Device::AiTrackNormal; enabled = false; }
        else if (opt.track_alt == "solo") mode = Device::AiTrackHumanNormal;
        else if (opt.track_alt == "group") mode = Device::AiTrackGroup;
        else { std::cerr << "error: bad --track-alt '" << opt.track_alt << "'\n"; return false; }
        const int32_t rc = dev->aiSetAiTrackModeEnabledR(mode, enabled);
        std::cout << "\n--track-alt " << opt.track_alt << "  aiSetAiTrackModeEnabledR("
                  << int(mode) << ", " << enabled << ") -> rc=" << rc << "\n";
        printReadback(dev);
    }
    if (!opt.auto_zoom.empty()) {
        static const std::pair<const char *, Device::DevTargetZoomType> kMap[] = {
            {"off", Device::DevTargetZoomTypeNormal},
            {"full", Device::DevTargetZoomTypeFullBody},
            {"upper", Device::DevTargetZoomTypeHalfBody},
            {"closeup", Device::DevTargetZoomTypeCloseUp},
            {"headless", Device::DevTargetZoomTypeGropHeadless},
            {"lower", Device::DevTargetZoomTypeGropLowerBody},
            {"adaptive", Device::DevTargetZoomTypeAdaptive}};
        auto it = std::find_if(std::begin(kMap), std::end(kMap),
                               [&](const auto &p) { return opt.auto_zoom == p.first; });
        if (it == std::end(kMap)) {
            std::cerr << "error: bad --auto-zoom '" << opt.auto_zoom << "'\n";
            return false;
        }
        const int32_t rc = dev->aiSetTargetZoomTypeR(it->second);
        std::cout << "\n--auto-zoom " << it->first << "  aiSetTargetZoomTypeR("
                  << int(it->second) << ") -> rc=" << rc << "\n";
        printReadback(dev);
    }
    if (!opt.zone_state.empty()) {
        bool on = false;
        if (!parseOnOff(opt.zone_state, on)) return false;
        const int32_t rc = dev->aiSetZoneTrackStateR(on);
        std::cout << "\n--zone-state " << opt.zone_state << "  aiSetZoneTrackStateR -> rc=" << rc << "\n";
        printReadback(dev);
    }
    if (opt.zone_limits) {
        std::cout << "\n--zone-limits\n";
        std::cout << "    aiSetLimitedZoneTrackYawMinR(" << opt.yaw_min << ") -> rc="
                  << dev->aiSetLimitedZoneTrackYawMinR(opt.yaw_min) << "\n";
        std::cout << "    aiSetLimitedZoneTrackYawMaxR(" << opt.yaw_max << ") -> rc="
                  << dev->aiSetLimitedZoneTrackYawMaxR(opt.yaw_max) << "\n";
        std::cout << "    aiSetLimitedZoneTrackPitchMinR(" << opt.pitch_min << ") -> rc="
                  << dev->aiSetLimitedZoneTrackPitchMinR(opt.pitch_min) << "\n";
        std::cout << "    aiSetLimitedZoneTrackPitchMaxR(" << opt.pitch_max << ") -> rc="
                  << dev->aiSetLimitedZoneTrackPitchMaxR(opt.pitch_max) << "\n";
        printZone(dev);
    }
    if (!opt.zone.empty()) {
        bool on = false;
        if (!parseOnOff(opt.zone, on)) return false;
        const int32_t rc = dev->aiSetLimitedZoneTrackEnabledR(on);
        std::cout << "\n--zone " << opt.zone << "  aiSetLimitedZoneTrackEnabledR -> rc=" << rc << "\n";
        printZone(dev);
        printReadback(dev);
    }
    if (opt.zone_read) {
        std::cout << "\n--zone-read\n";
        printZone(dev);
    }
    const auto H = Device::DevControlTargetTypeHuman;
    if (opt.czone_limits) {
        std::cout << "\n--czone-limits\n";
        std::cout << "    LimitPanMin(" << opt.pan_min << ") rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeLimitPanMin, opt.pan_min) << "\n";
        std::cout << "    LimitPanMax(" << opt.pan_max << ") rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeLimitPanMax, opt.pan_max) << "\n";
        std::cout << "    LimitPitchMin(" << opt.cpitch_min << ") rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeLimitPitchMin, opt.cpitch_min) << "\n";
        std::cout << "    LimitPitchMax(" << opt.cpitch_max << ") rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeLimitPitchMax, opt.cpitch_max) << "\n";
        printCtrlZone(dev);
    }
    if (!opt.czone_autosel.empty()) {
        bool on = false;
        if (!parseOnOff(opt.czone_autosel, on)) return false;
        std::cout << "\n--czone-autoselect " << opt.czone_autosel << "  rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeLimitAutoSelection, on) << "\n";
        printCtrlZone(dev);
    }
    if (!opt.czone.empty()) {
        bool on = false;
        if (!parseOnOff(opt.czone, on)) return false;
        const int tracker = on ? Device::DevTrackTypeLimitArea : Device::DevTrackTypeNormal;
        std::cout << "\n--czone " << opt.czone << "  TrackerType(" << tracker << ") rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeTrackerType, tracker) << "\n";
        printCtrlZone(dev);
        printReadback(dev);
    }
    if (opt.czone_read) {
        std::cout << "\n--czone-read\n";
        printCtrlZone(dev);
    }
    if (opt.cspeed >= 0) {
        std::cout << "\n--cspeed " << opt.cspeed << "  rc="
                  << dev->aiSetControlParaR(H, Device::DevControlParaTypeGimCtrlSpeedMode, opt.cspeed)
                  << "\n";
        printCtrlSpeed(dev);
    }
    if (opt.cspeed_read) {
        std::cout << "\n--cspeed-read\n";
        printCtrlSpeed(dev);
    }
    if (!opt.hdr.empty()) {
        bool on = false;
        if (!parseOnOff(opt.hdr, on)) return false;
        std::vector<int32_t> wdr_list;
        int32_t rc = dev->cameraGetWdrListR(wdr_list);
        std::cout << "\n--hdr " << opt.hdr << "\n    cameraGetWdrListR rc=" << rc << " list=[";
        for (size_t k = 0; k < wdr_list.size(); ++k) std::cout << (k ? "," : "") << wdr_list[k];
        std::cout << "]\n";
        rc = dev->cameraSetWdrR(on ? Device::DevWdrModeDol2TO1 : Device::DevWdrModeNone);
        std::cout << "    cameraSetWdrR(" << (on ? 1 : 0) << ") -> rc=" << rc << "\n";
        int32_t mode = -1;
        rc = dev->cameraGetWdrR(mode);
        std::cout << "    cameraGetWdrR rc=" << rc << " wdr_mode=" << mode << "\n";
        printReadback(dev);
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options opt;
    int exit_code = 0;
    if (!parseArgs(argc, argv, opt, exit_code)) {
        return exit_code;
    }

    std::cout << "obsbot-sdk-probe: SDK lib version " << get_dll_ver() << "\n";

    // Devices::get() is a global singleton; first access starts the SDK's
    // background USB detection task.
    Devices &devices = Devices::get();

    // Keep the spike focused on USB. mDNS/network scan is off unless asked.
    devices.setEnableMdnsScan(opt.enable_mdns);

    std::cout << "Waiting " << opt.wait_ms << " ms for USB discovery"
              << (opt.enable_mdns ? " (mDNS enabled)" : "") << " ...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.wait_ms));

    std::list<std::shared_ptr<Device>> dev_list = devices.getDevList();

    if (dev_list.empty()) {
        std::cerr << "\nERROR: no OBSBOT device found.\n"
                     "Diagnostics:\n"
                     "  - Is the Tiny 3 connected via USB and powered?\n"
                     "  - Try a longer settle time: --wait-ms 6000\n"
                     "  - Check `lsusb` for an OBSBOT / Remo entry.\n"
                     "  - USB control may need udev permissions; try running\n"
                     "    with sufficient privileges or add a udev rule.\n";
        return 1;
    }

    std::cout << "\nFound " << dev_list.size()
              << " OBSBOT device(s):\n";

    int index = 0;
    bool any_tiny3 = false;
    for (const auto &dev : dev_list) {
        printDevice(dev, index++);
        if (dev->productType() == ObsbotProdTiny3) {
            any_tiny3 = true;
        }
    }

    // Select the device to act on: --sn if given, else the first one.
    std::shared_ptr<Device> target;
    if (!opt.sn.empty()) {
        target = devices.getDevBySn(opt.sn);
        if (!target) {
            std::cerr << "\nERROR: no device with SN '" << opt.sn
                      << "' is connected.\n";
            return 1;
        }
    } else {
        target = dev_list.front();
    }

    const bool want_command =
        opt.status || opt.wake || opt.sleep || opt.center || opt.anyProbe();
    if (want_command) {
        std::cout << "\nActing on device: " << target->devName()
                  << " (SN " << target->devSn() << ")\n";
        target->setDevStatusCallbackFunc(&onStatusPush, nullptr);
        target->enableDevStatusCallback(true);
    }

    if (opt.status) {
        printStatus(target);
    }

    // Safe command path. No movement loops, no aggressive motion.
    if (opt.wake) {
        int32_t rc = target->cameraSetDevRunStatusR(Device::DevStatusRun);
        std::cout << "  --wake  cameraSetDevRunStatusR(Run)  -> rc=" << rc << "\n";
    }
    if (opt.sleep) {
        int32_t rc = target->cameraSetDevRunStatusR(Device::DevStatusSleep);
        std::cout << "  --sleep cameraSetDevRunStatusR(Sleep) -> rc=" << rc << "\n";
    }
    if (opt.center) {
        // gimbalRstPosR() moves the gimbal back to the zero/home position.
        // This is a single bounded reset, not a continuous motion command.
        int32_t rc = target->gimbalRstPosR();
        std::cout << "  --center gimbalRstPosR()             -> rc=" << rc << "\n";
    }

    if (opt.anyProbe() && !runProbes(opt, target)) {
        devices.close();
        return 2;
    }

    std::cout << "\nTiny 3 detected: " << (any_tiny3 ? "YES" : "no") << "\n";

    // Release SDK resources / stop the detection task cleanly.
    devices.close();
    return 0;
}

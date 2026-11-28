// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceList.h"

#include "AppSettings.h"
#include "platform/AndroidTools.h"
#include "platform/IosProject.h"
#include "util/CommandRunner.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::platform {

namespace fs = std::filesystem;

namespace {

    std::vector<std::string> deviceWords(const std::string& line) {
        std::istringstream in(line);
        std::vector<std::string> out;
        for (std::string word; in >> word;) out.push_back(word);
        return out;
    }

    std::string deviceTrimmed(std::string text) {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
        size_t start = 0;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) start++;
        return text.substr(start);
    }

    std::vector<std::string> deviceLines(const std::string& text) {
        std::vector<std::string> out;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) out.push_back(deviceTrimmed(line));
        return out;
    }

    // "com.apple.CoreSimulator.SimRuntime.iOS-17-2" -> "17.2"; "" for another OS
    std::string deviceIosRuntimeVersion(const std::string& runtime) {
        const std::string marker = "SimRuntime.iOS-";
        const size_t at = runtime.find(marker);
        if (at == std::string::npos) return "";
        std::string version = runtime.substr(at + marker.size());
        std::replace(version.begin(), version.end(), '-', '.');
        return version;
    }

    std::vector<RunDevice> deviceProbeAndroid(unsigned int minSdk, int timeoutMs) {
        const ToolLocation adb = adbLocation();
        if (!adb.found()) return {missingAdbRow()};
        std::string output;
        if (CommandRunner::captureResult({adb.path.string(), "devices", "-l"}, output, timeoutMs) != 0) {
            RunDevice row = missingAdbRow();
            row.name = "adb is not answering";
            row.why = "adb devices failed: restart adb (adb kill-server), then Refresh";
            return {row};
        }
        std::vector<RunDevice> devices = parseAdbDevicesLong(output);
        for (RunDevice& device : devices) {
            if (device.state != "device") continue;
            std::string props;
            // one shell call for the three properties
            if (CommandRunner::captureResult({adb.path.string(), "-s", device.id, "shell",
                                              "getprop ro.product.model;getprop ro.product.cpu.abilist;getprop ro.build.version.sdk"},
                                             props, timeoutMs) == 0)
                applyAndroidProperties(device, props, minSdk);
        }
        return devices;
    }

    std::vector<RunDevice> deviceProbeIos(int timeoutMs) {
        RunDevice none;
        none.id = "ios-simulators";
        none.platform = BuildPlatform::IOS;
        none.kind = RunDeviceKind::Simulator;
        none.name = "iOS Simulators need a Mac with Xcode";
        none.state = "unavailable";
        none.why = "iOS Simulators need macOS with Xcode";
#ifdef __APPLE__
        const XcodeChoice xcode = findXcode();
        if (!xcode.found()) {
            none.why = "iOS Simulators need macOS with Xcode: " + xcode.missing;
            return {none};
        }
        CommandRunner::Environment env;
        const CommandRunner::Environment* environment = nullptr;
        if (xcode.throughEnvironment) {
            env = CommandRunner::currentEnvironment();
            CommandRunner::setVariable(env, "DEVELOPER_DIR", xcode.developerDir.string());
            environment = &env;
        }
        std::string output;
        if (CommandRunner::captureResult({"xcrun", "simctl", "list", "devices", "available", "-j"}, output, timeoutMs, environment) != 0) {
            none.name = "simctl is not answering";
            none.why = "xcrun simctl list failed: open Xcode once to finish its setup, then Refresh";
            return {none};
        }
        std::vector<RunDevice> devices = parseSimctlJson(output);
        if (devices.empty()) {
            none.name = "No iOS Simulator";
            none.why = "add an iOS Simulator in Xcode > Window > Devices and Simulators, then Refresh";
            return {none};
        }
        return devices;
#else
        (void)timeoutMs;
        return {none};
#endif
    }

    // "Accept the prompt" -> "accept the prompt", inside a sentence
    std::string deviceLowerFirst(std::string text) {
        if (!text.empty() && text[0] >= 'A' && text[0] <= 'Z' && !(text.size() > 1 && text[1] >= 'A' && text[1] <= 'Z')) text[0] = static_cast<char>(text[0] - 'A' + 'a');
        return text;
    }

    struct DeviceProbeCache {
        std::mutex mutex;
        struct Entry {
            std::chrono::steady_clock::time_point at;
            unsigned int minSdk = 0;
            std::vector<RunDevice> devices;
        };
        std::map<int, Entry> entries;
    };

    DeviceProbeCache& deviceProbeCache() {
        static DeviceProbeCache* instance = new DeviceProbeCache();
        return *instance;
    }

}

const char* runDeviceKindName(RunDeviceKind kind) {
    switch (kind) {
        case RunDeviceKind::Host: return "host";
        case RunDeviceKind::Device: return "device";
        case RunDeviceKind::Emulator: return "emulator";
        case RunDeviceKind::Simulator: return "simulator";
        case RunDeviceKind::Browser: return "browser";
    }
    return "device";
}

std::vector<RunDevice> parseAdbDevicesLong(const std::string& output) {
    std::vector<RunDevice> devices;
    for (const std::string& line : deviceLines(output)) {
        if (line.empty() || line.rfind("List of devices", 0) == 0 || line.rfind("*", 0) == 0 || line.rfind("adb server", 0) == 0) continue;
        const std::vector<std::string> fields = deviceWords(line);
        if (fields.size() < 2) continue;
        RunDevice device;
        device.platform = BuildPlatform::Android;
        device.id = fields[0];
        device.kind = device.id.rfind("emulator-", 0) == 0 ? RunDeviceKind::Emulator : RunDeviceKind::Device;
        size_t next = 2;
        device.state = fields[1];
        // "no permissions (user in plugdev group; ...)": the state is two words, the rest its reason
        if (device.state == "no" && fields.size() > 2 && fields[2] == "permissions") {
            device.state = "no permissions";
            next = fields.size();
        }
        for (size_t i = next; i < fields.size(); i++) {
            const std::string& field = fields[i];
            if (field.rfind("model:", 0) == 0) {
                device.name = field.substr(6);
                std::replace(device.name.begin(), device.name.end(), '_', ' ');
            }
        }
        if (device.name.empty()) device.name = device.id;
        if (device.state == "device") {
            device.usable = true;
        } else if (device.state == "unauthorized") {
            device.why = "Accept the USB debugging prompt on the device";
        } else if (device.state == "offline") {
            device.why = "Reconnect the device or restart adb (adb kill-server)";
        } else if (device.state == "no permissions") {
            device.why = "adb may not open this USB device: add a udev rule for it (developer.android.com/studio/run/device), then Refresh";
        } else {
            device.why = "the device is in the " + device.state + " state: start Android on it, then Refresh";
        }
        devices.push_back(device);
    }
    return devices;
}

void applyAndroidProperties(RunDevice& device, const std::string& output, unsigned int minSdk) {
    const std::vector<std::string> values = deviceLines(output);
    if (values.size() > 0 && !values[0].empty()) device.name = values[0];
    if (values.size() > 1) {
        device.abis.clear();
        std::stringstream list(values[1]);
        for (std::string abi; std::getline(list, abi, ',');)
            if (!deviceTrimmed(abi).empty()) device.abis.push_back(deviceTrimmed(abi));
    }
    if (values.size() > 2) {
        device.osVersion = values[2];
        device.apiLevel = std::atoi(values[2].c_str());
    }
    if (device.apiLevel > 0 && minSdk > 0 && static_cast<unsigned int>(device.apiLevel) < minSdk) {
        device.usable = false;
        device.why = "Android " + std::to_string(device.apiLevel) + " is older than Minimum API Level " + std::to_string(minSdk);
    }
}

std::vector<RunDevice> parseSimctlJson(const std::string& text) {
    std::vector<RunDevice> devices;
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("devices") || !root["devices"].is_object()) return devices;
    for (const auto& [runtime, list] : root["devices"].items()) {
        const std::string version = deviceIosRuntimeVersion(runtime);
        if (version.empty() || !list.is_array()) continue;
        for (const nlohmann::json& entry : list) {
            if (!entry.is_object()) continue;
            if (!entry.value("isAvailable", true) || entry.contains("availabilityError")) continue;
            RunDevice device;
            device.platform = BuildPlatform::IOS;
            device.kind = RunDeviceKind::Simulator;
            device.id = entry.value("udid", "");
            device.name = entry.value("name", device.id);
            device.state = entry.value("state", "Shutdown");
            device.osVersion = version;
            device.usable = !device.id.empty();
            if (!device.usable) device.why = "simctl lists it without a UDID";
            devices.push_back(device);
        }
    }
    // booted first, then by name
    std::stable_sort(devices.begin(), devices.end(), [](const RunDevice& a, const RunDevice& b) {
        if ((a.state == "Booted") != (b.state == "Booted")) return a.state == "Booted";
        return a.name < b.name;
    });
    return devices;
}

ToolLocation adbLocation() {
    const std::string hint = "SDK Manager package \"platform-tools\"";
    ToolLocation location{"adb", {}, "", hint};
    const std::string configured = AppSettings::getExternalToolPath("adb");
    std::error_code ec;
    if (!configured.empty() && fs::is_regular_file(configured, ec)) {
        location.path = configured;
        location.source = "preferences";
        return location;
    }
    const fs::path sdk = AndroidTools::sdkRoot();
    if (!sdk.empty()) {
#ifdef _WIN32
        const fs::path candidate = sdk / "platform-tools" / "adb.exe";
#else
        const fs::path candidate = sdk / "platform-tools" / "adb";
#endif
        if (fs::is_regular_file(candidate, ec)) {
            location.path = candidate;
            location.source = "Android SDK";
            return location;
        }
    }
    location.path = ExternalTools::findOnPath("adb");
    if (location.found()) location.source = "PATH";
    return location;
}

RunDevice missingAdbRow() {
    RunDevice row;
    row.id = "adb";
    row.platform = BuildPlatform::Android;
    row.name = "adb not found";
    row.state = "unavailable";
    row.why = "Install the Android SDK Platform-Tools (SDK Manager package \"platform-tools\"), or set Preferences > External Tools > adb";
    return row;
}

RunDevice hostDevice() {
    RunDevice host;
    host.id = "host";
    host.platform = hostBuildPlatform();
    host.kind = RunDeviceKind::Host;
    host.name = "This computer";
    host.state = "ready";
    host.usable = true;
    return host;
}

RunDevice browserDevice() {
    RunDevice browser;
    browser.id = "web";
    browser.platform = BuildPlatform::Web;
    browser.kind = RunDeviceKind::Browser;
    browser.name = "Default browser";
    browser.state = "ready";
    browser.usable = true;
    return browser;
}

std::vector<RunDevice> probeDevices(BuildPlatform platform, unsigned int androidMinSdk, int timeoutMs) {
    switch (platform) {
        case BuildPlatform::Android: return deviceProbeAndroid(androidMinSdk, timeoutMs);
        case BuildPlatform::IOS: return deviceProbeIos(timeoutMs);
        case BuildPlatform::Web: return {browserDevice()};
        default: break;
    }
    if (platform == hostBuildPlatform()) return {hostDevice()};
    return {};
}

std::vector<RunDevice> devicesOf(BuildPlatform platform, unsigned int androidMinSdk, int maxAgeMs) {
    DeviceProbeCache& probes = deviceProbeCache();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(probes.mutex);
        const auto it = probes.entries.find(static_cast<int>(platform));
        if (it != probes.entries.end() && it->second.minSdk == androidMinSdk &&
            now - it->second.at < std::chrono::milliseconds(maxAgeMs))
            return it->second.devices;
    }
    std::vector<RunDevice> devices = probeDevices(platform, androidMinSdk);
    std::lock_guard<std::mutex> lock(probes.mutex);
    probes.entries[static_cast<int>(platform)] = {std::chrono::steady_clock::now(), androidMinSdk, devices};
    return devices;
}

const RunDevice* pickDevice(const std::vector<RunDevice>& devices, BuildPlatform platform, const std::string& id, std::string& error) {
    const std::string platformName = platform == BuildPlatform::Android ? "Android" : platform == BuildPlatform::IOS ? "iOS Simulator"
                                   : buildPlatformName(platform);
    if (id == "auto" || id.empty()) {
        std::vector<const RunDevice*> usable;
        for (const RunDevice& device : devices)
            if (device.usable) usable.push_back(&device);
        if (usable.size() == 1) return usable.front();
        if (usable.empty()) {
            if (platform == BuildPlatform::Android) {
                const bool noAdb = devices.size() == 1 && devices.front().id == "adb";
                error = noAdb ? "adb is not installed: " + devices.front().why
                              : "no Android device is connected: connect one with USB debugging on (get_build_info devices:true lists them)";
                for (const RunDevice& device : devices)
                    if (!device.usable && device.id != "adb") error = device.name + " is " + device.state + ": " + deviceLowerFirst(device.why);
            } else if (platform == BuildPlatform::IOS) {
                error = devices.empty() ? "iOS Simulators need macOS with Xcode" : devices.front().why;
            } else {
                error = "no " + platformName + " device to run on";
            }
            return nullptr;
        }
        error = std::to_string(usable.size()) + " " + platformName + " devices are usable: name one (get_build_info devices:true lists them)";
        return nullptr;
    }
    for (const RunDevice& device : devices) {
        if (device.id != id) continue;
        if (!device.usable) {
            error = device.state == "device" || device.state == "Booted" || device.state == "Shutdown"
                        ? device.name + ": " + device.why
                        : device.name + " is " + device.state + ": " + deviceLowerFirst(device.why);
            return nullptr;
        }
        return &device;
    }
    error = "no " + platformName + " device " + id + " is connected (get_build_info devices:true lists them)";
    return nullptr;
}

}  // namespace jasperengine::editor::platform

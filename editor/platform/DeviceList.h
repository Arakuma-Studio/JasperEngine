// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Build And Run's Run Device list (docs/specs/build-and-run-on-device.md R1, R2): Android devices
// and running emulators (adb devices -l), iOS Simulators (xcrun simctl, a Mac with Xcode), the
// default browser (Web) and this computer. Probing spawns processes, so it runs off the main
// thread with a 2 s limit per process, and a result younger than 2 s is reused.

#include "ShippingTypes.h"
#include "platform/ExternalTools.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    enum class RunDeviceKind { Host, Device, Emulator, Simulator, Browser };
    // "host" "device" "emulator" "simulator" "browser"
    const char* runDeviceKindName(RunDeviceKind kind);

    struct RunDevice {
        std::string id;                 // adb serial, simulator UDID, "web", "host"
        BuildPlatform platform = BuildPlatform::Android;
        RunDeviceKind kind = RunDeviceKind::Device;
        std::string name;
        std::string state;              // adb's (device, unauthorized, offline), simctl's (Booted, Shutdown), "ready"
        std::vector<std::string> abis;  // Android: ro.product.cpu.abilist
        std::string osVersion;          // Android: the API level; iOS: the runtime's version ("17.2")
        int apiLevel = 0;
        bool usable = false;
        std::string why;                // what makes it usable when it is not
    };

    // `adb devices -l`: every listed device, its state and the model adb reports (underscores as
    // spaces); usable only in the `device` state, with what to do otherwise
    std::vector<RunDevice> parseAdbDevicesLong(const std::string& output);
    // the output of `getprop ro.product.model; getprop ro.product.cpu.abilist; getprop
    // ro.build.version.sdk` (one line each) into the device; an API below minSdk is unusable
    void applyAndroidProperties(RunDevice& device, const std::string& output, unsigned int minSdk);
    // `xcrun simctl list devices available -j`: iOS simulators of available runtimes, booted or not
    std::vector<RunDevice> parseSimctlJson(const std::string& json);

    // Preferences > External Tools > adb, else <Android SDK>/platform-tools/adb, else PATH
    ToolLocation adbLocation();
    // the one unusable row when adb is missing
    RunDevice missingAdbRow();

    RunDevice hostDevice();
    RunDevice browserDevice();

    // The devices of a platform, probed now (each process limited to timeoutMs): Android: adb;
    // iOS: simctl on a Mac with Xcode, else one unusable row; Web: the browser; the host's
    // platform: this computer. Blocks: call it off the main thread.
    std::vector<RunDevice> probeDevices(BuildPlatform platform, unsigned int androidMinSdk, int timeoutMs = 2000);

    // probeDevices, reusing a result of the same platform younger than maxAgeMs (get_build_info,
    // the CLI); thread-safe
    std::vector<RunDevice> devicesOf(BuildPlatform platform, unsigned int androidMinSdk, int maxAgeMs = 2000);

    // `id`, "auto" (the only usable device of the platform) or "web" among `devices`; nullptr with
    // the error naming the fix when there is none
    const RunDevice* pickDevice(const std::vector<RunDevice>& devices, BuildPlatform platform, const std::string& id, std::string& error);

}

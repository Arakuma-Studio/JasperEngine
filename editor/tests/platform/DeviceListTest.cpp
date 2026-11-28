// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Build And Run's Run Device list and device commands (docs/specs/build-and-run-on-device.md R1,
// R6, R8, R16; AC1, AC2): adb's and simctl's listings parsed, the shell quoting of run-as
// commands, the patch plan and the patch decision.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "platform/DeviceCommands.h"
#include "platform/DeviceList.h"
#include "platform/DeviceRecords.h"
#include "platform/DeviceRunner.h"
#include "support/TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace device_list_test {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
using jasperengine::test::TestTempDir;


    // A POSIX sh's word splitting of one command line: quotes (' and "), backslashes, spaces.
    // Enough to check what the device's shell makes of the strings the editor sends.
    std::vector<std::string> shWords(const std::string& line) {
        std::vector<std::string> words;
        std::string word;
        bool inWord = false;
        for (size_t i = 0; i < line.size(); i++) {
            const char c = line[i];
            if (c == '\'') {
                const size_t end = line.find('\'', i + 1);
                word += line.substr(i + 1, end - i - 1);
                i = end;
                inWord = true;
            } else if (c == '"') {
                for (i++; i < line.size() && line[i] != '"'; i++) {
                    if (line[i] == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\' || line[i + 1] == '$')) i++;
                    word += line[i];
                }
                inWord = true;
            } else if (c == '\\' && i + 1 < line.size()) {
                word += line[++i];
                inWord = true;
            } else if (c == ' ') {
                if (inWord) words.push_back(word);
                word.clear();
                inWord = false;
            } else {
                word += c;
                inWord = true;
            }
        }
        if (inWord) words.push_back(word);
        return words;
    }

    // a variable of this process ("" unsets it)
    void setVariable(const char* name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        if (value.empty()) unsetenv(name);
        else setenv(name, value.c_str(), 1);
#endif
    }

    player::ManifestFile file(const std::string& sha, uint64_t size) {
        player::ManifestFile f;
        f.sha256 = sha;
        f.size = size;
        return f;
    }

}

namespace device_list_test {

using namespace device_list_test;

TEST(DeviceList, ParsesAdbDevicesLong) {
    const std::string output =
        "* daemon started successfully\n"
        "List of devices attached\n"
        "emulator-5554          device product:sdk_gphone64_x86_64 model:sdk_gphone64_x86_64 device:emu64xa transport_id:1\n"
        "R58M123ABC             unauthorized usb:1-1 transport_id:2\r\n"
        "0123456789ABCDEF       offline transport_id:3\n"
        "192.168.1.5:5555       device product:panther model:Pixel_7_Pro device:panther transport_id:4\n"
        "\n";
    const std::vector<RunDevice> devices = parseAdbDevicesLong(output);
    ASSERT_EQ(devices.size(), 4u);

    EXPECT_EQ(devices[0].id, "emulator-5554");
    EXPECT_EQ(devices[0].kind, RunDeviceKind::Emulator);
    EXPECT_EQ(devices[0].state, "device");
    EXPECT_TRUE(devices[0].usable);
    EXPECT_EQ(devices[0].platform, BuildPlatform::Android);

    EXPECT_EQ(devices[1].id, "R58M123ABC");
    EXPECT_EQ(devices[1].kind, RunDeviceKind::Device);
    EXPECT_EQ(devices[1].state, "unauthorized");
    EXPECT_FALSE(devices[1].usable);
    EXPECT_EQ(devices[1].why, "Accept the USB debugging prompt on the device");

    EXPECT_EQ(devices[2].state, "offline");
    EXPECT_FALSE(devices[2].usable);
    EXPECT_EQ(devices[2].why, "Reconnect the device or restart adb (adb kill-server)");

    // adb writes a model's spaces as underscores
    EXPECT_EQ(devices[3].name, "Pixel 7 Pro");
    EXPECT_TRUE(devices[3].usable);

    // the properties' model keeps its spaces; its ABIs and API level
    RunDevice pixel = devices[3];
    applyAndroidProperties(pixel, "Pixel 7 Pro\r\narm64-v8a,armeabi-v7a,armeabi\r\n34\r\n", 21);
    EXPECT_EQ(pixel.name, "Pixel 7 Pro");
    EXPECT_EQ(pixel.abis, (std::vector<std::string>{"arm64-v8a", "armeabi-v7a", "armeabi"}));
    EXPECT_EQ(pixel.apiLevel, 34);
    EXPECT_EQ(pixel.osVersion, "34");
    EXPECT_TRUE(pixel.usable);

    // no permission for a USB device: two words of state, unusable with what to do
    const std::vector<RunDevice> denied = parseAdbDevicesLong("List of devices attached\n0123 no permissions (user in plugdev group); see [http://developer.android.com/tools/device.html] usb:1-2\n");
    ASSERT_EQ(denied.size(), 1u);
    EXPECT_EQ(denied[0].state, "no permissions");
    EXPECT_FALSE(denied[0].usable);
    EXPECT_FALSE(denied[0].why.empty());

    EXPECT_TRUE(parseAdbDevicesLong("List of devices attached\n\n").empty());
}

TEST(DeviceList, ParsesSimctlJson) {
    const std::string json = R"JSON({
      "devices" : {
        "com.apple.CoreSimulator.SimRuntime.iOS-17-2" : [
          {"udid" : "A1B2", "isAvailable" : true, "state" : "Shutdown", "name" : "iPhone 15"},
          {"udid" : "C3D4", "isAvailable" : true, "state" : "Booted", "name" : "iPad Air (5th generation)"}
        ],
        "com.apple.CoreSimulator.SimRuntime.iOS-16-0" : [
          {"udid" : "DEAD", "isAvailable" : false, "state" : "Shutdown", "name" : "iPhone 14",
           "availabilityError" : "runtime profile not found"}
        ],
        "com.apple.CoreSimulator.SimRuntime.watchOS-10-2" : [
          {"udid" : "W1", "isAvailable" : true, "state" : "Shutdown", "name" : "Apple Watch Series 9 (45mm)"}
        ]
      }
    })JSON";
    const std::vector<RunDevice> devices = parseSimctlJson(json);
    ASSERT_EQ(devices.size(), 2u) << "the unavailable runtime and the watch are left out";
    // booted first
    EXPECT_EQ(devices[0].id, "C3D4");
    EXPECT_EQ(devices[0].state, "Booted");
    EXPECT_EQ(devices[0].name, "iPad Air (5th generation)");
    EXPECT_EQ(devices[1].id, "A1B2");
    EXPECT_EQ(devices[1].state, "Shutdown");
    for (const RunDevice& device : devices) {
        EXPECT_EQ(device.platform, BuildPlatform::IOS);
        EXPECT_EQ(device.kind, RunDeviceKind::Simulator);
        EXPECT_EQ(device.osVersion, "17.2");
        EXPECT_TRUE(device.usable);
    }
    EXPECT_TRUE(parseSimctlJson("not json").empty());
}

TEST(DeviceList, FlagsAnApiBelowMinimumSdk) {
    RunDevice device = parseAdbDevicesLong("List of devices attached\nold1 device model:Nexus_5 transport_id:9\n").front();
    applyAndroidProperties(device, "Nexus 5\narmeabi-v7a,armeabi\n19\n", 21);
    EXPECT_FALSE(device.usable);
    EXPECT_EQ(device.why, "Android 19 is older than Minimum API Level 21");
    std::string error;
    const std::vector<RunDevice> devices = {device};
    EXPECT_EQ(pickDevice(devices, BuildPlatform::Android, "old1", error), nullptr);
    EXPECT_EQ(error, "Nexus 5: Android 19 is older than Minimum API Level 21");
}

TEST(DeviceList, MissingAdbIsOneUnusableRow) {
    // no preference, no SDK, nothing on PATH
    const std::string savedAdb = AppSettings::getExternalToolPath("adb");
    const std::string savedSdk = AppSettings::getExternalToolPath("androidSdk");
    const char* path = std::getenv("PATH");
    const std::string savedPath = path ? path : "";
    const char* home = std::getenv("ANDROID_HOME");
    const std::string savedHome = home ? home : "";
    const char* root = std::getenv("ANDROID_SDK_ROOT");
    const std::string savedRoot = root ? root : "";
    TestTempDir empty("no-adb");
    AppSettings::setExternalToolPath("adb", "");
    AppSettings::setExternalToolPath("androidSdk", "");
    const char* home2 = std::getenv("HOME");
    const std::string savedUserHome = home2 ? home2 : "";
    setVariable("PATH", empty.path().string());
    setVariable("ANDROID_HOME", "");
    setVariable("ANDROID_SDK_ROOT", "");
    // (the SDK's default place under HOME)
    setVariable("HOME", empty.path().string());
    const std::vector<RunDevice> devices = probeDevices(BuildPlatform::Android, 21, 2000);
    setVariable("PATH", savedPath);
    setVariable("ANDROID_HOME", savedHome);
    setVariable("ANDROID_SDK_ROOT", savedRoot);
    setVariable("HOME", savedUserHome);
    AppSettings::setExternalToolPath("adb", savedAdb);
    AppSettings::setExternalToolPath("androidSdk", savedSdk);

    ASSERT_EQ(devices.size(), 1u);
    EXPECT_FALSE(devices[0].usable);
    EXPECT_NE(devices[0].why.find("platform-tools"), std::string::npos) << devices[0].why;
    std::string error;
    EXPECT_EQ(pickDevice(devices, BuildPlatform::Android, "auto", error), nullptr);
    EXPECT_NE(error.find("platform-tools"), std::string::npos) << error;

    // the browser and this computer are always there
    EXPECT_TRUE(probeDevices(BuildPlatform::Web, 21).front().usable);
    EXPECT_EQ(probeDevices(hostBuildPlatform(), 21).front().kind, RunDeviceKind::Host);
}

TEST(DeviceList, PicksTheOnlyUsableDeviceOrNamesTheFix) {
    std::vector<RunDevice> devices = parseAdbDevicesLong(
        "List of devices attached\nemulator-5554 device model:sdk transport_id:1\nR58 unauthorized usb:1 transport_id:2\n");
    std::string error;
    const RunDevice* only = pickDevice(devices, BuildPlatform::Android, "auto", error);
    ASSERT_NE(only, nullptr) << error;
    EXPECT_EQ(only->id, "emulator-5554");
    EXPECT_EQ(pickDevice(devices, BuildPlatform::Android, "R58", error), nullptr);
    EXPECT_EQ(error, "R58 is unauthorized: accept the USB debugging prompt on the device");
    EXPECT_EQ(pickDevice({}, BuildPlatform::Android, "auto", error), nullptr);
    EXPECT_EQ(error, "no Android device is connected: connect one with USB debugging on (get_build_info devices:true lists them)");
    EXPECT_EQ(pickDevice(devices, BuildPlatform::Android, "emulator-9999", error), nullptr);
    EXPECT_NE(error.find("get_build_info devices:true"), std::string::npos) << error;
}

TEST(DeviceCommands, QuotesRunAsPathsWithSpacesAndQuotes) {
    const std::string path = "assets/My \"Hero\".png";
    const std::string tricky = "lua/it's $HOME `x` \xC3\xA9t\xC3\xA9.lua";
    const std::string pushed = devicecmd::remotePatchDir("com.example.game", 3);
    EXPECT_EQ(pushed, "/data/local/tmp/jasper/com.example.game/patch-3");
    const std::string script = devicecmd::patchScript(pushed, {path, tricky}, false);
    const std::vector<std::string> argv = devicecmd::runAs("/sdk/platform-tools/adb", "emulator-5554", "com.example.game", script);

    // the argument vector: adb, the serial, shell, and ONE string for the device's shell
    ASSERT_EQ(argv.size(), 5u);
    EXPECT_EQ(argv[0], "/sdk/platform-tools/adb");
    EXPECT_EQ(argv[1], "-s");
    EXPECT_EQ(argv[2], "emulator-5554");
    EXPECT_EQ(argv[3], "shell");

    // the device's shell: run-as, the package, sh, -c and the script, unchanged
    const std::vector<std::string> deviceWords = shWords(argv[4]);
    ASSERT_EQ(deviceWords.size(), 5u) << argv[4];
    EXPECT_EQ(deviceWords[0], "run-as");
    EXPECT_EQ(deviceWords[1], "com.example.game");
    EXPECT_EQ(deviceWords[2], "sh");
    EXPECT_EQ(deviceWords[3], "-c");
    EXPECT_EQ(deviceWords[4], script);

    // sh -c: the rm names both files exactly, inside the development folder
    const std::vector<std::string> scriptWords = shWords(script);
    const auto rm = std::find(scriptWords.begin(), scriptWords.end(), "rm");
    ASSERT_NE(rm, scriptWords.end()) << script;
    ASSERT_GE(scriptWords.end() - rm, 4);
    EXPECT_EQ(*(rm + 1), "-f");
    EXPECT_EQ(*(rm + 2), "files/je-dev-content/assets/My \"Hero\".png");
    EXPECT_EQ(*(rm + 3), "files/je-dev-content/" + tricky);
    // the manifest is copied last
    EXPECT_EQ(scriptWords.back(), "files/je-dev-content/content-manifest.json");
    EXPECT_EQ(*(scriptWords.end() - 2), pushed + "/content-manifest.json");

    // a first patch empties the folder before anything else
    const std::vector<std::string> first = shWords(devicecmd::patchScript(pushed, {}, true));
    ASSERT_GE(first.size(), 3u);
    EXPECT_EQ(first[0], "rm");
    EXPECT_EQ(first[1], "-rf");
    EXPECT_EQ(first[2], "files/je-dev-content");
    EXPECT_EQ(shWords(devicecmd::clearScript()), (std::vector<std::string>{"rm", "-rf", "files/je-dev-content"}));

    EXPECT_EQ(devicecmd::shellQuote("it's"), "'it'\\''s'");
    EXPECT_EQ(shWords(devicecmd::shellJoin({"a b", "it's", "\"q\""})), (std::vector<std::string>{"a b", "it's", "\"q\""}));
}

TEST(DeviceCommands, WebQueryCarriesThePlayerOptions) {
    EXPECT_EQ(devicecmd::queryFromArgs({"--fixed-fps", "60", "--after-ready", "--scene", "Main Menu", "--", "--god-mode"}),
              "fixed-fps=60&after-ready&scene=Main%20Menu");
    EXPECT_EQ(devicecmd::queryFromArgs({"--device-tier=low"}), "device-tier=low");
    EXPECT_EQ(devicecmd::queryFromArgs({}), "");
}

TEST(DevicePatch, PushesOnlyWhatChangedAndDeletesWhatWentAway) {
    const std::map<std::string, player::ManifestFile> device = {
        {"game.json", file("a", 10)}, {"lua/main.lua", file("b", 20)}, {"lua/old.lua", file("c", 30)}, {"scenes/1.jscn", file("d", 40)}};
    std::map<std::string, player::ManifestFile> next = device;
    next.erase("lua/old.lua");
    next["lua/main.lua"] = file("b2", 25);
    next["lua/new.lua"] = file("e", 5);
    next["content-manifest.json"] = file("m", 1);
    const devicecmd::PatchPlan plan = devicecmd::planPatch(device, next, false);
    EXPECT_EQ(plan.push, (std::vector<std::string>{"lua/main.lua", "lua/new.lua"}));
    EXPECT_EQ(plan.remove, (std::vector<std::string>{"lua/old.lua"}));
    EXPECT_EQ(plan.bytes, 30u);
    EXPECT_FALSE(plan.first);

    // the first patch mirrors everything (the folder starts empty), and deletes nothing
    const devicecmd::PatchPlan first = devicecmd::planPatch(device, next, true);
    EXPECT_EQ(first.push.size(), 4u);
    EXPECT_TRUE(first.remove.empty());
    EXPECT_TRUE(first.first);
}

// the patch diff of 10,000 unchanged content files: <= 200 ms (enforced with perfSlack)
TEST(DevicePatch, DiffOf10kUnchangedFilesIsFast) {
    std::map<std::string, player::ManifestFile> files;
    for (int i = 0; i < 10000; i++) {
        char name[64];
        std::snprintf(name, sizeof(name), "assets/textures/t%05d.ktx2", i);
        files[name] = file(std::string(64, 'a' + (i % 6)), 1000u + i);
    }
    const auto start = std::chrono::steady_clock::now();
    const devicecmd::PatchPlan plan = devicecmd::planPatch(files, files, false);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(plan.push.empty());
    EXPECT_TRUE(plan.remove.empty());
    std::printf("[bench] DevicePatch diff of 10000 unchanged files: %.2f ms (budget 200 ms)\n", ms);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(ms, 200.0 * slack);
    }
}

TEST(DevicePatch, DecidesPatchOrNamesWhyNot) {
    RunDevice pixel;
    pixel.id = "emulator-5554";
    pixel.name = "Pixel 7";
    pixel.platform = BuildPlatform::Android;
    DeviceRecord record;
    record.playerId = "android-universal-gles3-development";
    record.playerSha256 = "id1";
    record.development = true;
    record.debuggable = true;
    EXPECT_TRUE(decidePatch(&record, pixel, true, record.playerId, "id1", true).patch);
    EXPECT_EQ(decidePatch(nullptr, pixel, true, record.playerId, "id1", true).why, "no install recorded on Pixel 7");
    EXPECT_EQ(decidePatch(&record, pixel, false, record.playerId, "id1", true).why, "Patch And Run needs a Development Build");
    EXPECT_NE(decidePatch(&record, pixel, true, "android-universal-vulkan-development", "id1", true).why.find("the player binary changed"),
              std::string::npos);
    EXPECT_NE(decidePatch(&record, pixel, true, record.playerId, "id2", true).why.find("the player binary changed"), std::string::npos);
    EXPECT_EQ(decidePatch(&record, pixel, true, record.playerId, "id1", false).why, "the Android player runtime predates Patch And Run: update it");
    record.debuggable = false;
    EXPECT_EQ(decidePatch(&record, pixel, true, record.playerId, "id1", true).why, "the installed package is not debuggable");
    RunDevice browser = browserDevice();
    EXPECT_FALSE(decidePatch(&record, browser, true, record.playerId, "id1", true).patch);
}

TEST(DeviceRecords, RoundTripAndIgnoreANewerFile) {
    TestTempDir dir("device-records");
    DeviceRecords records;
    records.selected["android"] = "emulator-5554";
    DeviceRecord record;
    record.name = "Pixel 7";
    record.platform = "android";
    record.package = "com.example.game";
    record.packageSha256 = std::string(64, 'f');
    record.playerId = "android-universal-gles3-development";
    record.playerSha256 = "abc";
    record.development = true;
    record.debuggable = true;
    record.patch = 2;
    record.content["lua/main.lua"] = file("b", 20);
    records.devices["emulator-5554"] = record;
    std::string error;
    ASSERT_TRUE(records.save(dir.path(), error)) << error;
    std::string warning;
    const DeviceRecords back = DeviceRecords::load(dir.path(), &warning);
    EXPECT_TRUE(warning.empty()) << warning;
    EXPECT_EQ(back.selected.at("android"), "emulator-5554");
    const DeviceRecord* read = back.find("emulator-5554");
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->package, "com.example.game");
    EXPECT_EQ(read->patch, 2);
    EXPECT_TRUE(read->debuggable);
    EXPECT_EQ(read->content.at("lua/main.lua").size, 20u);

    TestTempDir::write(DeviceRecords::file(dir.path()), "formatVersion: 2\ndevices: {}\n");
    const DeviceRecords newer = DeviceRecords::load(dir.path(), &warning);
    EXPECT_TRUE(newer.devices.empty());
    EXPECT_NE(warning.find("format 2"), std::string::npos) << warning;
    TestTempDir::write(DeviceRecords::file(dir.path()), "{{{ not yaml");
    warning.clear();
    EXPECT_TRUE(DeviceRecords::load(dir.path(), &warning).devices.empty());
    EXPECT_FALSE(warning.empty());
}

}  // namespace device_list_test

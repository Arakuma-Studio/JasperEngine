// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// .jasperengine/user/devices.yaml (docs/specs/build-and-run-on-device.md R5, R18, R28): per user,
// never committed. What each device runs of this project (the package, the player binary, the
// content and the patch counter: what Patch And Run diffs against), and the Run Device chosen per
// platform. An unreadable or newer file is ignored with one warning and rewritten.

#include "player/ContentManifest.h"

#include <filesystem>
#include <map>
#include <string>

namespace jasperengine::editor::platform {

    struct DeviceRecord {
        std::string name;                // the device's name when it was installed
        std::string platform;            // android, ios
        std::string package;             // Android package / bundle identifier
        std::string packageSha256;       // the installed .apk (the simulator .app's tree hash)
        // the player binary's identity: the runtime id and the hash of its binaries
        std::string playerId;
        std::string playerSha256;
        bool development = false;
        bool debuggable = false;         // Android: run-as reaches its files
        int patch = 0;                   // 0: the packaged content
        // what the device runs (the content manifest's files)
        std::map<std::string, player::ManifestFile> content;
    };

    struct DeviceRecords {
        static constexpr int FORMAT_VERSION = 1;

        std::map<std::string, DeviceRecord> devices;     // by device id
        std::map<std::string, std::string> selected;     // platform name -> the Run Device id

        static std::filesystem::path file(const std::filesystem::path& internalDir);
        // missing: empty; unreadable or newer: empty with `warning` set (the next save rewrites it)
        static DeviceRecords load(const std::filesystem::path& internalDir, std::string* warning = nullptr);
        bool save(const std::filesystem::path& internalDir, std::string& error) const;

        const DeviceRecord* find(const std::string& deviceId) const;
    };

}

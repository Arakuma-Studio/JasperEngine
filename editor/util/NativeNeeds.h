// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a build of one platform needs from plugins/native and the native settings
// (docs/shipping.md "Native plugins in prebuilt builds"): every input is either carried into a
// prebuilt player's build (the prebuilt path does it with the same result as the source export)
// or a reason to compile from source, named with its files. Never silently dropped.
//
//   Linux, Windows   C++ plugins compile
//   macOS            macos.plist_entries and the privacy manifest are carried; C++, Objective-C/
//                    Swift plugins, SDKs and macos.frameworks compile
//   iOS              Objective-C/Swift plugins, SDKs, Swift packages, frameworks, Info.plist
//                    entries, entitlements, deployment target and the privacy manifest are carried
//                    (the prebuilt build is an Xcode project); C++ plugins and CocoaPods compile
//   Android          extra permissions and the manifest XML are carried; C++, Java/Kotlin, AARs,
//                    resources, the plugin manifest, Gradle dependencies and tools: merge rules
//                    compile
//   Web              plugins/native/web/*.js and web.script_urls are carried; C++ compiles

#include "ShippingTypes.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct PlayerInfo;

    struct NativeNeeds {
        std::vector<std::string> compileReasons;
        std::vector<std::string> carried;
        // W-NATIVE-MOCK: plugins whose only answer on the platform is a Lua mock
        std::vector<std::string> mockWarnings;
    };

    // `player`: the runtime the build would use (null: this editor's own, which takes every carried input)
    NativeNeeds nativeNeedsFor(const Project& project, BuildPlatform platform, const PlayerInfo* player);

    // The NativePlugins::platformOnlyPlugins name of the platform a build runs on ("desktop" for
    // Linux and Windows, "macos-cmake" for macOS builds without Xcode)
    std::string nativeRunningName(BuildPlatform platform);

}

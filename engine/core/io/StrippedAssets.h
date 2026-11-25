// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A Development Build's list of the assets its build left out (content-manifest.json "stripped",
// docs/specs/S98-editor-tooling-gaps.md REQ-10): a file that does not open and is on it logs why,
// once, naming the setting that left it out. A release build carries no list, so this costs
// nothing there.

#include "Export.h"

#include <map>
#include <string>

namespace jasperengine {

    class JASPERENGINE_API StrippedAssets {
    public:
        // asset-relative path -> "unreferenced" or "excluded: <glob>" (the player sets it at start)
        static void set(std::map<std::string, std::string> paths);
        static bool empty();
        // the warning for a file that did not open (File::open); true when it was left out
        static bool explainMissing(const std::string& path);
        // "<path> was stripped from the build (Asset Stripping: Referenced Only): add it to Always Include"
        static std::string message(const std::string& path, const std::string& reason);
    };

}

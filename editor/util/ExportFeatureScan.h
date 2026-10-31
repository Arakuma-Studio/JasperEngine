// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef EXPORTFEATURESCAN_H
#define EXPORTFEATURESCAN_H

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Which engine features the code of an export uses, for what the platform needs declared
    // (Android permissions): Lua files (and the Lua of <script type="text/lua"> in HTML UI
    // documents) and C++ scripts. Shared by the http feature (INTERNET) and platform-services
    // (ACCESS_NETWORK_STATE for System.getInternetReachability): add a Feature, call scan().
    class ExportFeatureScan {
    public:
        struct Feature {
            std::string name;
            std::regex lua;     // over each Lua source
            std::regex cpp;     // over each C++ source and header
        };

        struct Use {
            std::string feature;
            std::string file;   // the first file that uses it, relative to the scanned root ("lua/net/analytics.lua")
            int files = 0;      // how many files use it
        };

        // Http: any use of the name Http in Lua (also `local H = Http`), C++ jasperengine::Http,
        // Http::, HttpRequestDesc or "net/Http.h"
        static Feature http();

        // Scans every file under `root` (hidden folders and build folders skipped). One Use per
        // feature found, in the order of `features`.
        static std::vector<Use> scan(const std::filesystem::path& root, const std::vector<Feature>& features);
        // Scans these files (relative to `root`; only .lua, HTML documents and C++ sources are read)
        static std::vector<Use> scanFiles(const std::filesystem::path& root, std::vector<std::filesystem::path> files,
                                          const std::vector<Feature>& features);
        // The files under `dir` (relative to `root`) that scan() would read
        static void collect(const std::filesystem::path& root, const std::filesystem::path& dir, std::vector<std::filesystem::path>& files);

        // The Lua of an HTML document's <script type="text/lua"> elements, one after the other
        static std::string luaOfHtml(const std::string& html);
    };

}

#endif // EXPORTFEATURESCAN_H

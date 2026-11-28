// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StrippedAssets.h"

#include "Log.h"
#include "System.h"

#include <atomic>
#include <mutex>
#include <set>

using namespace jasperengine;

namespace {

    struct StrippedAssetList {
        std::mutex mutex;
        std::map<std::string, std::string> paths;
        std::set<std::string> explained;
        std::atomic<bool> any{false};
    };

    StrippedAssetList& strippedAssetList() {
        static StrippedAssetList list;
        return list;
    }

}

void StrippedAssets::set(std::map<std::string, std::string> paths) {
    StrippedAssetList& list = strippedAssetList();
    std::lock_guard<std::mutex> lock(list.mutex);
    list.paths = std::move(paths);
    list.explained.clear();
    list.any = !list.paths.empty();
}

bool StrippedAssets::empty() {
    return !strippedAssetList().any;
}

std::string StrippedAssets::message(const std::string& path, const std::string& reason) {
    if (reason.rfind("excluded: ", 0) == 0)
        return path + " was left out of the build (Exclude From Build: " + reason.substr(10) + "): take it off that list";
    return path + " was stripped from the build (Asset Stripping: Referenced Only): add it to Always Include";
}

bool StrippedAssets::explainMissing(const std::string& path) {
    StrippedAssetList& list = strippedAssetList();
    if (!list.any) return false;
    // the forms a load names a file by: "asset://x", the asset root's absolute path, "./x"
    std::string key = path;
    if (key.rfind("asset://", 0) == 0) key = key.substr(8);
    const std::string root = System::instance().getAssetPath();
    if (!root.empty() && key.size() > root.size() + 1 && key.compare(0, root.size(), root) == 0 && key[root.size()] == '/')
        key = key.substr(root.size() + 1);
    while (key.rfind("./", 0) == 0) key = key.substr(2);
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(list.mutex);
        const auto found = list.paths.find(key);
        if (found == list.paths.end()) return false;
        if (!list.explained.insert(key).second) return true;
        reason = found->second;
    }
    Log::warn("%s", message(key, reason).c_str());
    return true;
}

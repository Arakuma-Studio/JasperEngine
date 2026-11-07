// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetClipboard.h"

namespace fs = std::filesystem;
using namespace jasperengine::editor;

void AssetClipboard::set(std::vector<fs::path> absolutePaths, bool cut) {
    items.clear();
    cutItems.clear();
    for (fs::path& path : absolutePaths) {
        items.push_back(path.lexically_normal());
    }
    this->cut = cut;
    if (cut) {
        cutItems.insert(items.begin(), items.end());
    }
}

void AssetClipboard::clear() {
    items.clear();
    cutItems.clear();
    cut = false;
}

bool AssetClipboard::isCutItem(const fs::path& absolute) const {
    return cut && !cutItems.empty() && cutItems.count(absolute.lexically_normal()) > 0;
}

std::vector<fs::path> AssetClipboard::existing(size_t* gone) const {
    std::vector<fs::path> out;
    size_t missing = 0;
    for (const fs::path& path : items) {
        std::error_code ec;
        if (fs::exists(fs::symlink_status(path, ec))) {
            out.push_back(path);
        } else {
            missing++;
        }
    }
    if (gone) *gone = missing;
    return out;
}

void AssetClipboard::pasted(bool succeeded) {
    if (succeeded && cut) {
        clear();
    }
}

fs::path AssetClipboard::pasteTarget(const fs::path& listingFolder, const fs::path* clicked, bool clickedIsDirectory) {
    if (!clicked || clicked->empty()) {
        return listingFolder;
    }
    return clickedIsDirectory ? *clicked : clicked->parent_path();
}

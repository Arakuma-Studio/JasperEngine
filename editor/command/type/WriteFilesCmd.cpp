// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WriteFilesCmd.h"

#include "util/ProjectChecks.h"

#include <fstream>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

bool writeWhole(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    out.close();
    return static_cast<bool>(out);
}

}

editor::WriteFilesCmd::WriteFilesCmd(std::vector<Item> items, std::function<void(const std::vector<fs::path>&)> afterChange)
    : items(std::move(items))
    , afterChange(std::move(afterChange)) {
}

std::vector<fs::path> editor::WriteFilesCmd::paths() const {
    std::vector<fs::path> out;
    for (const Item& item : items) out.push_back(item.path);
    return out;
}

bool editor::WriteFilesCmd::execute() {
    if (!captured) {
        previous.clear();
        for (const Item& item : items) {
            Previous prev;
            std::error_code ec;
            prev.existed = fs::is_regular_file(item.path, ec);
            if (prev.existed && !ProjectChecks::readFile(item.path, prev.content)) {
                return false;
            }
            previous.push_back(std::move(prev));
        }
        captured = true;
    }
    for (const Item& item : items) {
        std::error_code ec;
        const fs::path parent = item.path.parent_path();
        if (!fs::exists(parent, ec)) {
            // remember the outermost directory created, for undo
            fs::path missing = parent;
            while (!missing.parent_path().empty() && !fs::exists(missing.parent_path(), ec) && missing.parent_path() != missing) {
                missing = missing.parent_path();
            }
            fs::create_directories(parent, ec);
            if (ec) {
                return false;
            }
            createdDirs.push_back(missing);
        }
        if (!writeWhole(item.path, item.content)) {
            return false;
        }
    }
    if (afterChange) afterChange(paths());
    return true;
}

void editor::WriteFilesCmd::undo() {
    for (size_t i = 0; i < items.size() && i < previous.size(); i++) {
        std::error_code ec;
        if (previous[i].existed) writeWhole(items[i].path, previous[i].content);
        else fs::remove(items[i].path, ec);
    }
    for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) {
        std::error_code ec;
        if (fs::is_directory(*it, ec) && fs::is_empty(*it, ec)) fs::remove(*it, ec);
    }
    createdDirs.clear();
    if (afterChange) afterChange(paths());
}

bool editor::WriteFilesCmd::mergeWith(Command* previousCommand) {
    if (mergeKey.empty()) return false;
    auto* other = dynamic_cast<WriteFilesCmd*>(previousCommand);
    if (!other || other->mergeKey != mergeKey || other->items.size() != items.size() || other->previous.size() != items.size()) {
        return false;
    }
    for (size_t i = 0; i < items.size(); i++) {
        if (other->items[i].path != items[i].path) return false;
    }
    // undo goes back to what was there before the first edit of the run
    previous = other->previous;
    createdDirs.insert(createdDirs.begin(), other->createdDirs.begin(), other->createdDirs.end());
    return true;
}

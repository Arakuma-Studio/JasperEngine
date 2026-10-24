// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WriteFilesCmd.h"

#include <fstream>
#include <iterator>

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

bool readWhole(const fs::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
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
            if (prev.existed && !readWhole(item.path, prev.content)) {
                return false;
            }
            previous.push_back(std::move(prev));
        }
        captured = true;
    }
    for (size_t i = 0; i < items.size(); i++) {
        const Item& item = items[i];
        std::error_code ec;
        const fs::path parent = item.path.parent_path();
        if (!fs::exists(parent, ec)) {
            // remember every directory created (outermost first), for undo
            std::vector<fs::path> missing;
            for (fs::path dir = parent; !dir.empty() && !fs::exists(dir, ec); dir = dir.parent_path()) {
                missing.insert(missing.begin(), dir);
                if (dir.parent_path() == dir) break;
            }
            fs::create_directories(parent, ec);
            if (ec) {
                restore(i);
                return false;
            }
            createdDirs.insert(createdDirs.end(), missing.begin(), missing.end());
        }
        if (!writeWhole(item.path, item.content)) {
            // all or nothing: what this command already wrote (this file included) goes back
            restore(i + 1);
            return false;
        }
    }
    if (afterChange) afterChange(paths());
    return true;
}

void editor::WriteFilesCmd::restore(size_t count) {
    for (size_t i = 0; i < count && i < items.size() && i < previous.size(); i++) {
        std::error_code ec;
        if (previous[i].existed) writeWhole(items[i].path, previous[i].content);
        else fs::remove(items[i].path, ec);
    }
    for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) {
        std::error_code ec;
        if (fs::is_directory(*it, ec) && fs::is_empty(*it, ec)) fs::remove(*it, ec);
    }
    createdDirs.clear();
}

void editor::WriteFilesCmd::undo() {
    restore(items.size());
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

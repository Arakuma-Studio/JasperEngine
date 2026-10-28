// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WriteFilesCmd.h"

#include "util/FileUtils.h"
#include "asset/AssetTypes.h"
#include "command/CommandNames.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

// the whole file, as bytes (the pose tests link this file without the editor's utilities)
bool readWhole(const fs::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
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

void editor::WriteFilesCmd::fail(const std::string& message) {
    if (failureSink) *failureSink = message;
}

bool editor::WriteFilesCmd::restore(size_t i) {
    std::error_code ec;
    if (previous[i].existed) return FileUtils::writeFileReplacing(items[i].path, previous[i].content);
    // a file it created (or began to): gone again; never a directory that was there
    if (fs::is_regular_file(items[i].path, ec) || fs::is_symlink(items[i].path, ec)) fs::remove(items[i].path, ec);
    // and the meta the asset database gave it (S04): a created file leaves no orphan behind
    const fs::path meta = AssetTypes::metaPathFor(items[i].path);
    if (!previous[i].metaExisted && fs::is_regular_file(meta, ec)) fs::remove(meta, ec);
    return !fs::exists(fs::symlink_status(items[i].path, ec));
}

void editor::WriteFilesCmd::removeCreatedDirs() {
    std::vector<fs::path> dirs = createdDirs;
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) fs::remove(dir, ec);
    }
    createdDirs.clear();
}

bool editor::WriteFilesCmd::execute() {
    if (!captured) {
        previous.clear();
        for (const Item& item : items) {
            Previous prev;
            std::error_code ec;
            prev.existed = fs::is_regular_file(item.path, ec);
            prev.metaExisted = fs::exists(fs::symlink_status(AssetTypes::metaPathFor(item.path), ec));
            if (prev.existed && !readWhole(item.path, prev.content)) {
                fail("can't read " + FileUtils::pathToGenericUtf8(item.path) + " (its content is kept for undo); nothing was written");
                return false;
            }
            previous.push_back(std::move(prev));
        }
        captured = true;
    }
    createdDirs.clear();
    // on a failure at item `failed`: items [0, touched) back to their previous state, the
    // directories created removed
    auto rollback = [&](size_t touched, const std::string& why) {
        std::string unrestored;
        for (size_t j = touched; j-- > 0;) {
            if (!restore(j)) unrestored += (unrestored.empty() ? "" : ", ") + FileUtils::pathToGenericUtf8(items[j].path);
        }
        removeCreatedDirs();
        fail(why + (unrestored.empty() ? "; nothing was changed (the files written before it were put back)"
                                       : "; and " + unrestored + " could not be put back"));
        return false;
    };
    for (size_t i = 0; i < items.size(); i++) {
        const Item& item = items[i];
        std::error_code ec;
        const fs::path parent = item.path.parent_path();
        if (!parent.empty() && !fs::exists(parent, ec)) {
            // every directory created, for undo (and a rollback)
            std::vector<fs::path> missing;
            for (fs::path dir = parent; !dir.empty() && !fs::exists(dir, ec); dir = dir.parent_path()) {
                missing.push_back(dir);
                if (dir.parent_path() == dir) break;
            }
            fs::create_directories(parent, ec);
            for (const fs::path& dir : missing) {
                std::error_code exists;
                if (fs::is_directory(dir, exists)) createdDirs.push_back(dir);
            }
            if (ec) return rollback(i, "can't create the directory " + FileUtils::pathToGenericUtf8(parent) + " for " +
                                       FileUtils::pathToGenericUtf8(item.path) + ": " + ec.message());
        }
        errno = 0;
        if (!FileUtils::writeFileReplacing(item.path, item.content)) {
            const int code = errno;
            std::string reason = code ? std::strerror(code) : std::string("write failed");
            if (fs::is_directory(item.path, ec)) reason = "it is a directory";
            // the file itself may be half written: restored too
            return rollback(i + 1, "can't write " + FileUtils::pathToGenericUtf8(item.path) + ": " + reason);
        }
    }
    if (afterChange) afterChange(paths());
    return true;
}

void editor::WriteFilesCmd::undo() {
    for (size_t i = 0; i < items.size() && i < previous.size(); i++) restore(i);
    removeCreatedDirs();
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

std::string editor::WriteFilesCmd::describe() const {
    if (!stepName.empty()) {
        return stepName;
    }
    if (items.size() == 1) {
        const bool created = !previous.empty() && !previous.front().existed;
        return std::string(created ? "Create File " : "Write File ") + CommandNames::file(items.front().path);
    }
    return "Write " + CommandNames::files(paths());
}

bool editor::WriteFilesCmd::isNoOp() const {
    if (previous.size() != items.size()) return false;
    for (size_t i = 0; i < items.size(); i++) {
        if (!previous[i].existed || previous[i].content != items[i].content) return false;
    }
    return true;
}

size_t editor::WriteFilesCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + stepName.capacity() + mergeKey.capacity();
    for (const Item& item : items) bytes += sizeof(item) + item.content.capacity() + item.path.native().size();
    for (const Previous& prev : previous) bytes += sizeof(prev) + prev.content.capacity();
    return bytes;
}

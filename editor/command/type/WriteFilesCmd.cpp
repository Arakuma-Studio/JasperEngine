// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WriteFilesCmd.h"

#include "util/FileUtils.h"
#include "asset/AssetTypes.h"
#include "command/CommandNames.h"
#include "util/SafeWrite.h"

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
    lastFailure = message;
    if (failureSink) *failureSink = message;
}

fs::path editor::WriteFilesCmd::rootOf(const fs::path& path) const {
    if (!explicitRoot.empty()) return explicitRoot;
    if (!writeRoot().empty() || !ownFolders) return rootFor(path);
    // where the write lands (a live link at the name followed), its folder the root
    const fs::path where = safewrite::writeLocation(path);
    return where.empty() ? fs::path() : safewrite::existingFolderOf(where.parent_path());
}

std::string editor::WriteFilesCmd::undoRefusal() const {
    const std::string why = Command::undoRefusal();
    return why.empty() && !explicitRoot.empty() ? escapeFor(explicitRoot, true) : why;
}

std::string editor::WriteFilesCmd::redoRefusal() const {
    const std::string why = Command::redoRefusal();
    return why.empty() && !explicitRoot.empty() ? escapeFor(explicitRoot, false) : why;
}

std::vector<editor::Command::Touched> editor::WriteFilesCmd::touchedPaths(bool undo) const {
    std::vector<Touched> out;
    for (size_t i = 0; i < items.size(); i++) {
        // an undo writes the previous bytes back (a write) or removes the file it made (the entry);
        // a redo writes each file again
        const bool existed = i < previous.size() && previous[i].existed;
        out.push_back({items[i].path, undo && !existed});
    }
    if (undo) {
        for (const fs::path& dir : createdDirs) out.push_back({dir, true});
    }
    return out;
}

bool editor::WriteFilesCmd::restore(size_t i, std::string* why) {
    std::error_code ec;
    // beneath its write root, or the active write scope's (an MCP call undoing a step the GUI made)
    const fs::path beneathRoot = rootOf(items[i].path);
    if (!beneathRoot.empty()) {
        // beneath the root: the previous bytes written back / the new file removed (the entry
        // itself), never through a link made since
        std::string error;
        // every write scope in force allows it too (an explicit root is checked against them)
        const std::string refused = safewrite::scopesRefusal(items[i].path, !previous[i].existed);
        if (!refused.empty()) {
            if (why) *why = refused;
            return false;
        }
        const bool ok = previous[i].existed ? safewrite::writeFileResolvedBeneath(beneathRoot, items[i].path, previous[i].content, error)
                                            : safewrite::removeFileResolvedBeneath(beneathRoot, items[i].path, error);
        if (!ok && why) *why = error;
        return ok;
    }
    if (previous[i].existed) return FileUtils::writeFileReplacing(items[i].path, previous[i].content);
    // a file it created (or began to): gone again; never a directory that was there
    if (fs::is_regular_file(items[i].path, ec) || fs::is_symlink(items[i].path, ec)) FileUtils::removePath(items[i].path, ec);
    // and the meta the asset database gave it (S04): a created file leaves no orphan behind
    const fs::path meta = AssetTypes::metaPathFor(items[i].path);
    if (!previous[i].metaExisted && fs::is_regular_file(meta, ec)) FileUtils::removePath(meta, ec);
    return !fs::exists(fs::symlink_status(items[i].path, ec));
}

void editor::WriteFilesCmd::removeCreatedDirs() {
    std::vector<fs::path> dirs = createdDirs;
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    for (const fs::path& dir : dirs) {
        const fs::path beneathRoot = rootOf(dir);
        if (!beneathRoot.empty()) {
            std::string ignored;
            safewrite::removeEmptyDirResolvedBeneath(beneathRoot, dir, ignored);
            continue;
        }
        std::error_code ec;
        if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) FileUtils::removePath(dir, ec);
    }
    createdDirs.clear();
}

bool editor::WriteFilesCmd::execute() {
    lastFailure.clear();
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
    failedPath.clear();
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
        const fs::path beneathRoot = rootOf(item.path);
        if (!beneathRoot.empty()) {
            // the missing folders are made by the write (and listed for undo); a failed write
            // left the file as it was
            std::string why = safewrite::scopesRefusal(item.path, false);
            if (!why.empty() || !safewrite::writeFileResolvedBeneath(beneathRoot, item.path, item.content, why, &createdDirs)) {
                failedPath = item.path;
                const bool named = why.rfind("can't ", 0) == 0;
                return rollback(i, named ? why : "can't write " + FileUtils::pathToGenericUtf8(item.path) + ": " + why);
            }
            continue;
        }
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
            if (ec) failedPath = item.path;
            if (ec) return rollback(i, "can't create the directory " + FileUtils::pathToGenericUtf8(parent) + " for " +
                                       FileUtils::pathToGenericUtf8(item.path) + ": " + ec.message());
        }
        errno = 0;
        if (!FileUtils::writeFileReplacing(item.path, item.content)) {
            const int code = errno;
            std::string reason = code ? std::strerror(code) : std::string("write failed");
            if (fs::is_directory(item.path, ec)) reason = "it is a directory";
            failedPath = item.path;
            // the file itself may be half written: restored too
            return rollback(i + 1, "can't write " + FileUtils::pathToGenericUtf8(item.path) + ": " + reason);
        }
    }
    if (afterChange) afterChange(paths());
    return true;
}

void editor::WriteFilesCmd::undo() {
    // checked first (canUndo); a restore that still fails (a link planted since) is reported
    for (size_t i = 0; i < items.size() && i < previous.size(); i++) {
        std::string why;
        if (!restore(i, &why)) {
            const std::string problem = "could not put back " + FileUtils::pathToGenericUtf8(items[i].path) + (why.empty() ? std::string() : ": " + why);
            undoProblem += (undoProblem.empty() ? "" : "; ") + problem;
        }
    }
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

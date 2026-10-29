// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveFilesCmd.h"

#include "util/FileUtils.h"

#include <cstdio>
#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

// (in a write scope: beneath its root, its folders made the same way)
bool writeWhole(const fs::path& path, const std::string& content) {
    std::error_code ec;
    editor::FileUtils::createDirectories(path.parent_path(), ec);
    return editor::FileUtils::writeFileReplacing(path, content);
}

}

editor::RemoveFilesCmd::RemoveFilesCmd(std::vector<fs::path> files, std::function<void(const std::vector<fs::path>&)> afterChange)
    : files(std::move(files))
    , afterChange(std::move(afterChange)) {
}

bool editor::RemoveFilesCmd::execute() {
    // read everything first: a missing or unreadable file removes nothing
    contents.clear();
    for (const fs::path& file : files) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return false;
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        contents.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) return false;
    }
    for (size_t i = 0; i < files.size(); i++) {
        std::error_code ec;
        if (!editor::FileUtils::removePath(files[i], ec) || ec) {
            // all or nothing: the files already removed come back
            for (size_t j = 0; j < i; j++) writeWhole(files[j], contents[j]);
            if (afterChange && i > 0) afterChange(files);
            return false;
        }
    }
    if (afterChange) afterChange(files);
    return true;
}

void editor::RemoveFilesCmd::undo() {
    bool failed = false;
    for (size_t i = 0; i < files.size() && i < contents.size(); i++) {
        if (!writeWhole(files[i], contents[i])) {
            failed = true;
            undoProblem += (undoProblem.empty() ? "" : "; ") + std::string("could not put back ") + editor::FileUtils::pathToGenericUtf8(files[i]);
        }
    }
    if (failed) {
        // Command::undo cannot fail; the history step stays (a redo removes what came back)
        printf("Error: restoring a removed file failed (permissions or a full disk)\n");
    }
    if (afterChange) afterChange(files);
}

std::vector<editor::Command::Touched> editor::RemoveFilesCmd::touchedPaths(bool undo) const {
    std::vector<Touched> out;
    // an undo writes them back, a redo removes the entries
    for (const fs::path& file : files) out.push_back({file, !undo});
    return out;
}
